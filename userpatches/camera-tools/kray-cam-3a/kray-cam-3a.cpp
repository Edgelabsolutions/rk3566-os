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
// With the first parameters it also loads a lens shading table made by
// kray-cam-lsc (/etc/kray-cam-3a/lsc-<sensor>-<width>x<height>.conf), and
// with every white balance change it sets a colour correction matrix for
// the light's colour temperature (/etc/kray-cam-3a/ccm-<sensor>.conf).
// Noise reduction (Bayer 2D and temporal, colour, luma) and sharpening
// follow the sensor gain (/etc/kray-cam-3a/nr-<sensor>.conf, made by
// scripts/gen-isp21-nr.py).
//
// Settings: /etc/kray-cam-3a.conf (key = value, see the shipped file).
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <glob.h>
#include <map>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

// The ISP21 header uses the kernel's BIT_ULL for the ISP21-only module bits.
#ifndef BIT_ULL
#define BIT_ULL(n) (1ULL << (n))
#endif
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
	bool lsc = true;            // lens shading correction, if there is a table
	std::string lsc_dir = "/etc/kray-cam-3a";
	bool ccm = true;            // colour correction, if there is a table
	double saturation = 1.0;    // 0 = no colour correction, 1 = the table's matrix
	bool denoise = true;        // colour and luma noise reduction, if there is a table
	bool sharpen = true;        // sharpening, if there is a table
	bool bayer_denoise = true;  // Bayer-domain noise reduction, if the table has it
	bool temporal_denoise = true; // temporal (frame to frame) noise reduction, likewise
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
		else if (k == "lsc") c.lsc = yes();
		else if (k == "lsc_dir") c.lsc_dir = v;
		else if (k == "ccm") c.ccm = yes();
		else if (k == "saturation") c.saturation = std::clamp(atof(v.c_str()), 0.0, 2.0);
		else if (k == "denoise") c.denoise = yes();
		else if (k == "sharpen") c.sharpen = yes();
		else if (k == "bayer_denoise") c.bayer_denoise = yes();
		else if (k == "temporal_denoise") c.temporal_denoise = yes();
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

// ------------------------------------------------------- lens shading table

// The ISP's lens shading grid: 16 sectors per axis, given as 8 sizes for one
// half of the image and mirrored; 17 x 17 gains per Bayer channel (R, Gr, Gb,
// B), 1024 = 1.0.
constexpr int LSC_N = 17;
// The ISP's colour correction coefficients: 1.0 and the signed 11-bit range
// (the hardware takes the matrix minus the identity, see fill_params).
constexpr int CCM_ONE = 128;
constexpr long CCM_LIMIT = 1023;

struct LscTable {
	int width = 0, height = 0;
	int x_sect[8] = {}, y_sect[8] = {};
	double gain[4][LSC_N][LSC_N] = {};

	bool load(const std::string &path)
	{
		std::ifstream f(path);
		if (!f)
			return false;
		static const char *const names[4] = {"r", "gr", "gb", "b"};
		unsigned rows[4] = {};
		bool xs = false, ys = false;
		std::string line;
		while (std::getline(f, line)) {
			size_t h = line.find('#');
			if (h != std::string::npos)
				line.erase(h);
			size_t eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			std::istringstream ks(line.substr(0, eq)), vs(line.substr(eq + 1));
			std::string k;
			ks >> k;
			std::vector<double> v;
			for (double d; vs >> d;)
				v.push_back(d);
			if (k == "size" && v.size() == 2) {
				width = int(v[0]);
				height = int(v[1]);
			} else if ((k == "x_sect" || k == "y_sect") && v.size() == 8) {
				for (int i = 0; i < 8; i++)
					(k[0] == 'x' ? x_sect : y_sect)[i] = int(v[i]);
				(k[0] == 'x' ? xs : ys) = true;
			} else {
				for (int c = 0; c < 4; c++) {
					size_t n = strlen(names[c]);
					if (k.compare(0, n, names[c]) || k.size() == n || !isdigit(k[n]))
						continue;
					int row = atoi(k.c_str() + n);
					if (row < 0 || row >= LSC_N || v.size() != LSC_N)
						return false;
					for (int i = 0; i < LSC_N; i++)
						gain[c][row][i] = std::clamp(v[i] / 1024, 1.0 / 8, 8.0);
					rows[c] |= 1u << row;
				}
			}
		}
		for (unsigned r : rows)
			if (r != (1u << LSC_N) - 1)
				return false;
		// The 16 sectors must cover the image.
		int sx = 0, sy = 0;
		for (int i = 0; i < 8; i++) {
			sx += 2 * x_sect[i];
			sy += 2 * y_sect[i];
		}
		return width > 0 && height > 0 && xs && ys && sx == width && sy == height;
	}

