// SPDX-License-Identifier: GPL-3.0-or-later
#include "system_info.hpp"
#include "about_logic.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <dirent.h>
#include <cctype>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <unistd.h>

namespace lundukeabout {
namespace {

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
    s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

std::string unquote(std::string s) {
  s = trim(s);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
    return s.substr(1, s.size() - 2);
  return s;
}

std::string read_fd_limited(int fd) {
  std::string data;
  char buf[4096];
  while (data.size() < (1u << 20)) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    data.append(buf, buf + n);
  }
  return data;
}

std::string read_cpu_model() {
  std::ifstream in("/proc/cpuinfo");
  return cpu_model_from_cpuinfo(in);
}

bool parse_meminfo_line(const std::string& line, std::string& key, long& value) {
  const auto colon = line.find(':');
  if (colon == std::string::npos) return false;
  key = line.substr(0, colon + 1);
  const char* p = line.c_str() + colon + 1;
  while (*p == ' ' || *p == '\t') ++p;
  char* end = nullptr;
  const long parsed = std::strtol(p, &end, 10);
  if (end == p) return false;
  value = parsed;
  return true;
}

void apply_memory_fields(SystemInfo& info) {
  MemInfoSnapshot snap;
  std::ifstream in("/proc/meminfo");
  if (in) snap = parse_meminfo(in);
  const MemoryUsage usage = memory_usage_from_meminfo(snap);
  info.memory_known = usage.known;
  info.total_memory_kb = usage.total_kb;
  info.available_memory_kb = usage.available_kb;
  info.used_memory_kb = usage.used_kb;
  if (!usage.known) {
    info.total_memory = "Unknown";
    return;
  }
  info.total_memory = format_memory_human(info.total_memory_kb);
}

bool read_hex_sysfs(const std::string& path, unsigned& out) {
  std::ifstream in(path);
  std::string text;
  if (!(in >> text)) return false;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 0);
  if (end == text.c_str()) return false;
  out = static_cast<unsigned>(parsed);
  return true;
}

std::string read_driver_name(const std::string& uevent_path) {
  std::ifstream in(uevent_path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("DRIVER=", 0) == 0) return line.substr(7);
  }
  return {};
}

std::string find_pci_ids_file() {
  const char* paths[] = {
      "/usr/share/misc/pci.ids",
      "/usr/share/hwdata/pci.ids",
      "/usr/share/pci.ids",
  };
  for (const char* path : paths) {
    std::ifstream in(path);
    if (in) return path;
  }
  return {};
}

std::string read_drm_fallback() {
  DIR* dir = opendir("/sys/class/drm");
  if (!dir) return {};
  std::string driver;
  while (dirent* de = readdir(dir)) {
    const std::string name = de->d_name;
    if (name.rfind("card", 0) != 0) continue;
    if (name.find('-') != std::string::npos) continue;
    driver = read_driver_name("/sys/class/drm/" + name + "/device/uevent");
    if (!driver.empty()) break;
  }
  closedir(dir);
  if (driver.empty()) return {};
  return "DRM: " + driver;
}

// sysfs only on the startup path. lspci / glxinfo are not launched.
// The string is cached only after the PCI / DRM / DISPLAY choice.
std::string read_gpu() {
  static bool cached = false;
  static std::string value;
  if (cached) return value;

  std::vector<GpuDevice> devices;
  if (DIR* dir = opendir("/sys/bus/pci/devices")) {
    while (dirent* de = readdir(dir)) {
      if (de->d_name[0] == '.') continue;
      const std::string slot = de->d_name;
      const std::string base = "/sys/bus/pci/devices/" + slot;
      unsigned class_code = 0;
      if (!read_hex_sysfs(base + "/class", class_code)) continue;
      if (!is_display_class(class_code)) continue;
      unsigned vendor = 0;
      unsigned device = 0;
      read_hex_sysfs(base + "/vendor", vendor);
      read_hex_sysfs(base + "/device", device);
      devices.push_back(
          GpuDevice{slot, class_code, vendor, device, read_driver_name(base + "/uevent")});
    }
    closedir(dir);
  }
  std::sort(devices.begin(), devices.end(),
            [](const GpuDevice& a, const GpuDevice& b) { return a.slot < b.slot; });

  PciDb db;
  const std::string ids_path = find_pci_ids_file();
  if (!ids_path.empty()) {
    std::ifstream in(ids_path);
    if (in) db = parse_pci_ids(in);
  }
  value = gpu_label_from_devices(devices, db);
  if (value.empty()) value = read_drm_fallback();
  if (value.empty()) {
    const char* display = std::getenv("DISPLAY");
    const std::string comm = server_comm_owning_display(display ? display : "");
    value = virtual_display_label_for_server(comm);
  }
  cached = true;
  return value;
}

