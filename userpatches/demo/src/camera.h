// SPDX-License-Identifier: MIT
// V4L2 capture from the Rockchip ISP main path (NV12, multi-planar API) and
// exposure/gain control on the sensor sub-device.
#pragma once

#include <string>
#include <vector>

class Camera {
public:
	struct Buffer {
		void *map = nullptr;
		size_t length = 0;
		int dmafd = -1;
	};

	~Camera() { close(); }
	// red_gain/blue_gain: initial ISP white balance gains, 256 = 1.0.
	bool open(int width, int height, int red_gain, int blue_gain);
	void close();
	bool is_open() const { return fd_ >= 0; }
	// Set the ISP white balance gains (raw domain, 256 = 1.0; green stays 1.0).
	bool set_wb(int red_gain, int blue_gain);
	bool has_wb() const { return pfd_ >= 0; }

	// Wait for a filled buffer; returns its index or -1 on timeout/error.
	int dequeue(int timeout_ms);
	bool queue(int index);

	const Buffer &buffer(int i) const { return bufs_[i]; }
	int width() const { return width_; }
	int height() const { return height_; }
	int stride() const { return stride_; }
	bool failed() const { return errors_ > 10; }

private:
	bool params_open(int red_gain, int blue_gain);
	void params_close();
	int fd_ = -1;
	int width_ = 0, height_ = 0, stride_ = 0;
	int errors_ = 0;
	std::vector<Buffer> bufs_;
	// ISP parameters (rkisp-input-params, isp21_isp_params_cfg)
	int pfd_ = -1;
	std::vector<Buffer> pbufs_;
	std::vector<bool> pfree_;
};

class Sensor {
public:
	~Sensor();
	bool open(const std::string &name_match);
	bool is_open() const { return fd_ >= 0; }
	bool range(unsigned id, int &min, int &max);
	bool set(unsigned id, int value);

private:
	int fd_ = -1;
};

// Path of the /dev/videoN or /dev/v4l-subdevN node whose sysfs name contains `match`.
std::string find_v4l_node(const std::string &prefix, const std::string &match);