	// Grid point positions in pixels.
	static void points(const int sect[8], double pts[LSC_N])
	{
		pts[0] = 0;
		for (int i = 0; i < 16; i++)
			pts[i + 1] = pts[i] + sect[i < 8 ? i : 15 - i];
	}

	// Gain at a pixel position, bilinear between the grid points.
	double at(int c, double x, double y) const
	{
		double px[LSC_N], py[LSC_N];
		points(x_sect, px);
		points(y_sect, py);
		auto cell = [](const double *p, double v, int &i, double &t) {
			v = std::clamp(v, p[0], p[LSC_N - 1]);
			for (i = 0; i < LSC_N - 2 && v > p[i + 1]; i++)
				;
			t = (v - p[i]) / std::max(1.0, p[i + 1] - p[i]);
		};
		int i, j;
		double tx, ty;
		cell(px, x, i, tx);
		cell(py, y, j, ty);
		const auto &g = gain[c];
		return (g[j][i] * (1 - tx) + g[j][i + 1] * tx) * (1 - ty) +
		       (g[j + 1][i] * (1 - tx) + g[j + 1][i + 1] * tx) * ty;
	}
};

// Equal sectors for one half of the image, as kray-cam-lsc makes them.
static void lsc_sectors(int size, int sect[8])
{
	int half = size / 2;
	for (int i = 0; i < 8; i++)
		sect[i] = half / 8 + (i < half % 8 ? 1 : 0);
}

// The lens shading table for this sensor and size. A table made at this
// size is used as it is. Otherwise the largest table for the sensor is
// mapped onto the mode: a mode with the same aspect ratio is taken as
// binned or scaled (the whole field of view), any other as a centre crop.
static bool lsc_params(const Config &cfg, const std::string &sensor, int w, int h, isp2x_lsc_cfg &out)
{
	std::string prefix = cfg.lsc_dir + "/lsc-" + sensor + "-";
	LscTable t;
	std::string used = prefix + std::to_string(w) + "x" + std::to_string(h) + ".conf";
	if (!t.load(used)) {
		glob_t g{};
		used.clear();
		if (glob((prefix + "*x*.conf").c_str(), 0, nullptr, &g) == 0) {
			LscTable c;
			for (size_t i = 0; i < g.gl_pathc; i++)
				if (c.load(g.gl_pathv[i]) && c.width * c.height > t.width * t.height) {
					t = c;
					used = g.gl_pathv[i];
				}
		}
		globfree(&g);
		if (used.empty())
			return false;
	}
	bool same = t.width == w && t.height == h;
	bool scaled = !same && std::fabs(double(t.width) * h / (double(t.height) * w) - 1) < 0.01;
	if (!same && !scaled && (w > t.width || h > t.height)) {
		LOG("lsc: %s is %dx%d, smaller than the %dx%d mode; not used\n", used.c_str(), t.width, t.height, w, h);
		return false;
	}
	memset(&out, 0, sizeof(out));
	int xs[8], ys[8];
	lsc_sectors(w, xs);
	lsc_sectors(h, ys);
	if (same) {
		std::copy(t.x_sect, t.x_sect + 8, xs);
		std::copy(t.y_sect, t.y_sect + 8, ys);
	}
	for (int i = 0; i < 8; i++) {
		out.x_size_tbl[i] = xs[i];
		out.y_size_tbl[i] = ys[i];
		out.x_grad_tbl[i] = std::lround(32768.0 / std::max(1, xs[i]));
		out.y_grad_tbl[i] = std::lround(32768.0 / std::max(1, ys[i]));
	}
	double px[LSC_N], py[LSC_N];
	LscTable::points(xs, px);
	LscTable::points(ys, py);
	// The struct is packed: write the tables element by element.
	auto put = [&out](int c, int k, __u16 v) {
		switch (c) {
		case 0: out.r_data_tbl[k] = v; break;
		case 1: out.gr_data_tbl[k] = v; break;
		case 2: out.gb_data_tbl[k] = v; break;
		default: out.b_data_tbl[k] = v; break;
		}
	};
	for (int c = 0; c < 4; c++)
		for (int j = 0; j < LSC_N; j++)
			for (int i = 0; i < LSC_N; i++) {
				double x = px[i], y = py[j];
				if (scaled) {
					x = x * t.width / w;
					y = y * t.height / h;
				} else if (!same) {
					x += (t.width - w) / 2.0;
					y += (t.height - h) / 2.0;
				}
				double v = same ? t.gain[c][j][i] : t.at(c, x, y);
				put(c, j * LSC_N + i, std::clamp(std::lround(v * 1024), 1L, 8191L));
			}
	LOG("lsc: %s%s\n", used.c_str(), same ? "" : scaled ? " (scaled to the mode)" : " (centre crop of it)");
	return true;
}

