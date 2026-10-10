// SPDX-License-Identifier: MIT
// Kray-SoM-RKC: simple auto exposure and auto white balance for the Rockchip
// ISP (rkisp, ISP v2.1) without rkaiq.
//
// Runs as a service next to any capture program. It takes rkaiq's place in
// the ISP driver's 3A protocol: it subscribes to the stream start/stop
// events of the rkisp-input-params node; at stream start the ISP waits for
// the first parameter buffer, which switches on the raw AE statistics
// (rawae3, 15x15 windows with mean R, G and B) and sets the white balance
// gains. While the camera streams it reads the statistics from the
// rkisp-statistics node and
//   - auto exposure: sets exposure, then gain, on the sensor sub-device to
//     bring the mean green level to a target;
//   - auto white balance (grey world): sets the ISP red and blue gains so
//     that the near-neutral windows come out grey.
//
// Settings: /etc/kray-cam-3a.conf (key = value, see the shipped file).
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

#include <linux/rk-isp21-config.h>

#ifndef CIFISP_V4L2_EVENT_STREAM_START
#define CIFISP_V4L2_EVENT_STREAM_START (V4L2_EVENT_PRIVATE_START + 1)
#define CIFISP_V4L2_EVENT_STREAM_STOP (V4L2_EVENT_PRIVATE_START + 2)
#endif

static volatile sig_atomic_t quit;
static int verbose;

#define LOG(...) fprintf(stderr, "kray-cam-3a: " __VA_ARGS__)
#define DBG(...) do { if (verbose) LOG(__VA_ARGS__); } while (0)

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

// ------------------------------------------------------------------ settings

struct Config {
	bool ae = true;
	bool awb = true;
	double target = 0.30;       // mean green level, fraction of full scale above black
	double black = 64.0 / 1024; // sensor black level, fraction of full scale
	int max_exposure = 0;       // lines; 0 = the sensor's maximum
	double max_gain = 16.0;     // total gain limit
	int ae_interval = 3;        // frames between exposure changes (sensor latency)
	double wb_red = 1.6;        // start (or fixed, with awb = 0) gains
	double wb_blue = 1.6;
	double gamma = 2.2;         // ISP output gamma; 0 = leave the ISP's linear output
	bool bls = true;            // subtract the black level in the ISP
	int bls_value = -1;         // ISP black level value; -1 = from black_level
	std::string sensor;         // sub-device name match; empty = first one with an exposure control
};

static Config load_config(const char *path)
{
	Config c;
	std::ifstream f(path);
	std::string line;
	while (std::getline(f, line)) {
		size_t h = line.find('#');
		if (h != std::string::npos)
			line.erase(h);
		size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		auto trim = [](std::string s) {
			s.erase(0, s.find_first_not_of(" \t\""));
			s.erase(s.find_last_not_of(" \t\"\r") + 1);
			return s;
		};
		std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
		auto yes = [&] { return v == "1" || v == "yes" || v == "true"; };
		if (k == "ae") c.ae = yes();
		else if (k == "awb") c.awb = yes();
		else if (k == "target") c.target = atof(v.c_str());
		else if (k == "black_level") c.black = atof(v.c_str());
		else if (k == "max_exposure") c.max_exposure = atoi(v.c_str());
		else if (k == "max_gain") c.max_gain = atof(v.c_str());
		else if (k == "ae_interval") c.ae_interval = std::max(1, atoi(v.c_str()));
		else if (k == "wb_red") c.wb_red = atof(v.c_str());
		else if (k == "wb_blue") c.wb_blue = atof(v.c_str());
		else if (k == "sensor") c.sensor = v;
		else if (k == "gamma") c.gamma = atof(v.c_str());
		else if (k == "black_level_subtraction") c.bls = yes();
		else if (k == "bls_value") c.bls_value = atoi(v.c_str());
	}
	return c;
}

// --------------------------------------------------------------- v4l2 nodes

static std::string node_name(const std::string &n)
{
	std::ifstream f("/sys/class/video4linux/" + n + "/name");
	std::string name;
	std::getline(f, name);
	return name;
}

