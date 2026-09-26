// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace lundukeabout {

struct SystemInfo {
  std::string os_pretty;      // e.g. "LCOS 0.7"
  std::string total_memory;   // e.g. "16.0 GB"
  std::string cpu_model;      // from /proc/cpuinfo
  std::string gpu;            // best-effort from lspci
  long total_memory_kb = 0;
};

SystemInfo gather_system_info();
std::string format_memory_mb(long kb);
std::string format_memory_human(long kb);

}  // namespace lundukeabout
