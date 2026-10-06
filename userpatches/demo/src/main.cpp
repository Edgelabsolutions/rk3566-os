// SPDX-License-Identifier: MIT
// kray-demo: exhibition demo for Kray-SoM-RKC. Shows the camera on HDMI,
// detects and counts people with YOLOv8 on the NPU, and shows CPU/NPU load,
// frame rates, SoC temperature and memory use next to the picture.
//
// Data path: ISP (NV12, dmabuf) -> RGA -> display buffer and -> NPU input.
// The CPU only draws boxes and, twice a second, the statistics panel.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <linux/videodev2.h>
#include <rga/im2d.hpp>
#include <rga/RgaUtils.h>

#include "camera.h"
#include "canvas.h"
#include "detector.h"
#include "display.h"
#include "stats.h"

using Clock = std::chrono::steady_clock;

static std::atomic<bool> g_run{true};
static void on_signal(int) { g_run = false; }

// ---------------------------------------------------------------- config

struct Config {
	std::string model = "/usr/share/kray-demo/yolov8n-rk3566.rknn";
	std::string font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
	std::string font_bold = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
	std::string sensor = "imx219";
	std::string title = "Kray-SoM-RKC";
	std::string subtitle = "Rockchip RK3566 · Edge AI demo";
	std::string footer1 = "YOLOv8n INT8 on the RK3566 NPU";
	std::string footer2 = "Kray OS · Debian 13 · Linux 6.1";
	int cam_w = 1920, cam_h = 1080;
	float threshold = 0.40f;
	bool mirror = true;
	bool rotate180 = false;
	int target_luma = 110;
	int max_exposure = 1760; // lines; 1766-line frame in the 1920x1080 30 fps mode
	int max_gain = 10912;    // 10.66x analogue x 4x digital (IMX219 driver units)
	bool awb = true;         // automatic white balance (grey world on near-neutral pixels)
	int wb_red = 400;        // ISP white balance gains, 256 = 1.0: start values,
	int wb_blue = 400;       // or fixed values with awb = 0
};

