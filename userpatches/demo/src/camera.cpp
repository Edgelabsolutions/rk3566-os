// SPDX-License-Identifier: MIT
#include "camera.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/rk-isp21-config.h>

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

std::string find_v4l_node(const std::string &prefix, const std::string &match)
{
	DIR *d = opendir("/sys/class/video4linux");
	if (!d)
		return {};
	std::string found;
	while (dirent *e = readdir(d)) {
		std::string n = e->d_name;
		if (n.compare(0, prefix.size(), prefix) != 0)
			continue;
		std::ifstream f("/sys/class/video4linux/" + n + "/name");
		std::string name;
		std::getline(f, name);
		if (name.find(match) != std::string::npos) {
			found = "/dev/" + n;
			break;
		}
	}
	closedir(d);
	return found;
}

bool Camera::open(int width, int height, int red_gain, int blue_gain)
{
	std::string dev = find_v4l_node("video", "rkisp_mainpath");
	if (dev.empty())
		return false;
	fd_ = ::open(dev.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd_ < 0)
		return false;

	v4l2_format fmt{};
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = width;
	fmt.fmt.pix_mp.height = height;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix_mp.num_planes = 1;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
		perror("camera: VIDIOC_S_FMT");
		close();
		return false;
	}
	width_ = fmt.fmt.pix_mp.width;
	height_ = fmt.fmt.pix_mp.height;
	stride_ = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	if (!stride_)
		stride_ = width_;

	v4l2_requestbuffers req{};
	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
		perror("camera: VIDIOC_REQBUFS");
		close();
		return false;
	}
	bufs_.resize(req.count);
	for (unsigned i = 0; i < req.count; i++) {
		v4l2_plane plane{};
		v4l2_buffer b{};
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.m.planes = &plane;
		b.length = 1;
		if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) {
			perror("camera: VIDIOC_QUERYBUF");
			close();
			return false;
		}
		bufs_[i].length = plane.length;
		bufs_[i].map = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, plane.m.mem_offset);
		if (bufs_[i].map == MAP_FAILED) {
			bufs_[i].map = nullptr;
			perror("camera: mmap");
			close();
			return false;
		}
		v4l2_exportbuffer exp{};
		exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		exp.index = i;
		exp.plane = 0;
		exp.flags = O_RDWR | O_CLOEXEC;
		if (xioctl(fd_, VIDIOC_EXPBUF, &exp) < 0) {
			perror("camera: VIDIOC_EXPBUF");
			close();
			return false;
		}
		bufs_[i].dmafd = exp.fd;
		if (!queue(i)) {
			close();
			return false;
		}
	}
	// White balance gains go in before streaming starts: the first parameter
	// buffer is applied by the ISP at stream on.
	if (!params_open(red_gain, blue_gain))
		fprintf(stderr, "camera: no ISP parameter node, white balance not set\n");
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
		perror("camera: VIDIOC_STREAMON");
		close();
		return false;
	}
	errors_ = 0;
	fprintf(stderr, "camera: %s %dx%d NV12, stride %d, %zu buffers\n", dev.c_str(), width_, height_, stride_,
		bufs_.size());
	return true;
}

void Camera::close()
{
	params_close();
	if (fd_ < 0)
		return;
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	xioctl(fd_, VIDIOC_STREAMOFF, &type);
	for (auto &b : bufs_) {
		if (b.map)
			munmap(b.map, b.length);
		if (b.dmafd >= 0)
			::close(b.dmafd);
	}
	bufs_.clear();
	v4l2_requestbuffers req{};
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	xioctl(fd_, VIDIOC_REQBUFS, &req);
	::close(fd_);
	fd_ = -1;
}

int Camera::dequeue(int timeout_ms)
{
	pollfd p{fd_, POLLIN, 0};
	int r = poll(&p, 1, timeout_ms);
	if (r <= 0) {
		errors_++;
		return -1;
	}
	v4l2_plane plane{};
	v4l2_buffer b{};
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = &plane;
	b.length = 1;
	if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) {
		errors_++;
		return -1;
	}
	errors_ = 0;
	return b.index;
}

bool Camera::queue(int index)
{
	v4l2_plane plane{};
	v4l2_buffer b{};
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = index;
	b.m.planes = &plane;
	b.length = 1;
	if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) {
		perror("camera: VIDIOC_QBUF");
		return false;
	}
	return true;
}

Sensor::~Sensor()
{
	if (fd_ >= 0)
		::close(fd_);
}

bool Sensor::open(const std::string &name_match)
{
	std::string dev = find_v4l_node("v4l-subdev", name_match);
	if (dev.empty())
		return false;
	fd_ = ::open(dev.c_str(), O_RDWR | O_CLOEXEC);
	return fd_ >= 0;
}

bool Sensor::range(unsigned id, int &min, int &max)
{
	v4l2_queryctrl q{};
	q.id = id;
	if (fd_ < 0 || xioctl(fd_, VIDIOC_QUERYCTRL, &q) < 0)
		return false;
	min = q.minimum;
	max = q.maximum;
	return true;
}

