// SPDX-License-Identifier: MIT
// H.265 live stream of the screen with the RK3566 hardware video encoder
// (VEPU540, through Rockchip MPP). RGA converts the XRGB display buffer into
// an NV12 encoder buffer; MPP encodes it; the Annex-B stream goes into a
// FIFO that kray-demo-stream turns into fragmented MP4 for a web page.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

class Encoder {
public:
	~Encoder();
	bool init(int width, int height, int fps, int kbps, const std::string &fifo);
	bool ready() const { return ctx_ != nullptr; }

	// If the encoder is idle, convert the display buffer (XRGB8888 dmabuf)
	// and encode it. Returns false if the previous frame is still encoding.
	bool submit(int src_fd, int width, int height, int stride_px);

	float fps();          // frames encoded per second since the last call
	float kbps() const { return kbps_; }

private:
	void worker();
	void write_packet(const void *data, size_t len, bool key);

	void *ctx_ = nullptr; // MppCtx
	void *mpi_ = nullptr; // MppApi *
	void *group_ = nullptr; // MppBufferGroup
	void *bufs_[2] = {nullptr, nullptr};
	int rga_[2] = {0, 0};
	int next_ = 0;
	int width_ = 0, height_ = 0, hstride_ = 0, vstride_ = 0;

	int fifo_fd_ = -1;
	bool wait_key_ = false;

	std::thread thread_;
	std::mutex mu_;
	std::condition_variable cv_;
	int pending_ = -1;
	bool stop_ = false;
	std::atomic<bool> busy_{false};

	std::atomic<unsigned> frames_{0};
	std::atomic<unsigned long long> bytes_{0};
	unsigned frames_mark_ = 0;
	unsigned long long bytes_mark_ = 0;
	long long mark_us_ = 0;
	float kbps_ = 0;
};