static void load_config(const char *path, Config &c)
{
	std::ifstream f(path);
	std::string line;
	while (std::getline(f, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string k = line.substr(0, eq), v = line.substr(eq + 1);
		auto trim = [](std::string &s) {
			s.erase(0, s.find_first_not_of(" \t\""));
			s.erase(s.find_last_not_of(" \t\"\r") + 1);
		};
		trim(k);
		trim(v);
		if (k == "model") c.model = v;
		else if (k == "font") c.font = v;
		else if (k == "font_bold") c.font_bold = v;
		else if (k == "sensor") c.sensor = v;
		else if (k == "title") c.title = v;
		else if (k == "subtitle") c.subtitle = v;
		else if (k == "footer1") c.footer1 = v;
		else if (k == "footer2") c.footer2 = v;
		else if (k == "camera_width") c.cam_w = atoi(v.c_str());
		else if (k == "camera_height") c.cam_h = atoi(v.c_str());
		else if (k == "threshold") c.threshold = atof(v.c_str());
		else if (k == "mirror") c.mirror = v == "1" || v == "yes" || v == "true";
		else if (k == "rotate180") c.rotate180 = v == "1" || v == "yes" || v == "true";
		else if (k == "target_luma") c.target_luma = atoi(v.c_str());
		else if (k == "max_exposure") c.max_exposure = atoi(v.c_str());
		else if (k == "max_gain") c.max_gain = atoi(v.c_str());
		else if (k == "awb") c.awb = v == "1" || v == "yes" || v == "true";
		else if (k == "wb_red") c.wb_red = atoi(v.c_str());
		else if (k == "wb_blue") c.wb_blue = atoi(v.c_str());
	}
}

// ---------------------------------------------------------------- auto exposure

// The image has no rkaiq, so exposure and gain are set here from the mean
// luma of the picture: exposure first (up to one frame), then gain.
class AutoExposure {
public:
	void start(const Config &cfg)
	{
		cfg_ = cfg;
		thread_ = std::thread(&AutoExposure::run, this);
	}
	void stop()
	{
		stop_ = true;
		if (thread_.joinable())
			thread_.join();
	}
	void feed(float luma) { luma_ = luma; }

private:
	void run()
	{
		Sensor s;
		int emin = 4, emax = cfg_.max_exposure, gmin = 256, gmax = cfg_.max_gain;
		int exposure = 0, gain = 0;
		double total = 0;
		while (!stop_) {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			if (!s.is_open()) {
				if (!s.open(cfg_.sensor))
					continue;
				int a, b;
				if (s.range(V4L2_CID_EXPOSURE, a, b))
					emin = std::max(a, 1), emax = std::min(b, cfg_.max_exposure);
				if (s.range(V4L2_CID_GAIN, a, b))
					gmin = a, gmax = std::min(b, cfg_.max_gain);
				exposure = emax;
				gain = std::min(gmax, 2728);
				total = double(exposure) * gain;
				s.set(V4L2_CID_EXPOSURE, exposure);
				s.set(V4L2_CID_GAIN, gain);
				fprintf(stderr, "ae: sensor %s, exposure %d..%d, gain %d..%d\n", cfg_.sensor.c_str(), emin,
					emax, gmin, gmax);
				continue;
			}
			float m = luma_.exchange(-1);
			if (m < 0 || std::fabs(m - cfg_.target_luma) < 10)
				continue;
			double f = std::pow(cfg_.target_luma / std::max(m, 1.0f), 0.7);
			f = std::min(2.0, std::max(0.5, f));
			total = std::min(double(emax) * gmax, std::max(double(emin) * gmin, total * f));
			int e = int(std::min<double>(emax, std::max<double>(emin, total / gmin)));
			int g = int(std::min<double>(gmax, std::max<double>(gmin, total / e)));
			if (e != exposure)
				s.set(V4L2_CID_EXPOSURE, e);
			if (g != gain)
				s.set(V4L2_CID_GAIN, g);
			exposure = e;
			gain = g;
		}
	}

	Config cfg_;
	std::thread thread_;
	std::atomic<bool> stop_{false};
	std::atomic<float> luma_{-1};
};

static float mean_luma(const Camera &cam, int idx)
{
	const uint8_t *y = static_cast<const uint8_t *>(cam.buffer(idx).map);
	int w = cam.width(), h = cam.height(), st = cam.stride();
	unsigned long sum = 0, n = 0;
	for (int gy = 1; gy < 18; gy++)
		for (int gx = 1; gx < 32; gx++) {
			sum += y[size_t(h * gy / 18) * st + w * gx / 32];
			n++;
		}
	return n ? float(sum) / n : -1;
}

// Automatic white balance. Without rkaiq the ISP runs with unit colour gains,
// and the Bayer sensor's two green pixels per cell give the picture a green
// cast. This loop compares the mean chroma (Cb, Cr) of near-neutral pixels
// with grey and adjusts the ISP red and blue gains until they match.
class WhiteBalance {
public:
	void init(const Config &cfg)
	{
		red_ = cfg.wb_red;
		blue_ = cfg.wb_blue;
		enabled_ = cfg.awb;
	}
	int red() const { return int(red_); }
	int blue() const { return int(blue_); }

	// Returns true if the gains changed.
	bool update(const Camera &cam, int idx)
	{
		if (!enabled_)
			return false;
		const uint8_t *y = static_cast<const uint8_t *>(cam.buffer(idx).map);
		int w = cam.width(), h = cam.height(), st = cam.stride();
		const uint8_t *uv = y + size_t(st) * h;
		double su = 0, sv = 0;
		int n = 0;
		for (int gy = 1; gy < 36; gy++)
			for (int gx = 1; gx < 64; gx++) {
				int x = (w * gx / 64) & ~1, yy = h * gy / 36;
				int luma = y[size_t(yy) * st + x];
				const uint8_t *c = uv + size_t(yy / 2) * st + x;
				int u = c[0] - 128, v = c[1] - 128;
				// Skip dark, clipped and strongly coloured pixels.
				if (luma < 40 || luma > 230 || std::abs(u) + std::abs(v) > 50)
					continue;
				su += u;
				sv += v;
				n++;
			}
		if (n < 100)
			return false;
		double mu = su / n, mv = sv / n;
		if (std::fabs(mu) < 2 && std::fabs(mv) < 2)
			return false;
		// Cr follows R - Y, Cb follows B - Y. Small steps: the result of a
		// change shows a few frames later.
		mu = std::min(15.0, std::max(-15.0, mu));
		mv = std::min(15.0, std::max(-15.0, mv));
		red_ = std::min(768.0, std::max(256.0, red_ * std::exp(-0.01 * mv)));
		blue_ = std::min(768.0, std::max(256.0, blue_ * std::exp(-0.01 * mu)));
		return true;
	}

private:
	double red_ = 400, blue_ = 400;
	bool enabled_ = true;
};

// ---------------------------------------------------------------- layout and drawing

namespace col {
const uint32_t bg = rgb(13, 17, 23);
const uint32_t panel = rgb(22, 27, 34);
const uint32_t line = rgb(48, 54, 61);
const uint32_t text = rgb(230, 237, 243);
const uint32_t muted = rgb(139, 148, 158);
const uint32_t people = rgb(61, 220, 151);
const uint32_t cpu = rgb(56, 189, 248);
const uint32_t npu = rgb(167, 139, 250);
const uint32_t hot = rgb(248, 113, 113);
} // namespace col

static uint32_t dim(uint32_t c, float k)
{
	return rgb(uint8_t(((c >> 16) & 0xff) * k), uint8_t(((c >> 8) & 0xff) * k), uint8_t((c & 0xff) * k));
}

struct Rect {
	int x, y, w, h;
};

struct Layout {
	float s;
	Rect video, panel, strip;
	void compute(int W, int H)
	{
		s = H / 1080.0f;
		int pw = (W * 480 / 1920) & ~1;
		int vw = (W - pw) & ~1;
		int vh = (vw * 9 / 16) & ~1;
		if (vh > H - int(200 * s)) {
			vh = (H - int(200 * s)) & ~1;
			vw = (vh * 16 / 9) & ~1;
			pw = W - vw;
		}
		video = {0, 0, vw, vh};
		panel = {vw, 0, W - vw, H};
		strip = {0, vh, vw, H - vh};
	}
};

struct UiState {
	SysStats sys;
	float npu_share = 0; // fallback when the driver does not report NPU load
	float cam_fps = 0, npu_fps = 0, infer_ms = 0;
	int people = 0, max_today = 0;
	bool camera_ok = false, model_ok = false;
	std::deque<float> cpu_hist, npu_hist;
	float npu() const { return sys.npu >= 0 ? sys.npu : npu_share; }
};

static const size_t kHist = 120; // 60 s at 2 Hz

static std::string fmt(const char *f, double v)
{
	char b[64];
	snprintf(b, sizeof(b), f, v);
	return b;
}

static void draw_chart(Canvas &c, Font &reg, Font &bold, float s, Rect r, const char *name, float value,
		       const std::deque<float> &hist, uint32_t color, const std::vector<float> *cores)
{
	fill_rect(c, r.x, r.y, r.w, r.h, col::bg);
	int pad = int(24 * s);
	int head = int(52 * s);
	int x = r.x + pad;
	int ty = r.y + pad + bold.ascent(int(30 * s));
	x += bold.draw(c, x, ty, int(30 * s), col::text, col::bg, name);
	bold.draw(c, x + int(14 * s), ty, int(30 * s), color, col::bg, fmt("%.0f%%", value * 100));

	if (cores && !cores->empty()) {
		// One small bar per core, right-aligned in the header.
		int bw = int(14 * s), gap = int(8 * s), bh = int(30 * s);
		int n = int(cores->size());
		int bx = r.x + r.w - pad - n * bw - (n - 1) * gap;
		int lw = reg.width(int(18 * s), "per core");
		reg.draw(c, bx - lw - int(12 * s), ty, int(18 * s), col::muted, col::bg, "per core");
		for (int i = 0; i < n; i++) {
			int x0 = bx + i * (bw + gap), y0 = r.y + pad;
			fill_rect(c, x0, y0, bw, bh, col::line);
			int fh = int(bh * std::min(1.0f, (*cores)[i]));
			fill_rect(c, x0, y0 + bh - fh, bw, fh, color);
		}
	}

	Rect g{r.x + pad, r.y + pad + head, r.w - 2 * pad, r.h - 2 * pad - head};
	if (g.h < 10)
		return;
	fill_rect(c, g.x, g.y, g.w, g.h, col::panel);
	for (int q = 1; q < 4; q++)
		fill_rect(c, g.x, g.y + g.h * q / 4, g.w, 1, col::line);
	size_t n = hist.size();
	uint32_t area = dim(color, 0.35f);
	for (size_t i = 0; i < n; i++) {
		int x0 = g.x + int(g.w * (kHist - n + i) / kHist);
		int x1 = g.x + int(g.w * (kHist - n + i + 1) / kHist);
		int hgt = int(g.h * std::min(1.0f, std::max(0.0f, hist[i])));
		int top = g.y + g.h - hgt;
		fill_rect(c, x0, top, x1 - x0, hgt, area);
		fill_rect(c, x0, top, x1 - x0, std::max(2, int(3 * s)), color);
	}
	reg.draw(c, g.x + int(8 * s), g.y + int(22 * s), int(16 * s), col::muted, col::panel, "last 60 s");
}

static void draw_row(Canvas &c, Font &reg, Font &bold, float s, int x, int w, int y, const std::string &label,
		     const std::string &value, uint32_t vcol)
{
	reg.draw(c, x, y, int(24 * s), col::muted, col::panel, label);
	int vw = bold.width(int(26 * s), value);
	bold.draw(c, x + w - vw, y, int(26 * s), vcol, col::panel, value);
}

static void draw_chrome(Canvas &c, Font &reg, Font &bold, const Layout &L, const Config &cfg, const UiState &u)
{
	float s = L.s;
	const Rect &p = L.panel;
	fill_rect(c, p.x, p.y, p.w, p.h, col::panel);
	fill_rect(c, p.x, p.y, std::max(1, int(2 * s)), p.h, col::line);
	int pad = int(36 * s), x = p.x + pad, w = p.w - 2 * pad;
	int y = p.y + pad + bold.ascent(int(44 * s));
	bold.draw(c, x, y, int(44 * s), col::text, col::panel, cfg.title);
	y += int(40 * s);
	reg.draw(c, x, y, int(22 * s), col::muted, col::panel, cfg.subtitle);
	y += int(34 * s);
	fill_rect(c, x, y, w, 1, col::line);

	y += int(56 * s);
	reg.draw(c, x, y, int(22 * s), col::muted, col::panel, "PEOPLE IN VIEW");
	y += int(10 * s) + bold.ascent(int(150 * s));
	bold.draw(c, x - int(6 * s), y, int(150 * s), u.model_ok ? col::people : col::muted, col::panel,
		  u.model_ok ? std::to_string(u.people) : "–");
	y += int(52 * s);
	reg.draw(c, x, y, int(24 * s), col::muted, col::panel, "Max today");
	bold.draw(c, x + reg.width(int(24 * s), "Max today") + int(12 * s), y, int(24 * s), col::text, col::panel,
		  std::to_string(u.max_today));
	y += int(34 * s);
	fill_rect(c, x, y, w, 1, col::line);

	y += int(54 * s);
	draw_row(c, reg, bold, s, x, w, y, "Camera", fmt("%.1f fps", u.cam_fps), col::text);
	y += int(48 * s);
	draw_row(c, reg, bold, s, x, w, y, "NPU inference", fmt("%.1f fps", u.npu_fps), col::npu);
	y += int(48 * s);
	draw_row(c, reg, bold, s, x, w, y, "Inference time", fmt("%.0f ms", u.infer_ms), col::npu);
	y += int(48 * s);
	draw_row(c, reg, bold, s, x, w, y, "CPU load", fmt("%.0f %%", u.sys.cpu_total * 100), col::cpu);
	y += int(48 * s);
	draw_row(c, reg, bold, s, x, w, y, "SoC temperature",
		 u.sys.temp_c < 0 ? std::string("–") : fmt("%.1f °C", u.sys.temp_c),
		 u.sys.temp_c > 75 ? col::hot : col::text);
	y += int(48 * s);
	char mem[64];
	snprintf(mem, sizeof(mem), "%d / %d MB", u.sys.ram_used_mb, u.sys.ram_total_mb);
	draw_row(c, reg, bold, s, x, w, y, "Memory", mem, col::text);

	int fy = p.y + p.h - pad;
	reg.draw(c, x, fy, int(18 * s), col::muted, col::panel, cfg.footer2);
	reg.draw(c, x, fy - int(28 * s), int(18 * s), col::muted, col::panel, cfg.footer1);

	Rect half1{L.strip.x, L.strip.y, L.strip.w / 2, L.strip.h};
	Rect half2{L.strip.x + L.strip.w / 2, L.strip.y, L.strip.w - L.strip.w / 2, L.strip.h};
	draw_chart(c, reg, bold, s, half1, "CPU", u.sys.cpu_total, u.cpu_hist, col::cpu, &u.sys.cpu_core);
	draw_chart(c, reg, bold, s, half2, "NPU", u.npu(), u.npu_hist, col::npu, nullptr);
	fill_rect(c, half2.x, half2.y + int(20 * s), 1, half2.h - int(40 * s), col::line);

	if (!u.camera_ok) {
		const Rect &v = L.video;
		fill_rect(c, v.x, v.y, v.w, v.h, col::bg);
		std::string msg = "Waiting for the camera…";
		int tw = reg.width(int(36 * s), msg);
		reg.draw(c, v.x + (v.w - tw) / 2, v.y + v.h / 2, int(36 * s), col::muted, col::bg, msg);
	}
}

static void draw_boxes(Canvas &c, Font &bold, const Layout &L, const Config &cfg, int cw, int ch,
		       const std::vector<Detection> &dets)
{
	const Rect &v = L.video;
	float s = L.s;
	int t = std::max(2, int(4 * s));
	int fs = int(22 * s);
	for (const auto &d : dets) {
		float x1 = d.x1, x2 = d.x2, y1 = d.y1, y2 = d.y2;
		if (cfg.mirror != cfg.rotate180) {
			x1 = cw - d.x2;
			x2 = cw - d.x1;
		}
		if (cfg.rotate180) {
			y1 = ch - d.y2;
			y2 = ch - d.y1;
		}
		int bx = v.x + int(std::max(0.0f, x1) * v.w / cw);
		int by = v.y + int(std::max(0.0f, y1) * v.h / ch);
		int bx2 = v.x + int(std::min(float(cw), x2) * v.w / cw);
		int by2 = v.y + int(std::min(float(ch), y2) * v.h / ch);
		if (bx2 - bx < 2 * t || by2 - by < 2 * t)
			continue;
		frame_rect(c, bx, by, bx2 - bx, by2 - by, t, col::people);
		std::string label = fmt("person %.0f%%", d.score * 100);
		int lw = bold.width(fs, label) + int(16 * s), lh = int(32 * s);
		int ly = by >= lh ? by - lh : by;
		if (bx + lw > v.x + v.w)
			continue;
		fill_rect(c, bx, ly, lw, lh, col::people);
		bold.draw(c, bx + int(8 * s), ly + int(24 * s), fs, col::bg, col::people, label);
	}
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	Config cfg;
	load_config(argc > 1 ? argv[1] : "/etc/kray-demo.conf", cfg);

	Font reg, bold;
	if (!reg.load(cfg.font) || !bold.load(cfg.font_bold))
		fprintf(stderr, "kray-demo: cannot load fonts %s / %s\n", cfg.font.c_str(), cfg.font_bold.c_str());

	Display disp;
	Camera cam;
	Detector det;
	StatsReader stats;
	AutoExposure ae;
	WhiteBalance wb;
	UiState ui;
	Layout L{};

	ui.model_ok = det.init(cfg.model, cfg.cam_w, cfg.cam_h, cfg.threshold);
	ae.start(cfg);
	wb.init(cfg);

	std::vector<Detection> dets;
	std::deque<int> recent_counts;
	unsigned last_seq = 0, frames = 0, npu_done_mark = 0;
	int chrome_version = 1, buf_version[2] = {0, 0};
	int rga_disp[2] = {0, 0};
	int today = -1;
	auto t_stats = Clock::now(), t_fps = Clock::now(), t_retry = Clock::now() - std::chrono::seconds(10),
	     t_log = Clock::now();

	while (g_run) {
		auto now = Clock::now();

		// Display: (re)open when a monitor is connected.
		if (rga_disp[0] == 0) {
			if (now - t_retry < std::chrono::seconds(2)) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
			t_retry = now;
			if (!disp.open())
				continue;
			L.compute(disp.width(), disp.height());
			bool ok = true;
			for (int i = 0; i < 2; i++) {
				Display::Buffer &b = disp.back();
				rga_disp[disp.back_index()] = importbuffer_fd(b.dmafd, int(b.pitch / 4), disp.height(),
									       RK_FORMAT_BGRX_8888);
				ok = ok && rga_disp[disp.back_index()];
				Canvas c{b.map, disp.width(), disp.height(), int(b.pitch / 4)};
				fill_rect(c, 0, 0, c.w, c.h, col::bg);
				if (!disp.flip())
					ok = false;
			}
			buf_version[0] = buf_version[1] = 0;
			if (!ok) {
				fprintf(stderr, "kray-demo: display setup failed\n");
				for (int &h : rga_disp)
					if (h)
						releasebuffer_handle(h), h = 0;
				disp.close();
				continue;
			}
		}

		// Camera: (re)open if needed.
		if (!cam.is_open() && now - t_retry > std::chrono::seconds(3)) {
			t_retry = now;
			if (cam.open(cfg.cam_w, cfg.cam_h, wb.red(), wb.blue())) {
				ui.camera_ok = true;
				chrome_version++;
			}
		}

		Display::Buffer &b = disp.back();
		Canvas canvas{b.map, disp.width(), disp.height(), int(b.pitch / 4)};
		int idx = cam.is_open() ? cam.dequeue(1000) : -1;
		if (idx >= 0) {
			frames++;
			if (frames % 3 == 0)
				ae.feed(mean_luma(cam, idx));
			if (frames % 6 == 1 && cam.has_wb() && wb.update(cam, idx))
				cam.set_wb(wb.red(), wb.blue());
			const Camera::Buffer &cb = cam.buffer(idx);
			rga_buffer_handle_t sh =
				importbuffer_fd(cb.dmafd, cam.stride(), cam.height(), RK_FORMAT_YCbCr_420_SP);
			if (sh) {
				rga_buffer_t src = wrapbuffer_handle(sh, cam.width(), cam.height(), RK_FORMAT_YCbCr_420_SP,
								     cam.stride(), cam.height());
				src.color_space_mode = IM_YUV_TO_RGB_BT601_FULL;
				rga_buffer_t dst = wrapbuffer_handle(rga_disp[disp.back_index()], disp.width(),
								     disp.height(), RK_FORMAT_BGRX_8888, int(b.pitch / 4),
								     disp.height());
				rga_buffer_t pat{};
				int usage = IM_SYNC;
				if (cfg.mirror && cfg.rotate180)
					usage |= IM_HAL_TRANSFORM_FLIP_V;
				else if (cfg.mirror)
					usage |= IM_HAL_TRANSFORM_FLIP_H;
				else if (cfg.rotate180)
					usage |= IM_HAL_TRANSFORM_ROT_180;
				im_rect sr{0, 0, cam.width(), cam.height()};
				im_rect dr{L.video.x, L.video.y, L.video.w, L.video.h};
				im_rect pr{};
				IM_STATUS st = improcess(src, dst, pat, sr, dr, pr, -1, nullptr, nullptr, usage);
				if (st != IM_STATUS_SUCCESS)
					fprintf(stderr, "kray-demo: RGA video: %s\n", imStrError(st));
				releasebuffer_handle(sh);
			}
			if (ui.model_ok)
				det.submit(cb.dmafd, cam.width(), cam.height(), cam.stride());
			cam.queue(idx);
		} else if (cam.is_open() && cam.failed()) {
			fprintf(stderr, "kray-demo: camera stopped delivering frames, reopening\n");
			cam.close();
			ui.camera_ok = false;
			chrome_version++;
		}

		// Detection results.
		unsigned seq;
		float infer_ms;
		std::vector<Detection> latest;
		det.latest(latest, seq, infer_ms);
		if (seq != last_seq) {
			last_seq = seq;
			dets = latest;
			ui.infer_ms = infer_ms;
			recent_counts.push_back(int(dets.size()));
			if (recent_counts.size() > 5)
				recent_counts.pop_front();
			std::vector<int> sorted(recent_counts.begin(), recent_counts.end());
			std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
			int people = sorted[sorted.size() / 2];
			time_t tt = time(nullptr);
			struct tm tm;
			localtime_r(&tt, &tm);
			if (tm.tm_yday != today) {
				today = tm.tm_yday;
				ui.max_today = 0;
				chrome_version++;
			}
			if (people != ui.people || people > ui.max_today) {
				ui.people = people;
				ui.max_today = std::max(ui.max_today, people);
				chrome_version++;
			}
		}
		if (!ui.camera_ok)
			dets.clear();

		// Statistics twice a second.
		if (now - t_stats >= std::chrono::milliseconds(500)) {
			float dt = std::chrono::duration<float>(now - t_fps).count();
			t_stats = now;
			if (dt >= 1.0f) {
				ui.cam_fps = frames / dt;
				unsigned done = det.completed();
				ui.npu_fps = (done - npu_done_mark) / dt;
				npu_done_mark = done;
				frames = 0;
				t_fps = now;
			}
			stats.sample(ui.sys);
			ui.npu_share = det.busy_share();
			ui.cpu_hist.push_back(ui.sys.cpu_total);
			ui.npu_hist.push_back(ui.npu());
			while (ui.cpu_hist.size() > kHist)
				ui.cpu_hist.pop_front();
			while (ui.npu_hist.size() > kHist)
				ui.npu_hist.pop_front();
			chrome_version++;
		}

		if (buf_version[disp.back_index()] != chrome_version) {
			draw_chrome(canvas, reg, bold, L, cfg, ui);
			buf_version[disp.back_index()] = chrome_version;
		}
		if (ui.camera_ok)
			draw_boxes(canvas, bold, L, cfg, cam.width(), cam.height(), dets);

		if (now - t_log >= std::chrono::seconds(10)) {
			t_log = now;
			fprintf(stderr,
				"kray-demo: camera %.1f fps, npu %.1f fps (%.0f ms), people %d, cpu %.0f%%, npu %.0f%%, "
				"%.1f C, ram %d/%d MB, wb R %.2f B %.2f\n",
				ui.cam_fps, ui.npu_fps, ui.infer_ms, ui.people, ui.sys.cpu_total * 100, ui.npu() * 100,
				ui.sys.temp_c, ui.sys.ram_used_mb, ui.sys.ram_total_mb, wb.red() / 256.0, wb.blue() / 256.0);
		}

		if (!disp.flip() || (idx < 0 && !disp.connected())) {
			fprintf(stderr, "kray-demo: display lost, waiting for a monitor\n");
			for (int &h : rga_disp)
				if (h)
					releasebuffer_handle(h), h = 0;
			disp.close();
			t_retry = Clock::now();
		}
		if (idx < 0 && !cam.is_open())
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	ae.stop();
	for (int &h : rga_disp)
		if (h)
			releasebuffer_handle(h);
	return 0;
}