enum class MemUnit { MbTenth, MbInt, GbTenth };

void split_magnitude(long long kb, bool& neg, unsigned long long& mag) {
  if (kb >= 0) {
    neg = false;
    mag = static_cast<unsigned long long>(kb);
    return;
  }
  neg = true;
  if (kb == LLONG_MIN) mag = static_cast<unsigned long long>(LLONG_MAX) + 1ull;
  else mag = static_cast<unsigned long long>(-kb);
}

MemUnit unit_for_mag(unsigned long long mag_kb) {
  if (mag_kb >= 1024ull * 1024ull) return MemUnit::GbTenth;
  if (mag_kb >= 10ull * 1024ull) return MemUnit::MbInt;
  return MemUnit::MbTenth;
}

unsigned long long round_mag(unsigned long long mag, MemUnit unit) {
  unsigned long long denom = 1024;
  unsigned long long scale = 1;
  if (unit == MemUnit::GbTenth) {
    denom = 1024ull * 1024ull;
    scale = 10;
  } else if (unit == MemUnit::MbTenth) {
    scale = 10;
  }
  const unsigned long long whole = mag / denom;
  const unsigned long long rem = mag % denom;
  return whole * scale + (rem * scale + denom / 2) / denom;
}

std::string format_units_unsigned(unsigned long long mag, MemUnit unit, bool neg) {
  const char* sign = neg ? "-" : "";
  char buf[64];
  if (unit == MemUnit::MbInt) {
    std::snprintf(buf, sizeof(buf), "%s%llu MB", sign, mag);
  } else {
    const char* suffix = unit == MemUnit::GbTenth ? "GB" : "MB";
    std::snprintf(buf, sizeof(buf), "%s%llu.%llu %s", sign, mag / 10, mag % 10, suffix);
  }
  return buf;
}

std::string format_signed_kb(long kb) {
  if (kb == 0) return "0 MB";
  bool neg = false;
  unsigned long long mag = 0;
  split_magnitude(kb, neg, mag);
  const MemUnit unit = unit_for_mag(mag);
  return format_units_unsigned(round_mag(mag, unit), unit, neg);
}

}  // namespace

std::string format_memory_human(long kb) { return format_signed_kb(kb); }

std::string cpu_model_from_cpuinfo(std::istream& in) {
  std::string hardware;
  std::string processor;
  std::string line;
  auto trim_value = [](std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
      s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
  };
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto colon = line.find(':');
    const bool model = line.rfind("model name", 0) == 0;
    const bool hw = line.rfind("Hardware", 0) == 0;
    const bool proc = line.rfind("Processor", 0) == 0;
    if (!model && !hw && !proc) continue;
    if (colon == std::string::npos) continue;
    const std::string value = trim_value(line.substr(colon + 1));
    if (value.empty()) continue;
    if (model) return value;
    if (hw && hardware.empty()) hardware = value;
    if (proc && processor.empty()) processor = value;
  }
  if (!hardware.empty()) return hardware;
  if (!processor.empty()) return processor;
  return "Unknown CPU";
}