// ------------------------------------------------- colour correction table

// Colour correction matrices for a few colour temperatures, and the colour
// of grey (R/G, B/G before white balance) along the colour temperatures, to
// tell the temperature from the white balance gains. Same layout as the
// Raspberry Pi tuning files ("ct_curve" and "ccms").
struct CcmTable {
	std::vector<double> ct, r, b; // grey curve
	std::vector<std::pair<double, std::vector<double>>> ccms; // ct, 3x3 row-major

	bool load(const std::string &path)
	{
		std::ifstream f(path);
		if (!f)
			return false;
		std::string line;
		while (std::getline(f, line)) {
			size_t h = line.find('#');
			if (h != std::string::npos)
				line.erase(h);
			size_t eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			std::istringstream ks(line.substr(0, eq)), vs(line.substr(eq + 1));
			std::string k;
			ks >> k;
			std::vector<double> v;
			for (double d; vs >> d;)
				v.push_back(d);
			if (k == "ct_curve" && v.size() >= 6 && v.size() % 3 == 0) {
				for (size_t i = 0; i < v.size(); i += 3) {
					ct.push_back(v[i]);
					r.push_back(v[i + 1]);
					b.push_back(v[i + 2]);
				}
			} else if (k == "ccm" && v.size() == 9) {
				double t;
				if (ks >> t)
					ccms.emplace_back(t, v);
			}
		}
		std::sort(ccms.begin(), ccms.end());
		return ct.size() >= 2 && !ccms.empty();
	}

	// Colour temperature of the grey that the gains make neutral: the
	// nearest point of the grey curve to (1 / red gain, 1 / blue gain).
	double temperature(double red, double blue) const
	{
		double pr = 1 / red, pb = 1 / blue, best = 1e9, t = ct[0];
		for (size_t i = 0; i + 1 < ct.size(); i++) {
			double dr = r[i + 1] - r[i], db = b[i + 1] - b[i];
			double u = std::clamp(((pr - r[i]) * dr + (pb - b[i]) * db) / std::max(1e-9, dr * dr + db * db), 0.0, 1.0);
			double er = r[i] + u * dr - pr, eb = b[i] + u * db - pb;
			if (er * er + eb * eb < best) {
				best = er * er + eb * eb;
				t = ct[i] + u * (ct[i + 1] - ct[i]);
			}
		}
		return t;
	}

