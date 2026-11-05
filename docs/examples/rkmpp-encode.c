// SPDX-License-Identifier: MIT
// Encode 300 synthetic 1080p NV12 frames with the hardware H.264 encoder
// through libavcodec and write a raw H.264 stream.
//
//   gcc rkmpp-encode.c -o rkmpp-encode \
//       $(PKG_CONFIG_PATH=/opt/ffmpeg-rockchip/lib/pkgconfig pkg-config --cflags --libs libavcodec libavutil)
//   ./rkmpp-encode out.h264
#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

static int drain(AVCodecContext *ctx, AVPacket *pkt, FILE *out)
{
	int ret;

	while ((ret = avcodec_receive_packet(ctx, pkt)) == 0) {
		fwrite(pkt->data, 1, pkt->size, out);
		av_packet_unref(pkt);
	}
	return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
}

int main(int argc, char **argv)
{
	const int w = 1920, h = 1080, frames = 300;

	if (argc != 2) {
		fprintf(stderr, "usage: %s OUT.h264\n", argv[0]);
		return 2;
	}
	const AVCodec *codec = avcodec_find_encoder_by_name("h264_rkmpp");
	if (!codec) {
		fprintf(stderr, "h264_rkmpp not found: link against /opt/ffmpeg-rockchip\n");
		return 1;
	}
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	ctx->width = w;
	ctx->height = h;
	ctx->pix_fmt = AV_PIX_FMT_NV12;
	ctx->time_base = (AVRational){1, 30};
	ctx->framerate = (AVRational){30, 1};
	ctx->bit_rate = 4000000;
	ctx->gop_size = 60;
	av_opt_set(ctx->priv_data, "rc_mode", "CBR", 0);
	if (avcodec_open2(ctx, codec, NULL) < 0) {
		fprintf(stderr, "avcodec_open2 failed\n");
		return 1;
	}

	FILE *out = fopen(argv[1], "wb");
	if (!out) {
		perror(argv[1]);
		return 1;
	}
	AVFrame *frame = av_frame_alloc();
	AVPacket *pkt = av_packet_alloc();
	frame->format = ctx->pix_fmt;
	frame->width = w;
	frame->height = h;
	av_frame_get_buffer(frame, 0);

	for (int i = 0; i < frames; i++) {
		av_frame_make_writable(frame);
		// Moving gradient: Y plane, then interleaved UV at half resolution.
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				frame->data[0][y * frame->linesize[0] + x] = x + y + i * 4;
		for (int y = 0; y < h / 2; y++)
			for (int x = 0; x < w / 2; x++) {
				frame->data[1][y * frame->linesize[1] + 2 * x] = 128 + y - i;
				frame->data[1][y * frame->linesize[1] + 2 * x + 1] = 64 + x + i;
			}
		frame->pts = i;
		if (avcodec_send_frame(ctx, frame) < 0 || drain(ctx, pkt, out) < 0) {
			fprintf(stderr, "encoding failed at frame %d\n", i);
			return 1;
		}
	}
	avcodec_send_frame(ctx, NULL);
	drain(ctx, pkt, out);

	fclose(out);
	av_packet_free(&pkt);
	av_frame_free(&frame);
	avcodec_free_context(&ctx);
	printf("encoded %d frames with %s\n", frames, codec->name);
	return 0;
}
