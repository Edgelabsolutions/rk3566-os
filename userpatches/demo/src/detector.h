// SPDX-License-Identifier: Apache-2.0
// Person detection with YOLOv8 (rknn_model_zoo export, int8) on the RK3566 NPU.
// The camera frame is letterboxed into the NPU input buffer by RGA; inference
// and post-processing run in a worker thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rknn_api.h"

struct Detection {
	float x1, y1, x2, y2; // camera pixels
	float score;
};

class Detector {
public:
	~Detector();
	bool init(const std::string &model, int cam_w, int cam_h, float threshold);
	bool ready() const { return ctx_ != 0; }

	// If the NPU is idle, copy the frame (NV12 dmabuf) into the model input and
	// start an inference. Returns false if the NPU was busy.
	bool submit(int src_fd, int width, int height, int stride);

	// Latest result; `seq` grows with every finished inference.
	void latest(std::vector<Detection> &out, unsigned &seq, float &infer_ms);
	// Share of wall time the NPU spent in rknn_run since the last call (0..1).
	float busy_share();
	unsigned completed() const { return completed_; }

private:
	void worker();
	void postprocess(rknn_output *out, std::vector<Detection> &dets);

	rknn_context ctx_ = 0;
	rknn_input_output_num io_{};
	std::vector<rknn_tensor_attr> out_attrs_;
	rknn_tensor_attr in_attr_{};
	rknn_tensor_mem *in_mem_ = nullptr;
	int in_handle_ = 0; // RGA handle of the input buffer
	int model_w_ = 640, model_h_ = 640;
	// Letterbox: model = camera * scale_ + pad
	float scale_ = 1;
	int pad_x_ = 0, pad_y_ = 0, box_w_ = 0, box_h_ = 0;
	float threshold_ = 0.35f;

	std::thread thread_;
	std::mutex mu_;
	std::condition_variable cv_;
	bool pending_ = false, stop_ = false;
	std::atomic<bool> busy_{false};

	std::vector<Detection> result_;
	unsigned seq_ = 0;
	float infer_ms_ = 0;
	std::atomic<unsigned> completed_{0};
	std::atomic<long long> busy_us_{0};
	long long busy_mark_us_ = 0, wall_mark_us_ = 0;
};
