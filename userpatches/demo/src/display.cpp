// SPDX-License-Identifier: MIT
#include "display.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

bool Display::open()
{
	for (int card = 0; card < 4 && fd_ < 0; card++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/dri/card%d", card);
		int fd = ::open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		drmModeRes *res = drmModeGetResources(fd);
		if (!res || res->count_connectors == 0) {
			drmModeFreeResources(res);
			::close(fd);
			continue;
		}

		drmModeConnector *conn = nullptr;
		for (int i = 0; i < res->count_connectors && !conn; i++) {
			drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
			if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
				conn = c;
			else
				drmModeFreeConnector(c);
		}
		if (!conn) {
			drmModeFreeResources(res);
			::close(fd);
			continue;
		}

		// 1920x1080 if the monitor offers it, else its preferred mode.
		int pick = -1;
		for (int i = 0; i < conn->count_modes; i++) {
			const drmModeModeInfo &m = conn->modes[i];
			if (m.hdisplay == 1920 && m.vdisplay == 1080 && !(m.flags & DRM_MODE_FLAG_INTERLACE) &&
			    (pick < 0 || m.vrefresh == 60))
				pick = i;
		}
		for (int i = 0; i < conn->count_modes && pick < 0; i++)
			if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED)
				pick = i;
		if (pick < 0)
			pick = 0;
		mode_ = conn->modes[pick];

		uint32_t crtc = 0;
		drmModeEncoder *enc = conn->encoder_id ? drmModeGetEncoder(fd, conn->encoder_id) : nullptr;
		if (enc && enc->crtc_id)
			crtc = enc->crtc_id;
		drmModeFreeEncoder(enc);
		for (int e = 0; e < conn->count_encoders && !crtc; e++) {
			drmModeEncoder *en = drmModeGetEncoder(fd, conn->encoders[e]);
			for (int i = 0; en && i < res->count_crtcs; i++)
				if (en->possible_crtcs & (1u << i)) {
					crtc = res->crtcs[i];
					break;
				}
			drmModeFreeEncoder(en);
		}
		conn_id_ = conn->connector_id;
		crtc_id_ = crtc;
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		if (!crtc) {
			::close(fd);
			continue;
		}
		fd_ = fd;
		fprintf(stderr, "display: %s, connector %u, crtc %u, %dx%d@%d\n", path, conn_id_, crtc_id_,
			mode_.hdisplay, mode_.vdisplay, mode_.vrefresh);
	}
	if (fd_ < 0)
		return false;

	for (auto &b : bufs_)
		if (!create_buffer(b)) {
			close();
			return false;
		}
	saved_crtc_ = drmModeGetCrtc(fd_, crtc_id_);
	back_ = 0;
	crtc_set_ = false;
	return true;
}

bool Display::create_buffer(Buffer &b)
{
	drm_mode_create_dumb create{};
	create.width = mode_.hdisplay;
	create.height = mode_.vdisplay;
	create.bpp = 32;
	if (drmIoctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
		perror("display: CREATE_DUMB");
		return false;
	}
	b.handle = create.handle;
	b.pitch = create.pitch;
	b.size = create.size;
	uint32_t handles[4] = {b.handle}, pitches[4] = {b.pitch}, offsets[4] = {0};
	if (drmModeAddFB2(fd_, mode_.hdisplay, mode_.vdisplay, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &b.fb,
			  0)) {
		perror("display: AddFB2");
		return false;
	}
	drm_mode_map_dumb map{};
	map.handle = b.handle;
	if (drmIoctl(fd_, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
		perror("display: MAP_DUMB");
		return false;
	}
	void *p = mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, map.offset);
	if (p == MAP_FAILED) {
		perror("display: mmap");
		return false;
	}
	b.map = static_cast<uint32_t *>(p);
	memset(b.map, 0, b.size);
	if (drmPrimeHandleToFD(fd_, b.handle, DRM_CLOEXEC | DRM_RDWR, &b.dmafd)) {
		perror("display: PrimeHandleToFD");
		return false;
	}
	return true;
}

void Display::close()
{
	if (fd_ < 0)
		return;
	if (saved_crtc_) {
		drmModeSetCrtc(fd_, saved_crtc_->crtc_id, saved_crtc_->buffer_id, saved_crtc_->x, saved_crtc_->y,
			       &conn_id_, 1, &saved_crtc_->mode);
		drmModeFreeCrtc(saved_crtc_);
		saved_crtc_ = nullptr;
	}
	for (auto &b : bufs_) {
		if (b.map)
			munmap(b.map, b.size);
		if (b.dmafd >= 0)
			::close(b.dmafd);
		if (b.fb)
			drmModeRmFB(fd_, b.fb);
		if (b.handle) {
			drm_mode_destroy_dumb d{};
			d.handle = b.handle;
			drmIoctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
		}
		b = Buffer{};
	}
	::close(fd_);
	fd_ = -1;
}

bool Display::connected()
{
	if (fd_ < 0)
		return false;
	drmModeConnector *c = drmModeGetConnector(fd_, conn_id_);
	bool ok = c && c->connection == DRM_MODE_CONNECTED;
	drmModeFreeConnector(c);
	return ok;
}

static void flip_done(int, unsigned, unsigned, unsigned, void *data)
{
	*static_cast<bool *>(data) = true;
}

bool Display::flip()
{
	Buffer &b = bufs_[back_];
	if (!crtc_set_) {
		if (drmModeSetCrtc(fd_, crtc_id_, b.fb, 0, 0, &conn_id_, 1, &mode_)) {
			perror("display: SetCrtc");
			return false;
		}
		crtc_set_ = true;
	} else {
		bool done = false;
		if (drmModePageFlip(fd_, crtc_id_, b.fb, DRM_MODE_PAGE_FLIP_EVENT, &done)) {
			// A modeset (e.g. after re-plugging the monitor) cancels page flipping.
			crtc_set_ = false;
			return false;
		}
		drmEventContext ev{};
		ev.version = 2;
		ev.page_flip_handler = flip_done;
		while (!done) {
			pollfd p{fd_, POLLIN, 0};
			if (poll(&p, 1, 200) <= 0)
				break;
			drmHandleEvent(fd_, &ev);
		}
	}
	back_ ^= 1;
	return true;
}