namespace {

bool display_host_is_local(const std::string& host) {
  return host.empty() || host == "unix" || host == "localhost" || host == "127.0.0.1";
}

std::string read_comm_file(pid_t pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/comm");
  std::string comm;
  if (!std::getline(in, comm)) return {};
  while (!comm.empty() &&
         (comm.back() == '\n' || comm.back() == '\r' || comm.back() == ' ')) {
    comm.pop_back();
  }
  return comm;
}

bool parse_socket_inode(const std::string& target, unsigned long& inode) {
  constexpr char kPrefix[] = "socket:[";
  constexpr size_t n = sizeof(kPrefix) - 1;
  if (target.size() < n + 2 || target.compare(0, n, kPrefix) != 0 || target.back() != ']') {
    return false;
  }
  const std::string digits = target.substr(n, target.size() - n - 1);
  if (digits.empty()) return false;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(digits.c_str(), &end, 10);
  if (end != digits.c_str() + digits.size() || parsed == 0) return false;
  inode = parsed;
  return true;
}

}  // namespace

std::optional<int> local_x_display_number(const std::string& spec) {
  if (spec.empty()) return std::nullopt;
  const auto colon = spec.rfind(':');
  if (colon == std::string::npos) return std::nullopt;
  if (!display_host_is_local(spec.substr(0, colon))) return std::nullopt;
  std::string rest = spec.substr(colon + 1);
  const auto dot = rest.find('.');
  if (dot != std::string::npos) rest.resize(dot);
  if (rest.empty() || rest.size() > 8) return std::nullopt;
  for (unsigned char c : rest) {
    if (!std::isdigit(c)) return std::nullopt;
  }
  try {
    const int n = std::stoi(rest);
    if (n < 0) return std::nullopt;
    return n;
  } catch (...) {
    return std::nullopt;
  }
}

bool x_socket_path_matches_display(const std::string& path, int display) {
  if (display < 0 || path.empty()) return false;
  const std::string suffix = "/.X11-unix/X" + std::to_string(display);
  if (path.size() < suffix.size()) return false;
  return path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<XListenSocket> parse_proc_net_unix(std::istream& in) {
  std::vector<XListenSocket> out;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream iss(line);
    std::string num, refcnt, proto, flags_s, type, st, inode_s, path;
    if (!(iss >> num >> refcnt >> proto >> flags_s >> type >> st >> inode_s)) continue;
    char* flags_end = nullptr;
    const unsigned long flags = std::strtoul(flags_s.c_str(), &flags_end, 16);
    if (flags_end == flags_s.c_str()) continue;
    // SO_ACCEPTCON (1 << 16). Connected clients share the path and are not it.
    constexpr unsigned long kListen = 0x10000ul;
    if ((flags & kListen) == 0) continue;
    if (!(iss >> path) || path.empty()) continue;
    char* inode_end = nullptr;
    const unsigned long inode = std::strtoul(inode_s.c_str(), &inode_end, 10);
    if (inode_end == inode_s.c_str() || inode == 0) continue;
    out.push_back(XListenSocket{inode, path});
  }
  return out;
}

std::string server_comm_for_display(int display, const std::vector<XListenSocket>& sockets,
                                   const std::vector<ProcessSocket>& processes) {
  if (display < 0) return {};
  std::unordered_set<unsigned long> inodes;
  for (const auto& sock : sockets) {
    if (sock.inode == 0) continue;
    if (!x_socket_path_matches_display(sock.path, display)) continue;
    inodes.insert(sock.inode);
  }
  if (inodes.empty()) return {};
  const ProcessSocket* best = nullptr;
  for (const auto& proc : processes) {
    if (proc.pid <= 0 || proc.comm.empty()) continue;
    if (!inodes.count(proc.inode)) continue;
    if (!best || proc.pid < best->pid) best = &proc;
  }
  if (!best) return {};
  return best->comm;
}

