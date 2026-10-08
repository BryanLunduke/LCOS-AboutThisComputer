// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "about_logic.hpp"

#include <iosfwd>
#include <string>

namespace lundukeabout {

struct SystemInfo {
  std::string os_pretty;      // e.g. "LCOS 0.7"
  std::string total_memory;   // e.g. "16.0 GB", or "Unknown"
  std::string cpu_model;      // from /proc/cpuinfo
  std::string gpu;            // sysfs + pci.ids
  bool memory_known = false;  // false when MemTotal was not in meminfo
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
  bool saw_total = false;
  bool saw_available = false;
  long total_kb = 0;
  long available_kb = 0;
  long free_kb = 0;
  long buffers_kb = 0;
  long cached_kb = 0;
};

struct MemoryUsage {
  // false when MemTotal was absent. The fields are then not a measurement.
  bool known = false;
  long total_kb = 0;
  long available_kb = 0;
  long used_kb = 0;
};

MemInfoSnapshot parse_meminfo(std::istream& in);
// MemAvailable 0 is real when MemTotal was present. A missing MemAvailable
// field uses MemFree + Buffers + Cached. A missing MemTotal is unknown.
MemoryUsage memory_usage_from_meminfo(const MemInfoSnapshot& snap);

// First non-empty "model name". Empty values are skipped. Hardware, then
// Processor, are used only when every model name is empty. "Unknown CPU"
// when the stream has none of those.
std::string cpu_model_from_cpuinfo(std::istream& in);

// Local display number from a DISPLAY spec (:1, :1.0, unix:1, localhost:1,
// 127.0.0.1:1). nullopt when the spec is empty, not a display, or names
// another machine. A background X server on this host is not a substitute.
std::optional<int> local_x_display_number(const std::string& spec);

// Listening entry from /proc/net/unix. Connected clients are omitted.
struct XListenSocket {
  unsigned long inode = 0;
  std::string path;
};

// A process that holds a unix socket inode, with its /proc comm.
struct ProcessSocket {
  pid_t pid = 0;
  unsigned long inode = 0;
  std::string comm;
};

std::vector<XListenSocket> parse_proc_net_unix(std::istream& in);
// Pathname or abstract socket for this display (/tmp/.X11-unix/XN).
bool x_socket_path_matches_display(const std::string& path, int display);
// comm of the listener for `display`. Sockets and processes for other
// display numbers are ignored. Empty when nothing matches.
std::string server_comm_for_display(int display, const std::vector<XListenSocket>& sockets,
                                   const std::vector<ProcessSocket>& processes);
// /proc comm of the server that owns this DISPLAY spec. Empty when the spec
// is not local or the listener's comm cannot be read.
std::string server_comm_owning_display(const std::string& display_spec);

// Label for that one server. "Virtual framebuffer (Xvfb)" only when `comm`
// is Xvfb. Xtigervnc, Xvnc, and Xephyr are "Virtual display". Any other
// comm, including empty, is "Unknown GPU".
std::string virtual_display_label_for_server(const std::string& comm);

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
