// SPDX-License-Identifier: MIT
#include "stats.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/mount.h>
#include <unistd.h>

static std::string read_line(const std::string &path)
{
	std::ifstream f(path);
	std::string s;
	std::getline(f, s);
	return s;
}

StatsReader::StatsReader()
{
	// SoC temperature: the thermal zone named "soc-thermal" (or the first one).
	for (int i = 0; i < 8; i++) {
		std::string base = "/sys/class/thermal/thermal_zone" + std::to_string(i);
		std::string type = read_line(base + "/type");
		if (type.empty())
			break;
		if (temp_path_.empty() || type == "soc-thermal")
			temp_path_ = base + "/temp";
		if (type == "soc-thermal")
			break;
	}
	// NPU load is reported by the vendor rknpu driver in debugfs.
	npu_path_ = "/sys/kernel/debug/rknpu/load";
	if (access(npu_path_.c_str(), R_OK) != 0)
		mount("debugfs", "/sys/kernel/debug", "debugfs", 0, nullptr);
}

void StatsReader::sample(SysStats &s)
{
	std::ifstream f("/proc/stat");
	std::string line;
	std::vector<Cpu> cur;
	while (std::getline(f, line)) {
		if (line.compare(0, 3, "cpu") != 0)
			break;
		std::istringstream is(line);
		std::string name;
		unsigned long long v[8] = {};
		is >> name;
		for (auto &x : v)
			is >> x;
		Cpu c;
		c.total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
		c.busy = c.total - v[3] - v[4]; // minus idle and iowait
		cur.push_back(c);
	}
	if (prev_.size() == cur.size() && !cur.empty()) {
		auto share = [&](size_t i) {
			unsigned long long dt = cur[i].total - prev_[i].total;
			return dt ? float(cur[i].busy - prev_[i].busy) / float(dt) : 0.0f;
		};
		s.cpu_total = share(0);
		s.cpu_core.resize(cur.size() - 1);
		for (size_t i = 1; i < cur.size(); i++)
			s.cpu_core[i - 1] = share(i);
	}
	prev_ = cur;

	// "NPU load:  Core0: 45%," (one core on RK3566)
	std::string npu = read_line(npu_path_);
	size_t p = npu.find("Core0:");
	s.npu = p == std::string::npos ? -1 : atoi(npu.c_str() + p + 6) / 100.0f;

	std::string t = read_line(temp_path_);
	s.temp_c = t.empty() ? -1 : atoi(t.c_str()) / 1000.0f;

	std::ifstream m("/proc/meminfo");
	long total = 0, avail = 0;
	while (std::getline(m, line)) {
		if (line.compare(0, 9, "MemTotal:") == 0)
			total = atol(line.c_str() + 9);
		else if (line.compare(0, 13, "MemAvailable:") == 0)
			avail = atol(line.c_str() + 13);
	}
	s.ram_total_mb = int(total / 1024);
	s.ram_used_mb = int((total - avail) / 1024);
}