static std::string find_node(const std::string &prefix, const std::string &match)
{
	DIR *d = opendir("/sys/class/video4linux");
	if (!d)
		return {};
	std::string found;
	while (dirent *e = readdir(d)) {
		std::string n = e->d_name;
		if (n.compare(0, prefix.size(), prefix) == 0 && node_name(n).find(match) != std::string::npos) {
			found = "/dev/" + n;
			break;
		}
	}
	closedir(d);
	return found;
}

struct MmapBuf {
	void *map = nullptr;
	size_t length = 0;
};

// A video node with mmap buffers (params: META_OUTPUT, stats: META_CAPTURE).
struct Queue {
	int fd = -1;
	unsigned type = 0;
	std::vector<MmapBuf> bufs;
	std::vector<bool> free;

	bool setup(int fd_, unsigned type_, unsigned count)
	{
		fd = fd_;
		type = type_;
		v4l2_requestbuffers req{};
		req.count = count;
		req.type = type;
		req.memory = V4L2_MEMORY_MMAP;
		if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1) {
			LOG("REQBUFS: %s\n", strerror(errno));
			return false;
		}
		bufs.resize(req.count);
		free.assign(req.count, true);
		for (unsigned i = 0; i < req.count; i++) {
			v4l2_buffer b{};
			b.type = type;
			b.memory = V4L2_MEMORY_MMAP;
			b.index = i;
			if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0)
				return false;
			bufs[i].length = b.length;
			bufs[i].map = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
			if (bufs[i].map == MAP_FAILED) {
				bufs[i].map = nullptr;
				return false;
			}
		}
		return true;
	}

	bool queue(unsigned i)
	{
		v4l2_buffer b{};
		b.type = type;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.bytesused = bufs[i].length;
		if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
			LOG("QBUF: %s\n", strerror(errno));
			return false;
		}
		free[i] = false;
		return true;
	}

	int dequeue()
	{
		v4l2_buffer b{};
		b.type = type;
		b.memory = V4L2_MEMORY_MMAP;
		if (xioctl(fd, VIDIOC_DQBUF, &b) < 0)
			return -1;
		free[b.index] = true;
		return b.index;
	}

	int free_index()
	{
		while (dequeue() >= 0)
			;
		for (size_t i = 0; i < free.size(); i++)
			if (free[i])
				return i;
		return -1;
	}

	bool stream(bool on)
	{
		int t = type;
		return xioctl(fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &t) == 0;
	}

	void release()
	{
		if (fd < 0)
			return;
		for (auto &b : bufs)
			if (b.map)
				munmap(b.map, b.length);
		bufs.clear();
		free.clear();
		v4l2_requestbuffers req{};
		req.type = type;
		req.memory = V4L2_MEMORY_MMAP;
		xioctl(fd, VIDIOC_REQBUFS, &req);
	}
};

// ------------------------------------------------------------------- sensor

struct Sensor {
	int fd = -1;
	std::string name;
	unsigned gain_id = 0;
	int exp_min = 1, exp_max = 1, exp = 0;
	int gain_min = 0, gain_max = 0, gain = 0;
	bool inverse_gain = false; // Sony register: gain = 1024 / (1024 - code)
	int width = 0, height = 0;

	bool range(unsigned id, int &mn, int &mx, int &cur)
	{
		v4l2_queryctrl q{};
		q.id = id;
		if (xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0 || (q.flags & V4L2_CTRL_FLAG_DISABLED))
			return false;
		v4l2_control c{};
		c.id = id;
		if (xioctl(fd, VIDIOC_G_CTRL, &c) < 0)
			return false;
		mn = q.minimum;
		mx = q.maximum;
		cur = c.value;
		return true;
	}

	bool set(unsigned id, int v)
	{
		v4l2_control c{};
		c.id = id;
		c.value = v;
		return xioctl(fd, VIDIOC_S_CTRL, &c) == 0;
	}

	// Linear gain factor of a gain control value.
	double factor(int code) const
	{
		if (inverse_gain)
			return 1024.0 / std::max(1, 1024 - code);
		return gain_min > 0 ? double(code) / gain_min : code / 256.0;
	}

	int code(double f) const
	{
		int c;
		if (inverse_gain)
			c = int(std::lround(1024.0 - 1024.0 / std::max(1.0, f)));
		else
			c = int(std::lround(f * (gain_min > 0 ? gain_min : 256)));
		return std::clamp(c, gain_min, gain_max);
	}