	// The matrix for a colour temperature, linear between the table's.
	std::vector<double> matrix(double t) const
	{
		if (t <= ccms.front().first)
			return ccms.front().second;
		if (t >= ccms.back().first)
			return ccms.back().second;
		size_t i = 1;
		while (ccms[i].first < t)
			i++;
		double u = (t - ccms[i - 1].first) / (ccms[i].first - ccms[i - 1].first);
		std::vector<double> m(9);
		for (int k = 0; k < 9; k++)
			m[k] = ccms[i - 1].second[k] * (1 - u) + ccms[i].second[k] * u;
		return m;
	}
};

// ------------------------------------------- noise reduction / sharpening

// Register values of the ISP's colour noise reduction (CNR), luma noise
// reduction (YNR), sharpening and Bayer noise reduction (BAYNR, BAY3D)
// blocks for a grid of ISO values (ISO 50 = sensor gain 1), one line per
// block and ISO: "cnr 400 = v0 v1 ...", the fields of the kernel struct in
// declaration order. The Bayer rows are optional.
struct NrTable {
	enum { CNR, YNR, SHARP, BAYNR, BAY3D, N };
	std::map<int, std::vector<long>> rows[N];

	// Field sizes in bytes of isp21_cnr_cfg, isp21_ynr_cfg, isp21_sharp_cfg.
	static const std::vector<int> &widths(int b)
	{
		static const std::vector<int> w[N] = {
			expand({{1, 19}, {2, 5}}),
			expand({{1, 9}, {2, 12}, {1, 5}, {2, 6}, {1, 8}, {2, 51}, {1, 17}}),
			expand({{1, 12}, {2, 16}, {1, 2}, {2, 16}, {1, 9}}),
			expand({{1, 2}, {2, 43}}),
			expand({{1, 3}, {2, 2}, {4, 1}, {2, 4}, {2, 32}}),
		};
		return w[b];
	}

	static std::vector<int> expand(std::initializer_list<std::pair<int, int>> runs)
	{
		std::vector<int> v;
		for (auto &r : runs)
			v.insert(v.end(), r.second, r.first);
		return v;
	}

	bool load(const std::string &path)
	{
		std::ifstream f(path);
		if (!f)
			return false;
		static const char *const names[N] = {"cnr", "ynr", "sharp", "baynr", "bay3d"};
		std::string line;
		while (std::getline(f, line)) {
			size_t h = line.find('#');
			if (h != std::string::npos)
				line.erase(h);
			size_t eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			std::istringstream ks(line.substr(0, eq)), vs(line.substr(eq + 1));
			std::string k;
			int iso = 0;
			ks >> k >> iso;
			for (int b = 0; b < N; b++) {
				if (k != names[b] || iso <= 0)
					continue;
				std::vector<long> v;
				for (long d; vs >> d;)
					v.push_back(d);
				if (v.size() != widths(b).size())
					return false;
				rows[b][iso] = v;
			}
		}
		for (int b = CNR; b <= SHARP; b++)
			if (rows[b].empty())
				return false;
		return true;
	}

	bool has(int b) const { return !rows[b].empty(); }

	// The ISO of the table nearest to iso (on a log scale).
	int nearest(int b, double iso) const
	{
		int best = rows[b].begin()->first;
		for (auto &r : rows[b])
			if (std::fabs(std::log(r.first / iso)) < std::fabs(std::log(best / iso)))
				best = r.first;
		return best;
	}

	// Write a row into the packed kernel struct at dst (little-endian).
	void put(int b, int iso, uint8_t *dst) const
	{
		const std::vector<long> &v = rows[b].at(iso);
		const std::vector<int> &w = widths(b);
		for (size_t i = 0; i < v.size(); i++) {
			for (int k = 0; k < w[i]; k++)
				*dst++ = uint8_t(v[i] >> (8 * k));
		}
	}
};

