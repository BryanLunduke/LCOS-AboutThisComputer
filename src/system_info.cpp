// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "system_info.hpp"
#include "about_logic.hpp"

#include <X11/Xlib.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <set>
#include <unordered_set>
#include <utility>
#include <dirent.h>
#include <cctype>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <sys/socket.h>
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

}  // namespace

std::vector<GpuDevice> gpu_devices_from_sysfs(const std::string& devices_dir) {
  std::vector<GpuDevice> devices;
  DIR* dir = opendir(devices_dir.c_str());
  if (!dir) return devices;
  while (dirent* de = readdir(dir)) {
    if (de->d_name[0] == '.') continue;
    const std::string slot = de->d_name;
    const std::string base = devices_dir + "/" + slot;
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
  std::sort(devices.begin(), devices.end(),
            [](const GpuDevice& a, const GpuDevice& b) { return a.slot < b.slot; });
  return devices;
}

namespace {

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
std::string read_gpu(int x_connection_fd, const std::string& x_display_name) {
  static bool cached = false;
  static std::string value;
  if (cached) return value;

  const std::vector<GpuDevice> devices = gpu_devices_from_sysfs("/sys/bus/pci/devices");

  PciDb db;
  const std::string ids_path = find_pci_ids_file();
  if (!ids_path.empty()) {
    std::ifstream in(ids_path);
    if (in) db = parse_pci_ids(in);
  }
  value = gpu_label_from_devices(devices, db);
  if (value.empty()) value = read_drm_fallback();
  if (value.empty()) {
    std::string spec = x_display_name;
    if (spec.empty()) {
      if (const char* display = std::getenv("DISPLAY")) spec = display;
    }
    value = gpu_label_for_x_connection(x_connection_fd, spec);
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

namespace {

std::string trim_cpu_value(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
    s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

bool parse_cpu_int(const std::string& text, int& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str()) return false;
  out = static_cast<int>(parsed);
  return true;
}

std::string core_thread_phrase(int cores, int threads) {
  if (cores < 1) cores = 1;
  if (threads < 1) threads = 1;
  return " (" + std::to_string(cores) + (cores == 1 ? " core, " : " cores, ") +
         std::to_string(threads) + (threads == 1 ? " thread)" : " threads)");
}

struct CpuPiece {
  std::string model;
  bool saw_processor = false;
  bool has_phys = false;
  int phys = 0;
  bool has_core = false;
  int core = 0;
  bool has_cpu_cores = false;
  int cpu_cores = 0;
};

struct CpuGroup {
  std::string model;
  int threads = 0;
  bool any_core_id = false;
  std::set<std::pair<int, int>> core_ids;
  bool any_phys = false;
  std::map<int, int> cpu_cores_by_phys;
  int cpu_cores_only = 0;
  bool any_cpu_cores = false;
};

int cores_in_group(const CpuGroup& group) {
  int cores = 0;
  if (group.any_core_id) {
    cores = static_cast<int>(group.core_ids.size());
  } else if (group.any_cpu_cores) {
    if (group.any_phys && !group.cpu_cores_by_phys.empty()) {
      for (const auto& entry : group.cpu_cores_by_phys) {
        if (entry.second > 0) cores += entry.second;
      }
    } else {
      cores = group.cpu_cores_only;
    }
  } else {
    cores = group.threads;
  }
  if (cores < 1) cores = group.threads;
  // A per-package "cpu cores" copied onto every model overstates a cluster.
  if (cores > group.threads) cores = group.threads;
  return cores;
}

}  // namespace

std::string cpu_model_from_cpuinfo(std::istream& in) {
  std::string hardware;
  std::string processor_fallback;
  std::vector<CpuPiece> pieces;
  CpuPiece cur;
  bool in_block = false;
  std::string line;

  auto flush = [&]() {
    if (!in_block) return;
    pieces.push_back(cur);
    cur = CpuPiece{};
    in_block = false;
  };

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (trim_cpu_value(line).empty()) {
      flush();
      continue;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = trim_cpu_value(line.substr(0, colon));
    const std::string value = trim_cpu_value(line.substr(colon + 1));
    if (key == "processor") {
      if (in_block && cur.saw_processor) flush();
      in_block = true;
      cur.saw_processor = true;
      continue;
    }
    in_block = true;
    if (key == "model name") {
      if (!value.empty()) cur.model = value;
      continue;
    }
    if (key == "Hardware") {
      if (!value.empty() && hardware.empty()) hardware = value;
      continue;
    }
    if (key == "Processor") {
      if (!value.empty() && processor_fallback.empty()) processor_fallback = value;
      continue;
    }
    int number = 0;
    if (!parse_cpu_int(value, number)) continue;
    if (key == "physical id") {
      cur.has_phys = true;
      cur.phys = number;
    } else if (key == "core id") {
      cur.has_core = true;
      cur.core = number;
    } else if (key == "cpu cores") {
      cur.has_cpu_cores = true;
      cur.cpu_cores = number;
    }
  }
  flush();

  std::vector<CpuGroup> groups;
  std::map<std::string, size_t> index_of;
  bool any_processor = false;
  for (const CpuPiece& piece : pieces) {
    if (piece.saw_processor) any_processor = true;
    if (piece.model.empty()) continue;
    size_t idx = 0;
    const auto found = index_of.find(piece.model);
    if (found == index_of.end()) {
      idx = groups.size();
      index_of.emplace(piece.model, idx);
      CpuGroup group;
      group.model = piece.model;
      groups.push_back(group);
    } else {
      idx = found->second;
    }
    if (!piece.saw_processor) continue;
    CpuGroup& group = groups[idx];
    group.threads += 1;
    if (piece.has_core) {
      group.any_core_id = true;
      const int phys = piece.has_phys ? piece.phys : 0;
      group.core_ids.insert({phys, piece.core});
    }
    if (piece.has_cpu_cores && piece.cpu_cores > 0) {
      group.any_cpu_cores = true;
      group.cpu_cores_only = piece.cpu_cores;
      if (piece.has_phys) {
        group.any_phys = true;
        group.cpu_cores_by_phys[piece.phys] = piece.cpu_cores;
      }
    }
  }

  if (!any_processor) {
    for (const CpuPiece& piece : pieces) {
      if (!piece.model.empty()) return piece.model;
    }
    if (!hardware.empty()) return hardware;
    if (!processor_fallback.empty()) return processor_fallback;
    return "Unknown CPU";
  }

  std::string out;
  for (const CpuGroup& group : groups) {
    if (group.threads < 1) continue;
    if (!out.empty()) out += "; ";
    out += group.model + core_thread_phrase(cores_in_group(group), group.threads);
  }
  if (!out.empty()) return out;
  if (!hardware.empty()) return hardware;
  if (!processor_fallback.empty()) return processor_fallback;
  return "Unknown CPU";
}

std::string comm_of_pid(pid_t pid) {
  if (pid <= 0) return {};
  std::ifstream in("/proc/" + std::to_string(pid) + "/comm");
  std::string comm;
  if (std::getline(in, comm)) {
    while (!comm.empty() &&
           (comm.back() == '\n' || comm.back() == '\r' || comm.back() == ' ')) {
      comm.pop_back();
    }
    if (!comm.empty()) return comm;
  }
  // comm is mode 0444 even for root. stat is too, and carries the same name
  // when comm itself cannot be read. /proc/<pid>/fd is not used.
  std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (!std::getline(stat, line)) return {};
  const ProcSnapshot snap = parse_proc_stat_line(line);
  if (!snap.ok || snap.comm.empty()) return {};
  return snap.comm;
}

XPeerCred x_peer_cred_from_fd(int fd) {
  XPeerCred out;
  if (fd < 0) return out;
  struct ucred cred {};
  socklen_t len = sizeof(cred);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
    const int err = errno;
    // TCP sockets and non-sockets do not support the option.
    out.supported = !(err == ENOTSOCK || err == ENOPROTOOPT || err == EOPNOTSUPP);
    return out;
  }
  out.supported = true;
  if (static_cast<size_t>(len) < sizeof(cred) || cred.pid <= 0) return out;
  out.have_pid = true;
  out.pid = static_cast<pid_t>(cred.pid);
  out.uid = cred.uid;
  out.comm = comm_of_pid(out.pid);
  return out;
}

namespace {

std::string lower_copy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool all_digits(const std::string& text) {
  if (text.empty()) return false;
  for (unsigned char c : text) {
    if (!std::isdigit(c)) return false;
  }
  return true;
}

bool ipv4_loopback(const std::string& host) {
  if (host.size() < 5 || host.compare(0, 4, "127.") != 0) return false;
  int dots = 0;
  int oct = 0;
  bool any = false;
  for (size_t i = 4; i <= host.size(); ++i) {
    if (i == host.size() || host[i] == '.') {
      if (!any || oct > 255) return false;
      ++dots;
      oct = 0;
      any = false;
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(host[i]))) return false;
    any = true;
    oct = oct * 10 + (host[i] - '0');
    if (oct > 255) return false;
  }
  return dots == 3;
}

bool host_is_loopback(const std::string& host) {
  if (host.empty()) return true;
  const std::string h = lower_copy(host);
  if (h == "localhost" || h == "localhost.localdomain" || h == "unix") return true;
  if (h == "::1" || h == "0:0:0:0:0:0:0:1") return true;
  return ipv4_loopback(h);
}

bool parse_display_number(const std::string& rest, int& display, int& screen, bool& has_screen) {
  std::string disp = rest;
  std::string scr;
  const auto dot = rest.find('.');
  if (dot != std::string::npos) {
    disp = rest.substr(0, dot);
    scr = rest.substr(dot + 1);
    has_screen = true;
    if (!all_digits(scr) || scr.size() > 8) return false;
  } else {
    has_screen = false;
  }
  if (!all_digits(disp) || disp.size() > 8) return false;
  try {
    display = std::stoi(disp);
    screen = has_screen ? std::stoi(scr) : 0;
  } catch (...) {
    return false;
  }
  return display >= 0 && screen >= 0;
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

XDisplayParsed parse_x_display(const std::string& spec) {
  XDisplayParsed out;
  std::string text = trim_cpu_value(spec);
  if (text.empty()) return out;

  size_t colon = std::string::npos;
  const auto bracket = text.find('[');
  if (bracket != std::string::npos) {
    const auto end = text.find(']', bracket);
    if (end == std::string::npos || end + 1 >= text.size() || text[end + 1] != ':') return out;
    colon = end + 1;
  } else {
    colon = text.rfind(':');
    if (colon == std::string::npos) return out;
  }

  const std::string left = text.substr(0, colon);
  const std::string right = text.substr(colon + 1);
  std::string protocol;
  std::string host = left;
  const auto slash = left.find('/');
  if (slash != std::string::npos) {
    protocol = left.substr(0, slash);
    host = left.substr(slash + 1);
  }
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }

  int display = -1;
  int screen = 0;
  bool has_screen = false;
  if (!parse_display_number(right, display, screen, has_screen)) return out;

  const std::string proto = lower_copy(protocol);
  XDisplayTransport transport = XDisplayTransport::Remote;
  if (proto == "unix" || proto == "local") {
    transport = XDisplayTransport::LocalUnix;
  } else if (proto == "tcp" || proto == "inet" || proto == "inet6") {
    transport = host_is_loopback(host) && lower_copy(host) != "unix" ? XDisplayTransport::LocalTcp
                                                                     : XDisplayTransport::Remote;
  } else if (!proto.empty()) {
    transport = host_is_loopback(host) ? XDisplayTransport::LocalTcp : XDisplayTransport::Remote;
  } else if (host.empty() || lower_copy(host) == "unix") {
    transport = XDisplayTransport::LocalUnix;
  } else if (host_is_loopback(host)) {
    // An IPv6 literal is a TCP address. localhost and 127.0.0.1 are the
    // historical local unix forms.
    transport = host.find(':') != std::string::npos ? XDisplayTransport::LocalTcp
                                                     : XDisplayTransport::LocalUnix;
  }

  out.ok = true;
  out.transport = transport;
  out.protocol = protocol;
  out.host = host;
  out.display = display;
  out.screen = screen;
  out.has_screen = has_screen;
  return out;
}

std::optional<int> local_x_display_number(const std::string& spec) {
  const XDisplayParsed parsed = parse_x_display(spec);
  if (!parsed.ok) return std::nullopt;
  if (parsed.transport != XDisplayTransport::LocalUnix &&
      parsed.transport != XDisplayTransport::LocalTcp) {
    return std::nullopt;
  }
  return parsed.display;
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

namespace {

// Proc-fd walk. Used only when SO_PEERCRED does not apply (local TCP) or
// the unix display could not be opened. A root server's fd directory is
// not readable; that case is handled by the peer-credential path instead.
std::string server_comm_via_proc(int display) {
  if (display < 0) return {};
  std::ifstream table("/proc/net/unix");
  if (!table) return {};
  const std::vector<XListenSocket> sockets = parse_proc_net_unix(table);
  std::unordered_set<unsigned long> want;
  std::vector<XListenSocket> matched;
  matched.reserve(sockets.size());
  for (const auto& sock : sockets) {
    if (!x_socket_path_matches_display(sock.path, display)) continue;
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
    const std::string comm = comm_of_pid(pid);
    if (comm.empty()) continue;
    hits.push_back(ProcessSocket{pid, held, comm});
  }
  closedir(proc);
  return server_comm_for_display(display, matched, hits);
}

struct XOpenComm {
  bool opened = false;
  bool have_pid = false;
  std::string comm;
};

XOpenComm comm_from_opened_display(const std::string& spec) {
  XOpenComm out;
  if (spec.empty()) return out;
  Display* dpy = ::XOpenDisplay(spec.c_str());
  if (!dpy) return out;
  out.opened = true;
  const int fd = XConnectionNumber(dpy);
  const XPeerCred peer = x_peer_cred_from_fd(fd);
  ::XCloseDisplay(dpy);
  out.have_pid = peer.have_pid;
  out.comm = peer.comm;
  return out;
}

}  // namespace

std::string server_comm_owning_display(const std::string& display_spec) {
  const XDisplayParsed parsed = parse_x_display(display_spec);
  if (!parsed.ok || parsed.transport == XDisplayTransport::Remote) return {};
  if (parsed.transport == XDisplayTransport::LocalUnix) {
    XOpenComm opened = comm_from_opened_display(display_spec);
    if (!opened.opened) {
      const std::string simple = ":" + std::to_string(parsed.display);
      if (simple != display_spec) opened = comm_from_opened_display(simple);
    }
    if (opened.opened) {
      // The peer pid is the server. An unreadable comm stays empty rather
      // than being replaced by some other process from the proc walk.
      if (opened.have_pid) return opened.comm;
      return {};
    }
  }
  return server_comm_via_proc(parsed.display);
}

std::string virtual_display_label_for_server(const std::string& comm) {
  if (comm == "Xvfb") return "Virtual framebuffer (Xvfb)";
  if (comm == "Xtigervnc" || comm == "Xvnc" || comm == "Xephyr") return "Virtual display";
  return "Unknown GPU";
}

std::string gpu_label_for_x_connection(int connection_fd, const std::string& display_spec) {
  if (connection_fd >= 0) {
    const XPeerCred peer = x_peer_cred_from_fd(connection_fd);
    if (peer.have_pid) {
      // Authoritative for the connection the window already has.
      return virtual_display_label_for_server(peer.comm);
    }
    if (peer.supported) {
      // Unix socket, but no peer pid. Do not guess from the process list.
      return "Unknown GPU";
    }
    // SO_PEERCRED does not apply. Fall through to the spec.
  }
  const XDisplayParsed parsed = parse_x_display(display_spec);
  if (!parsed.ok) return "Unknown GPU";
  if (parsed.transport == XDisplayTransport::Remote) return "Remote display";
  const std::string comm = server_comm_owning_display(display_spec);
  return virtual_display_label_for_server(comm);
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

SystemInfo gather_system_info(int x_connection_fd, const std::string& x_display_name) {
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
  info.gpu = read_gpu(x_connection_fd, x_display_name);
  return info;
}

void refresh_memory_usage(SystemInfo& info) {
  apply_memory_fields(info);
}

}  // namespace lundukeabout
