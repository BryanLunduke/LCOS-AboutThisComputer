// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "about_logic.hpp"

#include <iosfwd>
#include <string>
#include <sys/types.h>

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

// CPU line from /proc/cpuinfo. Empty model names are skipped. Hardware, then
// Processor, are used only when every model name is empty ("Unknown CPU"
// when none of those exist). When logical processors are present, the line
// includes a core count and a thread count. Differing model names are listed
// in first-seen order, each with its own counts.
std::string cpu_model_from_cpuinfo(std::istream& in);

// [protocol/]host:display[.screen]. IPv6 hosts are bracketed ([::1]:N).
enum class XDisplayTransport {
  Invalid,
  LocalUnix,  // SO_PEERCRED applies: :N, unix:N, unix/:N, unix/host:N
  LocalTcp,   // loopback over tcp/inet/inet6; peer credentials do not apply
  Remote,
};

struct XDisplayParsed {
  bool ok = false;
  XDisplayTransport transport = XDisplayTransport::Invalid;
  std::string protocol;
  std::string host;
  int display = -1;
  int screen = 0;
  bool has_screen = false;
};

XDisplayParsed parse_x_display(const std::string& spec);

// Display number when the server is on this machine (local unix or local
// TCP). nullopt when the spec is empty, not a display, or names another
// machine. A background X server on this host is not a substitute.
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
// Credentials of the process at the far end of a connected socket.
// supported is false when SO_PEERCRED does not apply (TCP, or not a socket).
// comm is /proc/<pid>/comm, then the comm field of /proc/<pid>/stat.
// /proc/<pid>/fd is not consulted: that directory is not readable for a
// root-owned server, while comm is. comm is empty when neither file can
// be read (hidepid); have_pid is still set.
struct XPeerCred {
  bool supported = false;
  bool have_pid = false;
  pid_t pid = 0;
  uid_t uid = static_cast<uid_t>(-1);
  std::string comm;
};

XPeerCred x_peer_cred_from_fd(int fd);
// World-readable comm for pid. Empty when pid is not readable. Does not
// open /proc/<pid>/fd.
std::string comm_of_pid(pid_t pid);

// /proc comm of the server that owns this DISPLAY spec. A local unix spec
// is identified with SO_PEERCRED on an X connection (XConnectionNumber),
// not by scanning the process list. Local TCP falls back to that scan.
// Empty when the spec is remote, not a display, or the comm cannot be read.
std::string server_comm_owning_display(const std::string& display_spec);

// GPU label for the X server on connection_fd (the fd from XConnectionNumber).
// A peer pid from SO_PEERCRED wins over display_spec, so a --display that
// disagrees with the spec cannot name a different server. When the fd is
// not a unix socket, local TCP uses the proc fallback and a remote spec is
// "Remote display" rather than a false "Unknown GPU".
std::string gpu_label_for_x_connection(int connection_fd, const std::string& display_spec);

// Label for that one server. "Virtual framebuffer (Xvfb)" only when `comm`
// is Xvfb. Xtigervnc, Xvnc, and Xephyr are "Virtual display". Any other
// comm, including empty, is "Unknown GPU".
std::string virtual_display_label_for_server(const std::string& comm);

// Directory fd for /proc/<pid> (O_PATH when the kernel accepts it). -1 on failure.
int open_proc_pid_dir(pid_t pid);
ProcSnapshot read_proc_snapshot_at(int dirfd);
ProcSnapshot read_proc_snapshot(pid_t pid);

// x_connection_fd is XConnectionNumber of the display the window opened,
// or -1 when there is no live X connection. x_display_name is that
// display's name; an empty string uses getenv("DISPLAY").
SystemInfo gather_system_info(int x_connection_fd = -1,
                              const std::string& x_display_name = {});
// Display-class devices under a sysfs pci devices directory
// (/sys/bus/pci/devices, or a fixture laid out the same way).
std::vector<GpuDevice> gpu_devices_from_sysfs(const std::string& devices_dir);
// Re-read /proc/meminfo used/available (for live RAM bar refresh).
void refresh_memory_usage(SystemInfo& info);
std::string format_memory_human(long kb);
MemoryReadout format_memory_readout(long used_kb, long total_kb);

}  // namespace lundukeabout