// YNR's radial term: 1 / (half the image diagonal)^2 as mantissa and
// exponent (rkaiq ynr_fix_transfer_V2).
static __u16 ynr_rnr_max_r(int width, int height)
{
	double l = std::log2(16.0 / (double(width) * width + double(height) * height));
	int e = std::abs(int(std::ceil(l)));
	int m = int(std::floor(std::pow(2.0, l + e) * 256 + 0.5));
	return __u16(std::clamp((m << 5) + e, 0, 0x3fff));
}

// Rockchip sub-device naming "m00_b_imx219 1-0010": the sensor is "imx219".
static std::string sensor_model(const std::string &subdev)
{
	std::string nm = subdev.substr(0, subdev.find(' '));
	if (nm.size() > 6 && nm[0] == 'm' && nm[3] == '_' && nm[5] == '_')
		nm.erase(0, 6);
	return nm;
}

// ---------------------------------------------------------------------- 3A

class Engine {
public:
	explicit Engine(const Config &c) : cfg_(c), red_(c.wb_red), blue_(c.wb_blue) {}

	int run();

private:
	bool start();
	void stop();
	void fill_params(void *buf, size_t size, bool first);
	// Full scale over full scale minus the subtracted black level (12-bit).
	double headroom() const
	{
		if (!cfg_.bls)
			return 1.0;
		long v = cfg_.bls_value >= 0 ? cfg_.bls_value : std::lround(cfg_.black * 4096);
		return 4096.0 / std::max(2048L, 4096 - v);
	}
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
	bool lsc_ = false;
	isp2x_lsc_cfg lsc_cfg_{};
	bool ccm_ = false;
	CcmTable ccm_table_;
	double ccm_ct_ = 0; // colour temperature of the matrix in use
	bool nr_ = false;
	NrTable nr_table_;
	int nr_iso_ = 0;    // ISO of the noise reduction settings in use
	bool bayer_ = false, bay3d_ = false; // Bayer 2D / temporal noise reduction on
};

