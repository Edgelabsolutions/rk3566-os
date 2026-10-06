// SPDX-License-Identifier: Apache-2.0
// The output decoding (DFL boxes, score-sum filter, NMS) follows the YOLOv8
// example of airockchip/rknn_model_zoo v2.3.2 (Apache-2.0), reduced to the
// "person" class.
#include "detector.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include <rga/im2d.hpp>
#include <rga/RgaUtils.h>

static long long now_us()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(
		       std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

Detector::~Detector()
{
	{
		std::lock_guard<std::mutex> l(mu_);
		stop_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
	if (in_handle_)
		releasebuffer_handle(in_handle_);
	if (in_mem_)
		rknn_destroy_mem(ctx_, in_mem_);
	if (ctx_)
		rknn_destroy(ctx_);
}

bool Detector::init(const std::string &model, int cam_w, int cam_h, float threshold)
{
	threshold_ = threshold;
	std::ifstream f(model, std::ios::binary);
	if (!f) {
		fprintf(stderr, "detector: cannot read %s\n", model.c_str());
		return false;
	}
	std::vector<char> data((std::istreambuf_iterator<char>(f)), {});
	if (rknn_init(&ctx_, data.data(), data.size(), 0, nullptr) != RKNN_SUCC) {
		fprintf(stderr, "detector: rknn_init failed\n");
		ctx_ = 0;
		return false;
	}
	rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_, sizeof(io_));
	in_attr_.index = 0;
	rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attr_, sizeof(in_attr_));
	out_attrs_.resize(io_.n_output);
	for (unsigned i = 0; i < io_.n_output; i++) {
		out_attrs_[i].index = i;
		rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &out_attrs_[i], sizeof(rknn_tensor_attr));
	}
	if (in_attr_.fmt == RKNN_TENSOR_NCHW) {
		model_h_ = in_attr_.dims[2];
		model_w_ = in_attr_.dims[3];
	} else {
		model_h_ = in_attr_.dims[1];
		model_w_ = in_attr_.dims[2];
	}
	if (io_.n_output % 3) {
		fprintf(stderr, "detector: unexpected model, %u outputs\n", io_.n_output);
		return false;
	}

	// Zero-copy input: RGA writes RGB888 straight into NPU memory.
	in_attr_.type = RKNN_TENSOR_UINT8;
	in_attr_.fmt = RKNN_TENSOR_NHWC;
	in_attr_.pass_through = 0;
	size_t in_size = size_t(model_w_) * model_h_ * 3;
	in_mem_ = rknn_create_mem(ctx_, in_size);
	if (!in_mem_ || rknn_set_io_mem(ctx_, in_mem_, &in_attr_) != RKNN_SUCC) {
		fprintf(stderr, "detector: rknn_set_io_mem failed\n");
		return false;
	}
	memset(in_mem_->virt_addr, 114, in_size); // letterbox padding, as in training
	rknn_mem_sync(ctx_, in_mem_, RKNN_MEMORY_SYNC_TO_DEVICE);
	in_handle_ = importbuffer_fd(in_mem_->fd, model_w_, model_h_, RK_FORMAT_RGB_888);
	if (!in_handle_) {
		fprintf(stderr, "detector: RGA cannot import the NPU input buffer\n");
		return false;
	}

	scale_ = std::min(float(model_w_) / cam_w, float(model_h_) / cam_h);
	box_w_ = (int(cam_w * scale_) + 1) & ~1;
	box_h_ = (int(cam_h * scale_) + 1) & ~1;
	pad_x_ = ((model_w_ - box_w_) / 2) & ~1;
	pad_y_ = ((model_h_ - box_h_) / 2) & ~1;

	rknn_sdk_version ver{};
	rknn_query(ctx_, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver));
	fprintf(stderr, "detector: %s, input %dx%d, %u outputs, runtime %s\n", model.c_str(), model_w_, model_h_,
		io_.n_output, ver.api_version);

	wall_mark_us_ = now_us();
	thread_ = std::thread(&Detector::worker, this);
	return true;
}

bool Detector::submit(int src_fd, int width, int height, int stride)
{
	if (!ctx_ || busy_)
		return false;
	rga_buffer_handle_t sh = importbuffer_fd(src_fd, stride, height, RK_FORMAT_YCbCr_420_SP);
	if (!sh)
		return false;
	rga_buffer_t src = wrapbuffer_handle(sh, width, height, RK_FORMAT_YCbCr_420_SP, stride, height);
	rga_buffer_t dst = wrapbuffer_handle(in_handle_, model_w_, model_h_, RK_FORMAT_RGB_888);
	src.color_space_mode = IM_YUV_TO_RGB_BT601_FULL;
	rga_buffer_t pat{};
	im_rect srect{0, 0, width, height};
	im_rect drect{pad_x_, pad_y_, box_w_, box_h_};
	im_rect prect{};
	IM_STATUS st = improcess(src, dst, pat, srect, drect, prect, -1, nullptr, nullptr, IM_SYNC);
	releasebuffer_handle(sh);
	if (st != IM_STATUS_SUCCESS) {
		fprintf(stderr, "detector: RGA letterbox failed: %s\n", imStrError(st));
		return false;
	}
	busy_ = true;
	{
		std::lock_guard<std::mutex> l(mu_);
		pending_ = true;
	}
	cv_.notify_one();
	return true;
}