std::string server_comm_owning_display(const std::string& display_spec) {
  const auto number = local_x_display_number(display_spec);
  if (!number) return {};
  std::ifstream table("/proc/net/unix");
  if (!table) return {};
  const std::vector<XListenSocket> sockets = parse_proc_net_unix(table);
  std::unordered_set<unsigned long> want;
  std::vector<XListenSocket> matched;
  matched.reserve(sockets.size());
  for (const auto& sock : sockets) {
    if (!x_socket_path_matches_display(sock.path, *number)) continue;
    want.insert(sock.inode);
    matched.push_back(sock);
  }
  if (want.empty()) return {};

  std::vector<ProcessSocket> hits;
  DIR* proc = opendir("/proc");
  if (!proc) return {};
  while (dirent* de = readdir(proc)) {
    if (!std::isdigit(static_cast<unsigned char>(de->d_name[0]))) continue;
    char* end = nullptr;
    const unsigned long pid_ul = std::strtoul(de->d_name, &end, 10);
    if (!end || *end != '\0' || pid_ul == 0 || pid_ul > static_cast<unsigned long>(INT_MAX)) {
      continue;
    }
    const pid_t pid = static_cast<pid_t>(pid_ul);
    const std::string fd_dir_path = std::string("/proc/") + de->d_name + "/fd";
    DIR* fds = opendir(fd_dir_path.c_str());
    if (!fds) continue;
    unsigned long held = 0;
    while (dirent* fd = readdir(fds)) {
      if (fd->d_name[0] == '.') continue;
      const std::string link_path = fd_dir_path + "/" + fd->d_name;
      char buf[96];
      const ssize_t n = ::readlink(link_path.c_str(), buf, sizeof(buf));
      if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) continue;
      unsigned long inode = 0;
      if (!parse_socket_inode(std::string(buf, static_cast<size_t>(n)), inode)) continue;
      if (!want.count(inode)) continue;
      held = inode;
      break;
    }
    closedir(fds);
    if (held == 0) continue;
    const std::string comm = read_comm_file(pid);
    if (comm.empty()) continue;
    hits.push_back(ProcessSocket{pid, held, comm});
  }
  closedir(proc);
  return server_comm_for_display(*number, matched, hits);
}

std::string virtual_display_label_for_server(const std::string& comm) {
  if (comm == "Xvfb") return "Virtual framebuffer (Xvfb)";
  if (comm == "Xtigervnc" || comm == "Xvnc" || comm == "Xephyr") return "Virtual display";
  return "Unknown GPU";
}

MemoryReadout format_memory_readout(long used_kb, long total_kb) {
  MemoryReadout out;
  if (used_kb == 0 && total_kb == 0) {
    out.total = out.used = out.free = "0 MB";
    return out;
  }
  bool total_neg = false;
  unsigned long long total_mag = 0;
  split_magnitude(total_kb, total_neg, total_mag);
  const MemUnit unit = unit_for_mag(total_mag);
  bool used_neg = false;
  unsigned long long used_mag = 0;
  split_magnitude(used_kb, used_neg, used_mag);
  const unsigned long long total_units = round_mag(total_mag, unit);
  const unsigned long long used_units = round_mag(used_mag, unit);
  out.total = format_units_unsigned(total_units, unit, total_neg);
  out.used = format_units_unsigned(used_units, unit, used_neg);
  if (used_neg == total_neg) {
    if (total_units >= used_units) {
      out.free = format_units_unsigned(total_units - used_units, unit, total_neg);
    } else {
      out.free = format_units_unsigned(used_units - total_units, unit, !total_neg);
    }
  } else if (total_neg) {
    out.free = format_units_unsigned(total_units + used_units, unit, true);
  } else {
    out.free = format_units_unsigned(total_units + used_units, unit, false);
  }
  return out;
}

OsRelease parse_os_release(std::istream& in) {
  OsRelease rel;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const auto key = line.substr(0, eq);
    const auto val = unquote(line.substr(eq + 1));
    if (key == "PRETTY_NAME") rel.pretty = val;
    else if (key == "NAME") rel.name = val;
    else if (key == "VERSION") rel.version = val;
    else if (key == "ID") rel.id = val;
  }
  rel.ok = !rel.pretty.empty() || !rel.name.empty() || !rel.id.empty();
  return rel;
}

