// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "about_logic.hpp"

#include <iosfwd>
#include <string>

namespace lundukeabout {

struct SystemInfo {
  std::string os_pretty;      // e.g. "LCOS 0.7"
  std::string total_memory;   // e.g. "16.0 GB"
  std::string cpu_model;      // from /proc/cpuinfo
  std::string gpu;            // sysfs + pci.ids
  long total_memory_kb = 0;
  long available_memory_kb = 0;  // MemAvailable
  long used_memory_kb = 0;       // MemTotal - MemAvailable
};

// One rounding of used and of total, in a single unit. free is the rounded
// total minus the rounded used, so the three figures add up.
struct MemoryReadout {
  std::string total;
  std::string used;
  std::string free;
};

struct OsRelease {
  bool ok = false;
  std::string pretty;
  std::string name;
  std::string version;
  std::string id;
};

OsRelease parse_os_release(std::istream& in);
// Host text wins whenever it parsed. fallback is used only when host did not
// parse and allow_fallback is set (dev override). Pretty-name substrings are
// not consulted; ID is recorded on OsRelease for callers that care.
std::string os_display_name(const OsRelease& host, const OsRelease& fallback,
                            bool allow_fallback);

struct MemInfoSnapshot {
  bool saw_available = false;
  long total_kb = 0;
  long available_kb = 0;
  long free_kb = 0;
  long buffers_kb = 0;
  long cached_kb = 0;
};

struct MemoryUsage {
  long total_kb = 0;
  long available_kb = 0;
  long used_kb = 0;
};

MemInfoSnapshot parse_meminfo(std::istream& in);
// MemAvailable 0 is real. Only a missing MemAvailable field uses
// MemFree + Buffers + Cached.
MemoryUsage memory_usage_from_meminfo(const MemInfoSnapshot& snap);

// Directory fd for /proc/<pid> (O_PATH when the kernel accepts it). -1 on failure.
int open_proc_pid_dir(pid_t pid);
ProcSnapshot read_proc_snapshot_at(int dirfd);
ProcSnapshot read_proc_snapshot(pid_t pid);

SystemInfo gather_system_info();
// Re-read /proc/meminfo used/available (for live RAM bar refresh).
void refresh_memory_usage(SystemInfo& info);
std::string format_memory_human(long kb);
MemoryReadout format_memory_readout(long used_kb, long total_kb);

}  // namespace lundukeabout