	bool open(const std::string &match)
	{
		DIR *d = opendir("/sys/class/video4linux");
		if (!d)
			return false;
		while (dirent *e = readdir(d)) {
			std::string n = e->d_name;
			if (n.compare(0, 10, "v4l-subdev") != 0)
				continue;
			std::string nm = node_name(n);
			if (!match.empty() && nm.find(match) == std::string::npos)
				continue;
			// The CSI D-PHY and ISP sub-devices expose the sensor's
			// controls too; take the sensor itself (Rockchip naming
			// "m00_b_imx219 1-0010").
			if (match.empty() && !(nm.size() > 4 && nm[0] == 'm' && isdigit(nm[1]) && isdigit(nm[2]) && nm[3] == '_'))
				continue;
			int f = ::open(("/dev/" + n).c_str(), O_RDWR | O_CLOEXEC);
			if (f < 0)
				continue;
			fd = f;
			int a, b, c;
			if (range(V4L2_CID_EXPOSURE, a, b, c)) {
				name = nm;
				break;
			}
			::close(f);
			fd = -1;
		}
		closedir(d);
		if (fd < 0)
			return false;

		range(V4L2_CID_EXPOSURE, exp_min, exp_max, exp);
		exp_min = std::max(exp_min, 1);
		// Prefer the total gain control (Rockchip drivers), else the analogue one.
		if (range(V4L2_CID_GAIN, gain_min, gain_max, gain))
			gain_id = V4L2_CID_GAIN;
		else if (range(V4L2_CID_ANALOGUE_GAIN, gain_min, gain_max, gain))
			gain_id = V4L2_CID_ANALOGUE_GAIN;
		inverse_gain = gain_id == V4L2_CID_ANALOGUE_GAIN && gain_min == 0 && gain_max < 1024;

		v4l2_subdev_format fmt{};
		fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
		fmt.pad = 0;
		if (xioctl(fd, VIDIOC_SUBDEV_G_FMT, &fmt) == 0) {
			width = fmt.format.width;
			height = fmt.format.height;
		}
		return true;
	}

	void close()
	{
		if (fd >= 0)
			::close(fd);
		fd = -1;
	}
};

// ---------------------------------------------------------------------- 3A

class Engine {
public:
	explicit Engine(const Config &c) : cfg_(c), red_(c.wb_red), blue_(c.wb_blue) {}

	int run();

private:
	bool start();
	void stop();
	void fill_params(void *buf, size_t size, bool first);
	bool send_params(bool first);
	void on_stats(const rkisp_isp21_stat_buffer &s);
	void auto_exposure(double level, double bright);
	void auto_wb(const isp2x_rawaebig_stat &ae);

	Config cfg_;
	int pfd_ = -1, sfd_ = -1;
	Queue params_, stats_;
	Sensor sensor_;
	bool running_ = false;
	unsigned frames_ = 0;
	int max_exp_ = 0;
	double red_, blue_;
	double sent_red_ = 0, sent_blue_ = 0;
	double stat_black_ = 0;     // black level left in the statistics
};