void Engine::fill_params(void *buf, size_t size, bool first)
{
	memset(buf, 0, size);
	auto *p = static_cast<isp21_isp_params_cfg *>(buf);
	p->module_en_update = ISP2X_MODULE_AWB_GAIN;
	p->module_ens = ISP2X_MODULE_AWB_GAIN;
	p->module_cfg_update = ISP2X_MODULE_AWB_GAIN;
	isp21_awb_gain_cfg &g = p->others.awb_gain_cfg;
	// After the black level subtraction a saturated pixel is below full
	// scale; all four gains carry the headroom factor that brings it back
	// up, so that clipped areas come out white. Without it the red and
	// blue gains push clipped highlights above the clipped green
	// (magenta), most visibly in the centre, where lens shading
	// correction adds no gain.
	const double k = headroom();
	int r = std::lround(red_ * k * 256), b = std::lround(blue_ * k * 256), gr = std::lround(k * 256);
	g.gain0_red = g.gain1_red = g.gain2_red = r;
	g.gain0_blue = g.gain1_blue = g.gain2_blue = b;
	g.gain0_green_r = g.gain1_green_r = g.gain2_green_r = gr;
	g.gain0_green_b = g.gain1_green_b = g.gain2_green_b = gr;
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
		if (lsc_ && offsetof(isp21_isp_params_cfg, others.lsc_cfg) + sizeof(isp2x_lsc_cfg) <= size) {
			p->module_en_update |= ISP2X_MODULE_LSC;
			p->module_ens |= ISP2X_MODULE_LSC;
			p->module_cfg_update |= ISP2X_MODULE_LSC;
			p->others.lsc_cfg = lsc_cfg_;
		}
	}
	if (ccm_ && offsetof(isp21_isp_params_cfg, others.ccm_cfg) + sizeof(isp21_ccm_cfg) <= size) {
		// The ISP's matrix works on linear RGB after white balance. It
		// adds alpha x C x RGB to the input, with alpha from a curve over
		// the luma (alp_y, 1024 = 1.0) and C in units of CCM_ONE, so a
		// matrix M is programmed as C = M - I with alpha 1 everywhere
		// (measured on the RK3566: C = 0 leaves the picture unchanged,
		// alp_y = 0 switches the matrix off). Rows of the table's matrices
		// add up to 1, so a saturation below 1 scales C down towards the
		// identity and greys stay grey.
		ccm_ct_ = ccm_table_.temperature(red_, blue_);
		std::vector<double> m = ccm_table_.matrix(ccm_ct_);
		auto c = [&](int k) {
			double v = (m[k] - (k % 4 == 0 ? 1.0 : 0.0)) * cfg_.saturation;
			return __s16(std::clamp(std::lround(v * CCM_ONE), -CCM_LIMIT, CCM_LIMIT));
		};
		p->module_en_update |= ISP2X_MODULE_CCM;
		p->module_ens |= ISP2X_MODULE_CCM;
		p->module_cfg_update |= ISP2X_MODULE_CCM;
		isp21_ccm_cfg &cc = p->others.ccm_cfg;
		cc.highy_adjust_dis = 1;
		cc.coeff0_r = c(0), cc.coeff1_r = c(1), cc.coeff2_r = c(2);
		cc.coeff0_g = c(3), cc.coeff1_g = c(4), cc.coeff2_g = c(5);
		cc.coeff0_b = c(6), cc.coeff1_b = c(7), cc.coeff2_b = c(8);
		// Luma weights for the highlight adjustment (off), BT.601.
		cc.coeff0_y = std::lround(0.299 * CCM_ONE);
		cc.coeff1_y = std::lround(0.587 * CCM_ONE);
		cc.coeff2_y = std::lround(0.114 * CCM_ONE);
		for (int i = 0; i < ISP21_DHAZ_ENH_CURVE_NUM; i++)
			cc.alp_y[i] = 1024;
	}
	if (nr_) {
		// The driver declares the ISP21 parameter buffer with the size of
		// the ISP20 struct (10418 bytes, the ISP21 one is 10475), so the
		// sharpening block lies past it. The buffer is allocated in whole
		// pages and mapped so, and the driver reads the full ISP21 struct.
		const size_t mapped = (size + 4095) & ~size_t(4095);
		uint8_t *base = static_cast<uint8_t *>(buf);
		// The three blocks work as a set: with only some of them enabled
		// the picture gets a colour cast and stripes (measured), so all
		// three stay enabled, as rkaiq does, and switching one off is its
		// bypass bits.
		struct Block {
			int table;
			uint64_t module;
			size_t off, len;
		} blocks[] = {
			{NrTable::CNR, ISP2X_MODULE_CNR, offsetof(isp21_isp_params_cfg, others.cnr_cfg), sizeof(isp21_cnr_cfg)},
			{NrTable::YNR, ISP2X_MODULE_YNR, offsetof(isp21_isp_params_cfg, others.ynr_cfg), sizeof(isp21_ynr_cfg)},
			{NrTable::SHARP, ISP2X_MODULE_SHARP, offsetof(isp21_isp_params_cfg, others.sharp_cfg), sizeof(isp21_sharp_cfg)},
		};
		if (offsetof(isp21_isp_params_cfg, others.sharp_cfg) + sizeof(isp21_sharp_cfg) <= mapped) {
			for (const Block &b : blocks) {
				nr_table_.put(b.table, nr_table_.nearest(b.table, nr_iso_), base + b.off);
				p->module_en_update |= b.module;
				p->module_ens |= b.module;
				p->module_cfg_update |= b.module;
			}
			isp21_ynr_cfg &y = p->others.ynr_cfg;
			y.sw_ynr_rnr_max_r = ynr_rnr_max_r(sensor_.width, sensor_.height);
			if (!cfg_.denoise) {
				isp21_cnr_cfg &c = p->others.cnr_cfg;
				c.sw_cnr_lq_bila_bypass = c.sw_cnr_hq_bila_bypass = c.sw_cnr_exgain_bypass = 1;
				y.sw_ynr_sft5x5_bypass = y.sw_ynr_flt1x1_bypass = y.sw_ynr_lgft3x3_bypass = 1;
				y.sw_ynr_lbft5x5_bypass = y.sw_ynr_bft3x3_bypass = 1;
			}
			if (!cfg_.sharpen)
				p->others.sharp_cfg.sw_sharp_bypass = 1;
		}
		// Bayer-domain noise reduction: the 2D filter, and the temporal
		// one with it (rkaiq enables the 2D block whenever the temporal
		// one runs). The driver allocates the temporal filter's frame
		// buffer only when the first parameters enable it, so that is
		// decided at stream start (bay3d_).
		if (bayer_) {
			nr_table_.put(NrTable::BAYNR, nr_table_.nearest(NrTable::BAYNR, nr_iso_),
				      base + offsetof(isp21_isp_params_cfg, others.baynr_cfg));
			p->module_en_update |= ISP2X_MODULE_BAYNR;
			p->module_ens |= ISP2X_MODULE_BAYNR;
			p->module_cfg_update |= ISP2X_MODULE_BAYNR;
		}
		if (bay3d_) {
			nr_table_.put(NrTable::BAY3D, nr_table_.nearest(NrTable::BAY3D, nr_iso_),
				      base + offsetof(isp21_isp_params_cfg, others.bay3d_cfg));
			p->module_en_update |= ISP2X_MODULE_BAY3D;
			p->module_ens |= ISP2X_MODULE_BAY3D;
			p->module_cfg_update |= ISP2X_MODULE_BAY3D;
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
	lsc_ = false;
	std::string nm = sensor_model(sensor_.name);
	if (cfg_.lsc) {
		lsc_ = lsc_params(cfg_, nm, sensor_.width, sensor_.height, lsc_cfg_);
		if (!lsc_)
			LOG("lsc: no table for %s %dx%d in %s (kray-cam-lsc makes one)\n", nm.c_str(), sensor_.width,
			    sensor_.height, cfg_.lsc_dir.c_str());
	}
	ccm_ = false;
	if (cfg_.ccm) {
		std::string path = cfg_.lsc_dir + "/ccm-" + nm + ".conf";
		ccm_table_ = CcmTable();
		ccm_ = ccm_table_.load(path);
		if (ccm_)
			LOG("ccm: %s (%zu matrices, saturation %.2f)\n", path.c_str(), ccm_table_.ccms.size(), cfg_.saturation);
		else
			LOG("ccm: no table %s\n", path.c_str());
	}
	nr_ = bayer_ = bay3d_ = false;
	if (cfg_.denoise || cfg_.sharpen || cfg_.bayer_denoise || cfg_.temporal_denoise) {
		std::string path = cfg_.lsc_dir + "/nr-" + nm + ".conf";
		nr_table_ = NrTable();
		nr_ = nr_table_.load(path);
		nr_iso_ = 50 * sensor_.factor(sensor_.gain);
		bayer_ = nr_ && (cfg_.bayer_denoise || cfg_.temporal_denoise) && nr_table_.has(NrTable::BAYNR);
		bay3d_ = bayer_ && cfg_.temporal_denoise && nr_table_.has(NrTable::BAY3D);
		if (nr_)
			LOG("nr: %s (denoise %s, sharpen %s, bayer %s, temporal %s)\n", path.c_str(),
			    cfg_.denoise ? "on" : "off", cfg_.sharpen ? "on" : "off", bayer_ ? "on" : "off",
			    bay3d_ ? "on" : "off");
		else
			LOG("nr: no table %s\n", path.c_str());
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
	if (nr_) {
		// Noise reduction follows the gain: ISO 50 = gain 1.
		int iso = int(50 * s.factor(s.gain));
		if (nr_table_.nearest(NrTable::YNR, iso) != nr_table_.nearest(NrTable::YNR, nr_iso_)) {
			nr_iso_ = iso;
			if (send_params(false))
				DBG("nr: ISO %d\n", nr_table_.nearest(NrTable::YNR, iso));
		}
	}
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
			DBG("awb: %d windows, R %.2f B %.2f, %.0f K\n", n, red_, blue_, ccm_ ? ccm_ct_ : 0.0);
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
