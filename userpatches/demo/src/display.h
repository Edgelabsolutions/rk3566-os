// SPDX-License-Identifier: MIT
// Full-screen output on the HDMI connector through DRM/KMS: two XRGB8888
// dumb buffers, page flips synchronised to vblank.
#pragma once

#include <cstdint>
#include <xf86drmMode.h>

class Display {
public:
	struct Buffer {
		uint32_t handle = 0, fb = 0, pitch = 0;
		uint64_t size = 0;
		uint32_t *map = nullptr;
		int dmafd = -1;
	};

	~Display() { close(); }
	// Opens the first connected connector; prefers 1920x1080.
	bool open();
	void close();
	bool connected();

	int width() const { return mode_.hdisplay; }
	int height() const { return mode_.vdisplay; }
	Buffer &back() { return bufs_[back_]; }
	int back_index() const { return back_; }
	// Show the back buffer and wait until it is on screen.
	bool flip();

private:
	bool create_buffer(Buffer &b);
	int fd_ = -1;
	uint32_t conn_id_ = 0, crtc_id_ = 0;
	drmModeModeInfo mode_{};
	drmModeCrtc *saved_crtc_ = nullptr;
	Buffer bufs_[2];
	int back_ = 0;
	bool crtc_set_ = false;
};