void Engine::fill_params(void *buf, size_t size, bool first)
{
	memset(buf, 0, size);
	auto *p = static_cast<isp21_isp_params_cfg *>(buf);
	p->module_en_update = ISP2X_MODULE_AWB_GAIN;
	p->module_ens = ISP2X_MODULE_AWB_GAIN;
	p->module_cfg_update = ISP2X_MODULE_AWB_GAIN;
	isp21_awb_gain_cfg &g = p->others.awb_gain_cfg;
	int r = std::lround(red_ * 256), b = std::lround(blue_ * 256);
	g.gain0_red = g.gain1_red = g.gain2_red = r;
	g.gain0_blue = g.gain1_blue = g.gain2_blue = b;
	g.gain0_green_r = g.gain1_green_r = g.gain2_green_r = 256;
	g.gain0_green_b = g.gain1_green_b = g.gain2_green_b = 256;
	if (first) {
		stat_black_ = cfg_.black;
		// Raw AE statistics over the whole sensor image, 15x15 windows.
		// rawae3 is the block on the linear path (rawae0-2 serve the HDR
		// frames); rawae_sel 0 measures the raw data before any gain.
		p->module_en_update |= ISP2X_MODULE_RAWAE3;
		p->module_ens |= ISP2X_MODULE_RAWAE3;
		p->module_cfg_update |= ISP2X_MODULE_RAWAE3;
		isp2x_rawaebig_meas_cfg &ae = p->meas.rawae3;
		ae.rawae_sel = 0;
		ae.wnd_num = 2;
		ae.win.h_offs = 0;
		ae.win.v_offs = 0;
		ae.win.h_size = sensor_.width;
		ae.win.v_size = sensor_.height;

		if (cfg_.bls) {
			// Fixed black level, in the ISP's 12-bit scale.
			p->module_en_update |= ISP2X_MODULE_BLS;
			p->module_ens |= ISP2X_MODULE_BLS;
			p->module_cfg_update |= ISP2X_MODULE_BLS;
			isp2x_bls_fixed_val &v = p->others.bls_cfg.fixed_val;
			v.r = v.gr = v.gb = v.b = cfg_.bls_value >= 0 ? cfg_.bls_value : std::lround(cfg_.black * 4096);
			// rawae3 measures after the black level subtraction.
			stat_black_ = std::max(0.0, cfg_.black - v.r / 4096.0);
		}
		if (cfg_.gamma > 0 && offsetof(isp21_isp_params_cfg, others.gammaout_cfg) + sizeof(isp2x_gammaout_cfg) <= size) {
			// Output gamma: 45 points over the 12-bit range, equidistant.
			p->module_en_update |= ISP2X_MODULE_GOC;
			p->module_ens |= ISP2X_MODULE_GOC;
			p->module_cfg_update |= ISP2X_MODULE_GOC;
			isp2x_gammaout_cfg &gc = p->others.gammaout_cfg;
			gc.equ_segm = 1;
			gc.offset = 0;
			for (int i = 0; i < ISP2X_GAMMA_OUT_MAX_SAMPLES; i++) {
				double x = std::min(1.0, i * 4096.0 / (ISP2X_GAMMA_OUT_MAX_SAMPLES - 1) / 4095.0);
				gc.gamma_y[i] = std::lround(4095 * std::pow(x, 1.0 / cfg_.gamma));
			}
		}
	}
	sent_red_ = red_;
	sent_blue_ = blue_;
}

bool Engine::send_params(bool first)
{
	int i = params_.free_index();
	if (i < 0)
		return false;
	fill_params(params_.bufs[i].map, params_.bufs[i].length, first);
	return params_.queue(i);
}