void Detector::worker()
{
	std::vector<rknn_output> outs(io_.n_output);
	std::vector<Detection> dets;
	for (;;) {
		{
			std::unique_lock<std::mutex> l(mu_);
			cv_.wait(l, [&] { return pending_ || stop_; });
			if (stop_)
				return;
			pending_ = false;
		}
		long long t0 = now_us();
		int r = rknn_run(ctx_, nullptr);
		long long t1 = now_us();
		busy_us_ += t1 - t0;
		dets.clear();
		if (r == RKNN_SUCC) {
			for (auto &o : outs) {
				memset(&o, 0, sizeof(o));
				o.want_float = 0;
			}
			for (unsigned i = 0; i < io_.n_output; i++)
				outs[i].index = i;
			if (rknn_outputs_get(ctx_, io_.n_output, outs.data(), nullptr) == RKNN_SUCC) {
				postprocess(outs.data(), dets);
				rknn_outputs_release(ctx_, io_.n_output, outs.data());
			}
		}
		{
			std::lock_guard<std::mutex> l(mu_);
			result_ = dets;
			seq_++;
			infer_ms_ = (t1 - t0) / 1000.0f;
		}
		completed_++;
		busy_ = false;
	}
}

void Detector::latest(std::vector<Detection> &out, unsigned &seq, float &infer_ms)
{
	std::lock_guard<std::mutex> l(mu_);
	out = result_;
	seq = seq_;
	infer_ms = infer_ms_;
}

float Detector::busy_share()
{
	long long now = now_us(), busy = busy_us_;
	float share = now > wall_mark_us_ ? float(busy - busy_mark_us_) / float(now - wall_mark_us_) : 0;
	busy_mark_us_ = busy;
	wall_mark_us_ = now;
	return std::min(std::max(share, 0.0f), 1.0f);
}

static inline float deq(int8_t v, int32_t zp, float scale)
{
	return (float(v) - float(zp)) * scale;
}

static inline int8_t qnt(float f, int32_t zp, float scale)
{
	float v = f / scale + zp;
	return int8_t(std::max(-128.0f, std::min(127.0f, v)));
}

static float iou(const Detection &a, const Detection &b)
{
	float w = std::max(0.0f, std::min(a.x2, b.x2) - std::max(a.x1, b.x1));
	float h = std::max(0.0f, std::min(a.y2, b.y2) - std::max(a.y1, b.y1));
	float i = w * h;
	float u = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - i;
	return u > 0 ? i / u : 0;
}

void Detector::postprocess(rknn_output *out, std::vector<Detection> &dets)
{
	const int per_branch = io_.n_output / 3;
	std::vector<Detection> cand;
	for (int b = 0; b < 3; b++) {
		const rknn_tensor_attr &ba = out_attrs_[b * per_branch];
		const rknn_tensor_attr &sa = out_attrs_[b * per_branch + 1];
		const rknn_tensor_attr *ssa = per_branch == 3 ? &out_attrs_[b * per_branch + 2] : nullptr;
		const int8_t *box = static_cast<const int8_t *>(out[b * per_branch].buf);
		const int8_t *score = static_cast<const int8_t *>(out[b * per_branch + 1].buf);
		const int8_t *score_sum = ssa ? static_cast<const int8_t *>(out[b * per_branch + 2].buf) : nullptr;
		const int grid_h = ba.dims[2], grid_w = ba.dims[3], grid = grid_h * grid_w;
		const int dfl = ba.dims[1] / 4;
		const int stride = model_h_ / grid_h;
		const int8_t thr = qnt(threshold_, sa.zp, sa.scale);
		const int8_t sum_thr = ssa ? qnt(threshold_, ssa->zp, ssa->scale) : -128;

		for (int i = 0; i < grid_h; i++)
			for (int j = 0; j < grid_w; j++) {
				int off = i * grid_w + j;
				if (score_sum && score_sum[off] < sum_thr)
					continue;
				int8_t s = score[off]; // class 0: person
				if (s <= thr)
					continue;
				float d[4];
				for (int k = 0; k < 4; k++) {
					float e[32], sum = 0, acc = 0;
					for (int n = 0; n < dfl && n < 32; n++) {
						e[n] = expf(deq(box[(k * dfl + n) * grid + off], ba.zp, ba.scale));
						sum += e[n];
					}
					for (int n = 0; n < dfl && n < 32; n++)
						acc += e[n] / sum * n;
					d[k] = acc;
				}
				Detection det;
				det.x1 = (j + 0.5f - d[0]) * stride;
				det.y1 = (i + 0.5f - d[1]) * stride;
				det.x2 = (j + 0.5f + d[2]) * stride;
				det.y2 = (i + 0.5f + d[3]) * stride;
				det.score = deq(s, sa.zp, sa.scale);
				cand.push_back(det);
			}
	}

	std::sort(cand.begin(), cand.end(), [](const Detection &a, const Detection &b) { return a.score > b.score; });
	std::vector<bool> dropped(cand.size(), false);
	for (size_t i = 0; i < cand.size(); i++) {
		if (dropped[i])
			continue;
		for (size_t j = i + 1; j < cand.size(); j++)
			if (!dropped[j] && iou(cand[i], cand[j]) > 0.45f)
				dropped[j] = true;
		// Model input -> camera pixels.
		Detection d = cand[i];
		d.x1 = (d.x1 - pad_x_) / scale_;
		d.x2 = (d.x2 - pad_x_) / scale_;
		d.y1 = (d.y1 - pad_y_) / scale_;
		d.y2 = (d.y2 - pad_y_) / scale_;
		dets.push_back(d);
	}
}
