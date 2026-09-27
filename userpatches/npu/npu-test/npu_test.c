// NPU smoke test for Kray-SoM-RKC (RK3566): MobileNet v1 classification
// with timing. Usage: npu_test <model.rknn> <image.jpg> [iterations]
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rknn_api.h"

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static unsigned char *load_file(const char *path, int *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *buf = malloc(*len);
	if (buf && fread(buf, 1, *len, f) != (size_t)*len) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	return buf;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "Usage: %s <model.rknn> <image.jpg> [iterations]\n", argv[0]);
		return 1;
	}
	int iters = argc > 3 ? atoi(argv[3]) : 100;

	int w, h, c;
	unsigned char *img = stbi_load(argv[2], &w, &h, &c, 3);
	if (!img || w != 224 || h != 224) {
		fprintf(stderr, "need a 224x224 image, got %s (%dx%d)\n", argv[2], w, h);
		return 1;
	}

	int model_len;
	unsigned char *model = load_file(argv[1], &model_len);
	if (!model) {
		fprintf(stderr, "cannot read model %s\n", argv[1]);
		return 1;
	}

	rknn_context ctx;
	double t0 = now_ms();
	int ret = rknn_init(&ctx, model, model_len, 0, NULL);
	if (ret < 0) {
		fprintf(stderr, "rknn_init failed: %d\n", ret);
		return 1;
	}
	printf("rknn_init: %.1f ms\n", now_ms() - t0);

	rknn_sdk_version ver;
	if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == 0)
		printf("runtime: %s\ndriver:  %s\n", ver.api_version, ver.drv_version);

	rknn_input in;
	memset(&in, 0, sizeof(in));
	in.index = 0;
	in.type = RKNN_TENSOR_UINT8;
	in.fmt = RKNN_TENSOR_NHWC;
	in.size = 224 * 224 * 3;
	in.buf = img;

	double total = 0, best = 1e9;
	for (int i = 0; i < iters; i++) {
		if ((ret = rknn_inputs_set(ctx, 1, &in)) < 0) {
			fprintf(stderr, "rknn_inputs_set failed: %d\n", ret);
			return 1;
		}
		t0 = now_ms();
		if ((ret = rknn_run(ctx, NULL)) < 0) {
			fprintf(stderr, "rknn_run failed: %d\n", ret);
			return 1;
		}
		double dt = now_ms() - t0;
		total += dt;
		if (dt < best)
			best = dt;
	}
	printf("rknn_run x%d: avg %.2f ms, best %.2f ms (%.1f FPS)\n",
	       iters, total / iters, best, 1000.0 * iters / total);

	rknn_output out;
	memset(&out, 0, sizeof(out));
	out.want_float = 1;
	if ((ret = rknn_outputs_get(ctx, 1, &out, NULL)) < 0) {
		fprintf(stderr, "rknn_outputs_get failed: %d\n", ret);
		return 1;
	}

	float *p = out.buf;
	int n = out.size / sizeof(float);
	printf("top5 (class index: probability):\n");
	for (int k = 0; k < 5; k++) {
		int best_i = 0;
		for (int i = 1; i < n; i++)
			if (p[i] > p[best_i])
				best_i = i;
		printf("  %4d: %.4f\n", best_i, p[best_i]);
		p[best_i] = -1.0f;
	}

	rknn_outputs_release(ctx, 1, &out);
	rknn_destroy(ctx);
	free(model);
	stbi_image_free(img);
	return 0;
}