bool Sensor::set(unsigned id, int value)
{
	v4l2_control c{};
	c.id = id;
	c.value = value;
	return fd_ >= 0 && xioctl(fd_, VIDIOC_S_CTRL, &c) == 0;
}

static void fill_wb(void *buf, size_t size, int red, int blue)
{
	memset(buf, 0, size);
	auto *p = static_cast<isp21_isp_params_cfg *>(buf);
	p->module_en_update = ISP2X_MODULE_AWB_GAIN;
	p->module_ens = ISP2X_MODULE_AWB_GAIN;
	p->module_cfg_update = ISP2X_MODULE_AWB_GAIN;
	isp21_awb_gain_cfg &g = p->others.awb_gain_cfg;
	g.gain0_red = g.gain1_red = g.gain2_red = red;
	g.gain0_blue = g.gain1_blue = g.gain2_blue = blue;
	g.gain0_green_r = g.gain1_green_r = g.gain2_green_r = 256;
	g.gain0_green_b = g.gain1_green_b = g.gain2_green_b = 256;
}

bool Camera::params_open(int red_gain, int blue_gain)
{
	std::string dev = find_v4l_node("video", "rkisp-input-params");
	if (dev.empty())
		return false;
	pfd_ = ::open(dev.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (pfd_ < 0)
		return false;
	v4l2_format fmt{};
	fmt.type = V4L2_BUF_TYPE_META_OUTPUT;
	// The driver sizes the buffer for the common ISP2x layout, which is a bit
	// shorter than isp21_isp_params_cfg; only the header and the AWB gains
	// (near the start of "others") are used here.
	const size_t need = offsetof(isp21_isp_params_cfg, others.awb_gain_cfg) + sizeof(isp21_awb_gain_cfg);
	if (xioctl(pfd_, VIDIOC_G_FMT, &fmt) < 0 || fmt.fmt.meta.buffersize < need) {
		fprintf(stderr, "camera: ISP parameter buffer %u bytes, need %zu\n", fmt.fmt.meta.buffersize, need);
		params_close();
		return false;
	}
	v4l2_requestbuffers req{};
	req.count = 3;
	req.type = V4L2_BUF_TYPE_META_OUTPUT;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(pfd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
		params_close();
		return false;
	}
	pbufs_.resize(req.count);
	pfree_.assign(req.count, true);
	for (unsigned i = 0; i < req.count; i++) {
		v4l2_buffer b{};
		b.type = V4L2_BUF_TYPE_META_OUTPUT;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		if (xioctl(pfd_, VIDIOC_QUERYBUF, &b) < 0) {
			params_close();
			return false;
		}
		pbufs_[i].length = b.length;
		pbufs_[i].map = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, pfd_, b.m.offset);
		if (pbufs_[i].map == MAP_FAILED) {
			pbufs_[i].map = nullptr;
			params_close();
			return false;
		}
	}
	if (!set_wb(red_gain, blue_gain)) {
		params_close();
		return false;
	}
	int type = V4L2_BUF_TYPE_META_OUTPUT;
	if (xioctl(pfd_, VIDIOC_STREAMON, &type) < 0) {
		perror("camera: ISP parameters STREAMON");
		params_close();
		return false;
	}
	fprintf(stderr, "camera: ISP white balance via %s, R %.2f B %.2f\n", dev.c_str(), red_gain / 256.0,
		blue_gain / 256.0);
	return true;
}

void Camera::params_close()
{
	if (pfd_ < 0)
		return;
	int type = V4L2_BUF_TYPE_META_OUTPUT;
	xioctl(pfd_, VIDIOC_STREAMOFF, &type);
	for (auto &b : pbufs_)
		if (b.map)
			munmap(b.map, b.length);
	pbufs_.clear();
	pfree_.clear();
	v4l2_requestbuffers req{};
	req.type = V4L2_BUF_TYPE_META_OUTPUT;
	req.memory = V4L2_MEMORY_MMAP;
	xioctl(pfd_, VIDIOC_REQBUFS, &req);
	::close(pfd_);
	pfd_ = -1;
}

bool Camera::set_wb(int red_gain, int blue_gain)
{
	if (pfd_ < 0)
		return false;
	// Take back the buffers the ISP has finished with.
	for (;;) {
		v4l2_buffer b{};
		b.type = V4L2_BUF_TYPE_META_OUTPUT;
		b.memory = V4L2_MEMORY_MMAP;
		if (xioctl(pfd_, VIDIOC_DQBUF, &b) < 0)
			break;
		pfree_[b.index] = true;
	}
	for (size_t i = 0; i < pbufs_.size(); i++) {
		if (!pfree_[i])
			continue;
		fill_wb(pbufs_[i].map, pbufs_[i].length, red_gain, blue_gain);
		v4l2_buffer b{};
		b.type = V4L2_BUF_TYPE_META_OUTPUT;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.bytesused = pbufs_[i].length;
		if (xioctl(pfd_, VIDIOC_QBUF, &b) < 0) {
			perror("camera: ISP parameters QBUF");
			return false;
		}
		pfree_[i] = false;
		return true;
	}
	return false; // all buffers still queued; try again later
}