std::string os_display_name(const OsRelease& host, const OsRelease& fallback,
                            bool allow_fallback) {
  const auto format = [](const OsRelease& rel) {
    if (!rel.pretty.empty()) return rel.pretty;
    if (!rel.name.empty()) {
      return rel.version.empty() ? rel.name : (rel.name + " " + rel.version);
    }
    if (!rel.id.empty()) return rel.id;
    return std::string();
  };
  if (host.ok) {
    const std::string text = format(host);
    if (!text.empty()) return text;
  }
  if (allow_fallback && fallback.ok) {
    const std::string text = format(fallback);
    if (!text.empty()) return text;
  }
  return "Unknown OS";
}

MemInfoSnapshot parse_meminfo(std::istream& in) {
  MemInfoSnapshot snap;
  std::string line;
  while (std::getline(in, line)) {
    std::string key;
    long value = 0;
    if (!parse_meminfo_line(line, key, value)) continue;
    if (key == "MemTotal:") {
      snap.total_kb = value;
      snap.saw_total = true;
    } else if (key == "MemAvailable:") {
      snap.available_kb = value;
      snap.saw_available = true;
    } else if (key == "MemFree:") snap.free_kb = value;
    else if (key == "Buffers:") snap.buffers_kb = value;
    else if (key == "Cached:") snap.cached_kb = value;
  }
  return snap;
}

MemoryUsage memory_usage_from_meminfo(const MemInfoSnapshot& snap) {
  MemoryUsage usage;
  if (!snap.saw_total) return usage;
  usage.known = true;
  usage.total_kb = snap.total_kb;
  // MemAvailable: 0 is real (nothing reclaimable). Only an absent field
  // falls back to the older MemFree + Buffers + Cached estimate.
  if (snap.saw_available) usage.available_kb = snap.available_kb;
  else usage.available_kb = snap.free_kb + snap.buffers_kb + snap.cached_kb;
  if (usage.available_kb > usage.total_kb) usage.available_kb = usage.total_kb;
  if (usage.available_kb < 0) usage.available_kb = 0;
  usage.used_kb = usage.total_kb - usage.available_kb;
  if (usage.used_kb < 0) usage.used_kb = 0;
  return usage;
}

int open_proc_pid_dir(pid_t pid) {
  if (pid <= 0) return -1;
  const std::string path = "/proc/" + std::to_string(pid);
  int fd = ::open(path.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  return fd;
}

ProcSnapshot read_proc_snapshot_at(int dirfd) {
  ProcSnapshot id;
  if (dirfd < 0) return id;
  const int fd = ::openat(dirfd, "stat", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return id;
  std::string text = read_fd_limited(fd);
  ::close(fd);
  const auto nl = text.find('\n');
  if (nl != std::string::npos) text.resize(nl);
  return parse_proc_stat_line(text);
}

ProcSnapshot read_proc_snapshot(pid_t pid) {
  const int dirfd = open_proc_pid_dir(pid);
  if (dirfd < 0) return {};
  const ProcSnapshot id = read_proc_snapshot_at(dirfd);
  ::close(dirfd);
  return id;
}

SystemInfo gather_system_info() {
  SystemInfo info;
  OsRelease host;
  {
    std::ifstream in("/etc/os-release");
    if (in) host = parse_os_release(in);
  }
  OsRelease fallback;
  bool allow_fallback = false;
  // A parsed /etc/os-release is the host. The recipe file is not compiled in.
  // LUNDUKE_ABOUT_OS_RELEASE is a dev path used only when /etc did not parse.
  if (!host.ok) {
    if (const char* env = std::getenv("LUNDUKE_ABOUT_OS_RELEASE")) {
      if (env[0] != '\0') {
        std::ifstream in(env);
        if (in) {
          fallback = parse_os_release(in);
          allow_fallback = true;
        }
      }
    }
  }
  info.os_pretty = os_display_name(host, fallback, allow_fallback);

  apply_memory_fields(info);
  info.cpu_model = read_cpu_model();
  info.gpu = read_gpu();
  return info;
}

void refresh_memory_usage(SystemInfo& info) {
  apply_memory_fields(info);
}

}  // namespace lundukeabout
