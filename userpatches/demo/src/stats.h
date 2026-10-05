// SPDX-License-Identifier: MIT
// System figures shown on screen: CPU per core, NPU load, SoC temperature, RAM.
#pragma once

#include <string>
#include <vector>

struct SysStats {
	float cpu_total = 0;           // 0..1
	std::vector<float> cpu_core;   // 0..1 per core
	float npu = -1;                // 0..1 from the NPU driver, -1 if unavailable
	float temp_c = -1;
	int ram_used_mb = 0, ram_total_mb = 0;
};

class StatsReader {
public:
	StatsReader();
	void sample(SysStats &s);

private:
	struct Cpu {
		unsigned long long busy = 0, total = 0;
	};
	std::vector<Cpu> prev_;
	std::string temp_path_;
	std::string npu_path_;
};