bool Engine::start()
{
	sensor_.close();
	if (!sensor_.open(cfg_.sensor)) {
		LOG("no sensor sub-device with an exposure control\n");
		return false;
	}
	max_exp_ = cfg_.max_exposure > 0 ? std::min(cfg_.max_exposure, sensor_.exp_max) : sensor_.exp_max;
	LOG("stream start: sensor \"%s\" %dx%d, exposure %d..%d, %s %d..%d%s\n", sensor_.name.c_str(),
	    sensor_.width, sensor_.height, sensor_.exp_min, max_exp_,
	    sensor_.gain_id == V4L2_CID_GAIN ? "gain" : "analogue_gain", sensor_.gain_min, sensor_.gain_max,
	    sensor_.inverse_gain ? " (1024/(1024-x))" : "");

	v4l2_format fmt{};
	fmt.type = V4L2_BUF_TYPE_META_OUTPUT;
	const size_t need = offsetof(isp21_isp_params_cfg, others.awb_gain_cfg) + sizeof(isp21_awb_gain_cfg);
	if (xioctl(pfd_, VIDIOC_G_FMT, &fmt) < 0 || fmt.fmt.meta.buffersize < need) {
		LOG("ISP parameter buffer %u bytes, need %zu\n", fmt.fmt.meta.buffersize, need);
		return false;
	}
	if (!params_.setup(pfd_, V4L2_BUF_TYPE_META_OUTPUT, 3))
		return false;
	// The first buffer goes to the ISP right away: it is the configuration
	// the ISP waits for before it starts.
	if (!send_params(true) || !params_.stream(true)) {
		LOG("ISP parameters: %s\n", strerror(errno));
		return false;
	}

	std::string sdev = find_node("video", "rkisp-statistics");
	sfd_ = sdev.empty() ? -1 : ::open(sdev.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (sfd_ < 0 || !stats_.setup(sfd_, V4L2_BUF_TYPE_META_CAPTURE, 4)) {
		LOG("ISP statistics node: %s\n", sdev.empty() ? "not found" : strerror(errno));
		return false;
	}
	for (unsigned i = 0; i < stats_.bufs.size(); i++)
		stats_.queue(i);
	if (!stats_.stream(true)) {
		LOG("ISP statistics STREAMON: %s\n", strerror(errno));
		return false;
	}
	frames_ = 0;
	running_ = true;
	return true;
}

void Engine::stop()
{
	if (sfd_ >= 0) {
		stats_.stream(false);
		stats_.release();
		::close(sfd_);
		sfd_ = -1;
	}
	if (!params_.bufs.empty()) {
		params_.stream(false);
		params_.release();
	}
	running_ = false;
}

void Engine::auto_exposure(double level, double bright)
{
	Sensor &s = sensor_;
	if (level < 0)
		return;
	double total = double(s.exp) * s.factor(s.gain);
	double err = cfg_.target / std::max(level, 0.002);
	// Many clipped windows: do not brighten further (a lamp or a window in
	// view alone should not darken the picture).
	if (bright > 0.15)
		err = std::min(err, bright > 0.3 ? 0.85 : 1.0);
	if (std::fabs(std::log(err)) < 0.08)
		return;
	double f = std::clamp(std::pow(err, 0.8), 0.25, 4.0);
	double want = total * f;

	// Exposure first, up to the limit; then gain.
	int e = std::clamp(int(std::lround(want)), s.exp_min, max_exp_);
	double g = std::clamp(want / e, 1.0, cfg_.max_gain);
	int gc = s.gain_id ? s.code(g) : s.gain;
	if (s.gain_id && s.factor(gc) > cfg_.max_gain)
		gc = s.code(cfg_.max_gain);
	if (e != s.exp && s.set(V4L2_CID_EXPOSURE, e))
		s.exp = e;
	if (gc != s.gain && s.set(s.gain_id, gc))
		s.gain = gc;
	DBG("ae: level %.3f (target %.3f, clip %.2f) -> exposure %d, gain %d (%.2fx)\n", level, cfg_.target,
	    bright, s.exp, s.gain, s.factor(s.gain));
}

void Engine::auto_wb(const isp2x_rawaebig_stat &ae)
{
	// Grey world over windows that are neither dark nor clipped and not
	// strongly coloured. R and B means are 10-bit, G is 12-bit. rawae3
	// measures after the ISP's black level subtraction and white balance
	// gains, so the gains in use are divided out first.
	const double bl = stat_black_;
	double sr = 0, sg = 0, sb = 0;
	int n = 0;
	for (int i = 0; i < ISP2X_RAWAEBIG_MEAN_NUM; i++) {
		double r = ae.data[i].channelr_xy / 1023.0, b = ae.data[i].channelb_xy / 1023.0;
		double g = ae.data[i].channelg_xy / 4095.0;
		if (g > 0.85 || r > 0.9 || b > 0.9)
			continue;
		r = r / sent_red_ - bl;
		b = b / sent_blue_ - bl;
		g -= bl;
		if (g < 0.04 || r <= 0 || b <= 0)
			continue;
		// near neutral with the current gains
		double rr = r * red_ / g, bb = b * blue_ / g;
		if (rr < 0.5 || rr > 2.0 || bb < 0.5 || bb > 2.0)
			continue;
		sr += r;
		sg += g;
		sb += b;
		n++;
	}
	if (n < 10 || sr <= 0 || sb <= 0)
		return;
	double wr = std::clamp(sg / sr, 0.8, 4.0), wb = std::clamp(sg / sb, 0.8, 4.0);
	red_ += (wr - red_) * 0.3;
	blue_ += (wb - blue_) * 0.3;
	if (std::fabs(red_ - sent_red_) > 0.01 || std::fabs(blue_ - sent_blue_) > 0.01) {
		if (send_params(false))
			DBG("awb: %d windows, R %.2f B %.2f\n", n, red_, blue_);
	}
}

void Engine::on_stats(const rkisp_isp21_stat_buffer &s)
{
	frames_++;
	if (!(s.meas_type & ISP2X_STAT_RAWAE3))
		return;
	const isp2x_rawaebig_stat &ae = s.params.rawae3;
	// Mean green above black, centre windows counted twice; share of clipped windows.
	double sum = 0, w = 0;
	int clipped = 0;
	for (int y = 0; y < 15; y++)
		for (int x = 0; x < 15; x++) {
			double g = ae.data[y * 15 + x].channelg_xy / 4095.0;
			double k = (x >= 4 && x <= 10 && y >= 4 && y <= 10) ? 2 : 1;
			sum += k * std::max(0.0, g - stat_black_);
			w += k;
			if (g > 0.9)
				clipped++;
		}
	double level = sum / w / (1 - stat_black_);
	if (verbose > 1 && frames_ % 30 == 1) {
		LOG("stats: frame %u meas 0x%x level %.3f clip %d sum R/G/B %u/%u/%u\n", s.frame_id, s.meas_type,
		    level, clipped, ae.sumr[0], ae.sumg[0], ae.sumb[0]);
		for (int i : {0, 7, 56, 112, 168, 224})
			LOG("  win %3d R/G/B %u/%u/%u\n", i, ae.data[i].channelr_xy, ae.data[i].channelg_xy,
			    ae.data[i].channelb_xy);
	}
	if (cfg_.ae && frames_ % cfg_.ae_interval == 0)
		auto_exposure(level, clipped / 225.0);
	if (cfg_.awb && frames_ % 4 == 0)
		auto_wb(ae);
}

int Engine::run()
{
	std::string pdev;
	for (int i = 0; i < 60 && pdev.empty() && !quit; i++) {
		pdev = find_node("video", "rkisp-input-params");
		if (pdev.empty())
			sleep(1);
	}
	if (pdev.empty()) {
		LOG("no rkisp-input-params node (no camera overlay enabled?)\n");
		return 0;
	}
	pfd_ = ::open(pdev.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (pfd_ < 0) {
		LOG("%s: %s\n", pdev.c_str(), strerror(errno));
		return 1;
	}
	for (unsigned t : {CIFISP_V4L2_EVENT_STREAM_START, CIFISP_V4L2_EVENT_STREAM_STOP}) {
		v4l2_event_subscription sub{};
		sub.type = t;
		if (xioctl(pfd_, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
			LOG("subscribe: %s\n", strerror(errno));
			return 1;
		}
	}
	LOG("waiting for the camera on %s (AE %s, AWB %s, target %.2f)\n", pdev.c_str(), cfg_.ae ? "on" : "off",
	    cfg_.awb ? "on" : "off", cfg_.target);

	while (!quit) {
		pollfd p[2] = {{pfd_, POLLPRI, 0}, {sfd_, POLLIN, 0}};
		int n = poll(p, running_ ? 2 : 1, 1000);
		if (n < 0 && errno != EINTR)
			break;
		if (p[0].revents & POLLPRI) {
			v4l2_event ev{};
			while (xioctl(pfd_, VIDIOC_DQEVENT, &ev) == 0) {
				if (ev.type == CIFISP_V4L2_EVENT_STREAM_START) {
					stop();
					if (!start())
						stop();
				} else if (ev.type == CIFISP_V4L2_EVENT_STREAM_STOP) {
					LOG("stream stop\n");
					stop();
				}
			}
		}
		if (running_ && (p[1].revents & POLLIN)) {
			int i;
			while ((i = stats_.dequeue()) >= 0) {
				on_stats(*static_cast<const rkisp_isp21_stat_buffer *>(stats_.bufs[i].map));
				stats_.queue(i);
			}
		}
	}
	stop();
	sensor_.close();
	::close(pfd_);
	return 0;
}

int main(int argc, char **argv)
{
	const char *conf = "/etc/kray-cam-3a.conf";
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v"))
			verbose++;
		else if (!strcmp(argv[i], "-vv"))
			verbose += 2;
		else if (!strcmp(argv[i], "-c") && i + 1 < argc)
			conf = argv[++i];
		else {
			fprintf(stderr, "usage: kray-cam-3a [-v|-vv] [-c config]\n");
			return 2;
		}
	}
	signal(SIGINT, [](int) { quit = 1; });
	signal(SIGTERM, [](int) { quit = 1; });
	setvbuf(stderr, nullptr, _IOLBF, 0);
	Engine e(load_config(conf));
	return e.run();
}
