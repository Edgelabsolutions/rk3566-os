// SPDX-License-Identifier: MIT
#include "encoder.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rga/RgaUtils.h>
#include <rga/im2d.hpp>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/rk_venc_cmd.h>

static long long now_us()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(
		       std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

#define CTX static_cast<MppCtx>(ctx_)
#define MPI static_cast<MppApi *>(mpi_)

Encoder::~Encoder()
{
	{
		std::lock_guard<std::mutex> l(mu_);
		stop_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
	for (int i = 0; i < 2; i++) {
		if (rga_[i])
			releasebuffer_handle(rga_[i]);
		if (bufs_[i])
			mpp_buffer_put(static_cast<MppBuffer>(bufs_[i]));
	}
	if (group_)
		mpp_buffer_group_put(static_cast<MppBufferGroup>(group_));
	if (ctx_)
		mpp_destroy(CTX);
	if (fifo_fd_ >= 0)
		close(fifo_fd_);
}

bool Encoder::init(int width, int height, int fps, int kbps, const std::string &fifo)
{
	width_ = width;
	height_ = height;
	hstride_ = (width + 15) & ~15;
	vstride_ = (height + 15) & ~15;

	MppCtx ctx = nullptr;
	MppApi *mpi = nullptr;
	if (mpp_create(&ctx, &mpi) != MPP_OK) {
		fprintf(stderr, "encoder: mpp_create failed\n");
		return false;
	}
	MppPollType block = MPP_POLL_BLOCK;
	mpi->control(ctx, MPP_SET_INPUT_TIMEOUT, &block);
	mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &block);
	if (mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC) != MPP_OK) {
		fprintf(stderr, "encoder: no H.265 encoder\n");
		mpp_destroy(ctx);
		return false;
	}

	MppEncCfg cfg;
	mpp_enc_cfg_init(&cfg);
	mpi->control(ctx, MPP_ENC_GET_CFG, cfg);
	mpp_enc_cfg_set_s32(cfg, "prep:width", width);
	mpp_enc_cfg_set_s32(cfg, "prep:height", height);
	mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", hstride_);
	mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", vstride_);
	mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
	mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_target", kbps * 1000);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_max", kbps * 1000 * 17 / 16);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_min", kbps * 1000 * 15 / 16);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
	mpp_enc_cfg_set_s32(cfg, "rc:gop", fps / 2); // keyframe every 0.5 s: quick start for new viewers
	mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingHEVC);
	MPP_RET r = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
	mpp_enc_cfg_deinit(cfg);
	if (r != MPP_OK) {
		fprintf(stderr, "encoder: MPP_ENC_SET_CFG failed (%d)\n", r);
		mpp_destroy(ctx);
		return false;
	}
	// VPS/SPS/PPS before every keyframe, so a viewer can start at any keyframe.
	MppEncHeaderMode hm = MPP_ENC_HEADER_MODE_EACH_IDR;
	mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &hm);
	ctx_ = ctx;
	mpi_ = mpi;

	MppBufferGroup group = nullptr;
	if (mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_DRM) != MPP_OK) {
		fprintf(stderr, "encoder: no buffer group\n");
		return false;
	}
	group_ = group;
	size_t size = size_t(hstride_) * vstride_ * 3 / 2;
	for (int i = 0; i < 2; i++) {
		MppBuffer b = nullptr;
		if (mpp_buffer_get(group, &b, size) != MPP_OK) {
			fprintf(stderr, "encoder: cannot allocate input buffer\n");
			return false;
		}
		bufs_[i] = b;
		rga_[i] = importbuffer_fd(mpp_buffer_get_fd(b), hstride_, vstride_, RK_FORMAT_YCbCr_420_SP);
		if (!rga_[i]) {
			fprintf(stderr, "encoder: RGA cannot import the input buffer\n");
			return false;
		}
	}

	// The stream FIFO is opened read-write so that writing never blocks or
	// fails while nobody reads it; a full FIFO drops frames until the next
	// keyframe.
	mkdir("/run/kray-demo", 0755);
	unlink(fifo.c_str());
	if (mkfifo(fifo.c_str(), 0644) == 0)
		fifo_fd_ = open(fifo.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fifo_fd_ < 0)
		fprintf(stderr, "encoder: cannot create %s: %s\n", fifo.c_str(), strerror(errno));
	else
		fcntl(fifo_fd_, F_SETPIPE_SZ, 1 << 20);

	mark_us_ = now_us();
	thread_ = std::thread(&Encoder::worker, this);
	fprintf(stderr, "encoder: H.265 %dx%d, %d fps, %d kbit/s -> %s\n", width, height, fps, kbps, fifo.c_str());
	return true;
}

bool Encoder::submit(int src_fd, int width, int height, int stride_px)
{
	if (!ctx_ || busy_)
		return false;
	int i = next_;
	rga_buffer_handle_t sh = importbuffer_fd(src_fd, stride_px, height, RK_FORMAT_BGRX_8888);
	if (!sh)
		return false;
	rga_buffer_t src = wrapbuffer_handle(sh, width, height, RK_FORMAT_BGRX_8888, stride_px, height);
	rga_buffer_t dst = wrapbuffer_handle(rga_[i], width_, height_, RK_FORMAT_YCbCr_420_SP, hstride_, vstride_);
	dst.color_space_mode = IM_RGB_TO_YUV_BT709_LIMIT;
	rga_buffer_t pat{};
	im_rect sr{0, 0, width, height}, dr{0, 0, width_, height_}, pr{};
	IM_STATUS st = improcess(src, dst, pat, sr, dr, pr, -1, nullptr, nullptr, IM_SYNC);
	releasebuffer_handle(sh);
	if (st != IM_STATUS_SUCCESS) {
		fprintf(stderr, "encoder: RGA conversion failed: %s\n", imStrError(st));
		return false;
	}
	busy_ = true;
	next_ ^= 1;
	{
		std::lock_guard<std::mutex> l(mu_);
		pending_ = i;
	}
	cv_.notify_one();
	return true;
}

void Encoder::worker()
{
	for (;;) {
		int i;
		{
			std::unique_lock<std::mutex> l(mu_);
			cv_.wait(l, [&] { return pending_ >= 0 || stop_; });
			if (stop_)
				return;
			i = pending_;
			pending_ = -1;
		}
		MppFrame frame = nullptr;
		mpp_frame_init(&frame);
		mpp_frame_set_width(frame, width_);
		mpp_frame_set_height(frame, height_);
		mpp_frame_set_hor_stride(frame, hstride_);
		mpp_frame_set_ver_stride(frame, vstride_);
		mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
		mpp_frame_set_color_primaries(frame, MPP_FRAME_PRI_BT709);
		mpp_frame_set_buffer(frame, static_cast<MppBuffer>(bufs_[i]));
		MPP_RET r = MPI->encode_put_frame(CTX, frame);
		mpp_frame_deinit(&frame);
		if (r == MPP_OK) {
			bool eoi = false;
			while (!eoi) {
				MppPacket pkt = nullptr;
				if (MPI->encode_get_packet(CTX, &pkt) != MPP_OK || !pkt)
					break;
				RK_S32 intra = 0;
				MppMeta meta = mpp_packet_get_meta(pkt);
				if (meta)
					mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &intra);
				size_t len = mpp_packet_get_length(pkt);
				write_packet(mpp_packet_get_pos(pkt), len, intra != 0);
				bytes_ += len;
				eoi = !mpp_packet_is_partition(pkt) || mpp_packet_is_eoi(pkt);
				mpp_packet_deinit(&pkt);
			}
			frames_++;
		}
		busy_ = false;
	}
}

void Encoder::write_packet(const void *data, size_t len, bool key)
{
	if (fifo_fd_ < 0)
		return;
	if (wait_key_ && !key)
		return;
	int room = 0;
	// Write whole packets only; with too little room, drop up to the next keyframe.
	if (ioctl(fifo_fd_, FIONREAD, &room) == 0 && size_t((1 << 20) - room) < len) {
		wait_key_ = true;
		return;
	}
	ssize_t n = write(fifo_fd_, data, len);
	wait_key_ = n != ssize_t(len);
}

float Encoder::fps()
{
	long long now = now_us();
	unsigned f = frames_;
	unsigned long long b = bytes_;
	float dt = (now - mark_us_) / 1e6f;
	float out = dt > 0 ? (f - frames_mark_) / dt : 0;
	kbps_ = dt > 0 ? (b - bytes_mark_) * 8 / 1000.0f / dt : 0;
	frames_mark_ = f;
	bytes_mark_ = b;
	mark_us_ = now;
	return out;
}
