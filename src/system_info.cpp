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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <utility>
#include <fcntl.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
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
  // A TCP connection can succeed and still report pid 0 / uid -1. That
  // does not identify a peer; callers fall through to the display spec.
  if (static_cast<size_t>(len) < sizeof(cred) || cred.pid <= 0) return out;
  out.supported = true;
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

bool display_server_comm(const std::string& comm) {
  return comm == "Xvfb" || comm == "Xorg" || comm == "X" || comm == "Xtigervnc" ||
         comm == "Xvnc" || comm == "Xephyr" || comm == "Xwayland" || comm == "Xnest" ||
         comm == "Xdmx" || comm == "Xpra";
}

}  // namespace

std::vector<TcpListenEntry> parse_proc_net_tcp(std::istream& in) {
  std::vector<TcpListenEntry> out;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find("local_address") != std::string::npos) continue;
    std::istringstream iss(line);
    std::string sl, local, remote, state, txrx, tr, retr, uid_s, timeout_s, inode_s;
    if (!(iss >> sl >> local >> remote >> state >> txrx >> tr >> retr >> uid_s >> timeout_s >>
          inode_s)) {
      continue;
    }
    const auto colon = local.rfind(':');
    if (colon == std::string::npos || colon + 1 >= local.size()) continue;
    char* end = nullptr;
    const unsigned long port = std::strtoul(local.c_str() + colon + 1, &end, 16);
    if (end == local.c_str() + colon + 1 || port > 65535ul) continue;
    end = nullptr;
    const unsigned long st = std::strtoul(state.c_str(), &end, 16);
    if (end == state.c_str()) continue;
    end = nullptr;
    const unsigned long uid = std::strtoul(uid_s.c_str(), &end, 10);
    if (end == uid_s.c_str()) continue;
    end = nullptr;
    const unsigned long inode = std::strtoul(inode_s.c_str(), &end, 10);
    if (end == inode_s.c_str()) continue;
    TcpListenEntry entry;
    entry.port = static_cast<unsigned>(port);
    entry.inode = inode;
    entry.uid = static_cast<uid_t>(uid);
    entry.listening = st == 0x0Aul;
    out.push_back(entry);
  }
  return out;
}

std::string comm_owning_tcp_listeners(const std::vector<TcpListenEntry>& listeners,
                                     const std::vector<ProcFdRecord>& procs) {
  std::unordered_set<unsigned long> inodes;
  std::unordered_set<uid_t> uids;
  for (const TcpListenEntry& entry : listeners) {
    if (!entry.listening || entry.inode == 0) continue;
    inodes.insert(entry.inode);
    uids.insert(entry.uid);
  }
  if (inodes.empty()) return {};

  const ProcFdRecord* inode_hit = nullptr;
  for (const ProcFdRecord& proc : procs) {
    if (!proc.fd_dir_readable || proc.pid <= 0 || proc.comm.empty()) continue;
    bool holds = false;
    for (unsigned long inode : proc.socket_inodes) {
      if (inodes.count(inode)) holds = true;
    }
    if (!holds) continue;
    if (!inode_hit || proc.pid < inode_hit->pid) inode_hit = &proc;
  }
  if (inode_hit) return inode_hit->comm;

  // Root (and any other uid whose fd directory we cannot read). The socket
  // uid is the owner; comm names the process. A command line that contains
  // this display beats a guess, and a display-server comm beats sudo.
  const ProcFdRecord* cmdline_server = nullptr;
  const ProcFdRecord* cmdline_any = nullptr;
  const ProcFdRecord* only_server = nullptr;
  int servers = 0;
  const ProcFdRecord* only_proc = nullptr;
  int uid_procs = 0;
  for (const ProcFdRecord& proc : procs) {
    if (proc.fd_dir_readable || proc.pid <= 0 || proc.comm.empty()) continue;
    if (!uids.count(proc.uid)) continue;
    ++uid_procs;
    if (!only_proc || proc.pid < only_proc->pid) only_proc = &proc;
    const bool server = display_server_comm(proc.comm);
    if (server) {
      ++servers;
      if (!only_server || proc.pid < only_server->pid) only_server = &proc;
    }
    if (!proc.cmdline_matches_display) continue;
    if (!cmdline_any || proc.pid < cmdline_any->pid) cmdline_any = &proc;
    if (server && (!cmdline_server || proc.pid < cmdline_server->pid)) cmdline_server = &proc;
  }
  // A command line that names this display and a display-server comm is
  // the listener. One display-server comm for this uid is next (sudo's
  // argv also contains the display, and must not win). Then any command
  // line match, then the only process with this uid.
  if (cmdline_server) return cmdline_server->comm;
  if (servers == 1 && only_server) return only_server->comm;
  if (cmdline_any) return cmdline_any->comm;
  if (uid_procs == 1 && only_proc) return only_proc->comm;
  return {};
}

namespace {

// How long a display may sit silent after accept before we stop waiting.
// A healthy local server answers the connection setup in well under this.
constexpr int kXReplyTimeoutMs = 400;

int x11_tcp_port(int display) {
  if (display < 0) return -1;
  const long port = 6000L + static_cast<long>(display);
  if (port <= 0 || port > 65535L) return -1;
  return static_cast<int>(port);
}

bool xlib_uses_unix_socket(const XDisplayParsed& parsed) {
  const std::string proto = lower_copy(parsed.protocol);
  if (proto == "unix" || proto == "local") return true;
  if (!proto.empty()) return false;
  if (parsed.host.empty() || lower_copy(parsed.host) == "unix") return true;
  return false;
}

enum class IoKind { Ok, TimedOut, Failed };

IoKind connect_with_timeout(int fd, const sockaddr* addr, socklen_t len, int timeout_ms) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return IoKind::Failed;
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return IoKind::Failed;
  if (::connect(fd, addr, len) == 0) return IoKind::Ok;
  if (errno != EINPROGRESS) return IoKind::Failed;
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLOUT;
  const int pr = ::poll(&pfd, 1, timeout_ms);
  if (pr < 0) return IoKind::Failed;
  if (pr == 0) return IoKind::TimedOut;
  int err = 0;
  socklen_t elen = sizeof(err);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0) return IoKind::Failed;
  if (err == 0) return IoKind::Ok;
  return IoKind::Failed;
}

IoKind send_all(int fd, const void* data, size_t n, int timeout_ms) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  size_t off = 0;
  const auto start = std::chrono::steady_clock::now();
  while (off < n) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (elapsed >= timeout_ms) return IoKind::TimedOut;
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int pr = ::poll(&pfd, 1, static_cast<int>(timeout_ms - elapsed));
    if (pr < 0) {
      if (errno == EINTR) continue;
      return IoKind::Failed;
    }
    if (pr == 0) return IoKind::TimedOut;
    const ssize_t wrote = ::send(fd, bytes + off, n - off, MSG_NOSIGNAL);
    if (wrote < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      return IoKind::Failed;
    }
    if (wrote == 0) return IoKind::Failed;
    off += static_cast<size_t>(wrote);
  }
  return IoKind::Ok;
}

IoKind read_exact(int fd, unsigned char* buf, size_t n, int timeout_ms) {
  size_t off = 0;
  const auto start = std::chrono::steady_clock::now();
  while (off < n) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (elapsed >= timeout_ms) return IoKind::TimedOut;
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, static_cast<int>(timeout_ms - elapsed));
    if (pr < 0) {
      if (errno == EINTR) continue;
      return IoKind::Failed;
    }
    if (pr == 0) return IoKind::TimedOut;
    const ssize_t got = ::recv(fd, buf + off, n - off, 0);
    if (got < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      return IoKind::Failed;
    }
    if (got == 0) return IoKind::Failed;
    off += static_cast<size_t>(got);
  }
  return IoKind::Ok;
}

// Result of one connection attempt. Reset means the peer accepted and then
// dropped us (Xvfb does that while it is still finishing the previous
// client). TimedOut means it accepted and never spoke. Absent means nothing
// was listening.
enum class ProbeKind { Absent, Answered, TimedOut, Reset };

struct ProbeResult {
  ProbeKind kind = ProbeKind::Absent;
  bool have_pid = false;
  std::string comm;
};

int connect_unix_display(int display, bool abstract_ns, int timeout_ms, IoKind* kind) {
  *kind = IoKind::Failed;
  const std::string path = "/tmp/.X11-unix/X" + std::to_string(display);
  if (path.size() + 1 >= sizeof(sockaddr_un::sun_path)) return -1;
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  socklen_t len = 0;
  if (abstract_ns) {
    addr.sun_path[0] = '\0';
    std::memcpy(addr.sun_path + 1, path.data(), path.size());
    len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + path.size());
  } else {
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    len = sizeof(addr);
  }
  *kind = connect_with_timeout(fd, reinterpret_cast<sockaddr*>(&addr), len, timeout_ms);
  if (*kind != IoKind::Ok) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int connect_ipv4_port(const std::string& host, int port, int timeout_ms, IoKind* kind) {
  *kind = IoKind::Failed;
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    return -1;
  }
  *kind = connect_with_timeout(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr), timeout_ms);
  if (*kind != IoKind::Ok) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int connect_ipv6_loopback(int port, int timeout_ms, IoKind* kind) {
  *kind = IoKind::Failed;
  const int fd = ::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET6, "::1", &addr.sin6_addr) != 1) {
    ::close(fd);
    return -1;
  }
  *kind = connect_with_timeout(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr), timeout_ms);
  if (*kind != IoKind::Ok) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// Read the X11 setup reply and name the peer from this socket. The fd is
// closed before return. A silent peer is TimedOut so the caller never
// enters XOpenDisplay. The extra setup bytes are drained so the server is
// not left mid-write, which is what makes the next connect get ECONNRESET.
ProbeResult handshake_display(int fd, int timeout_ms) {
  ProbeResult out;
  unsigned char req[12] = {};
  req[0] = 'l';
  req[2] = 11;
  const IoKind sent = send_all(fd, req, sizeof req, timeout_ms);
  if (sent != IoKind::Ok) {
    ::close(fd);
    out.kind = sent == IoKind::TimedOut ? ProbeKind::TimedOut : ProbeKind::Reset;
    return out;
  }
  unsigned char hdr[8];
  const IoKind hdr_io = read_exact(fd, hdr, sizeof hdr, timeout_ms);
  if (hdr_io != IoKind::Ok) {
    ::close(fd);
    out.kind = hdr_io == IoKind::TimedOut ? ProbeKind::TimedOut : ProbeKind::Reset;
    return out;
  }
  const XPeerCred peer = x_peer_cred_from_fd(fd);
  out.kind = ProbeKind::Answered;
  out.have_pid = peer.have_pid;
  out.comm = peer.comm;
  const unsigned extra_units = static_cast<unsigned>(hdr[6]) | (static_cast<unsigned>(hdr[7]) << 8);
  unsigned extra = extra_units * 4u;
  if (extra > 65536u) extra = 65536u;
  unsigned char buf[512];
  while (extra > 0) {
    const size_t chunk = extra < sizeof buf ? extra : sizeof buf;
    if (read_exact(fd, buf, chunk, timeout_ms) != IoKind::Ok) break;
    extra -= static_cast<unsigned>(chunk);
  }
  ::close(fd);
  return out;
}

ProbeResult probe_one_fd(int fd, IoKind connect_kind) {
  ProbeResult out;
  if (connect_kind == IoKind::TimedOut) {
    out.kind = ProbeKind::TimedOut;
    return out;
  }
  if (fd < 0) return out;
  return handshake_display(fd, kXReplyTimeoutMs);
}

// Prefer a definitive answer. A timeout on a socket that accepted is the
// wedged server: do not dial the next address and wait again.
ProbeResult probe_unix_once(int display, int timeout_ms) {
  IoKind kind = IoKind::Failed;
  const int path_fd = connect_unix_display(display, false, timeout_ms, &kind);
  if (kind == IoKind::TimedOut) return probe_one_fd(-1, kind);
  if (path_fd >= 0) return handshake_display(path_fd, timeout_ms);
  const int abstract_fd = connect_unix_display(display, true, timeout_ms, &kind);
  if (kind == IoKind::TimedOut) return probe_one_fd(-1, kind);
  if (abstract_fd < 0) return {};
  return handshake_display(abstract_fd, timeout_ms);
}

ProbeResult probe_tcp_once(const XDisplayParsed& parsed, int timeout_ms) {
  const int port = x11_tcp_port(parsed.display);
  if (port < 0) return {};
  const std::string host = lower_copy(parsed.host);
  auto finish = [&](int fd, IoKind kind) {
    if (kind == IoKind::TimedOut) return probe_one_fd(-1, kind);
    if (fd < 0) {
      ProbeResult absent;
      return absent;
    }
    return handshake_display(fd, timeout_ms);
  };
  if (host == "::1" || host == "0:0:0:0:0:0:0:1") {
    IoKind kind = IoKind::Failed;
    const int fd = connect_ipv6_loopback(port, timeout_ms, &kind);
    return finish(fd, kind);
  }
  if (ipv4_loopback(host)) {
    IoKind kind = IoKind::Failed;
    const int fd = connect_ipv4_port(host, port, timeout_ms, &kind);
    return finish(fd, kind);
  }
  // localhost: Xlib tries IPv6 first on this resolver order, then IPv4.
  // A reset on one address must not be forgotten when the other address
  // has no listener, or the retry loop would treat it as "nothing there".
  bool saw_reset = false;
  {
    IoKind kind = IoKind::Failed;
    const int fd = connect_ipv6_loopback(port, timeout_ms, &kind);
    if (kind == IoKind::TimedOut) return probe_one_fd(-1, kind);
    if (fd >= 0) {
      ProbeResult answered = handshake_display(fd, timeout_ms);
      if (answered.kind == ProbeKind::Answered || answered.kind == ProbeKind::TimedOut) return answered;
      saw_reset = answered.kind == ProbeKind::Reset;
    }
  }
  IoKind kind = IoKind::Failed;
  const int fd = connect_ipv4_port("127.0.0.1", port, timeout_ms, &kind);
  ProbeResult v4 = finish(fd, kind);
  if (v4.kind == ProbeKind::Absent && saw_reset) v4.kind = ProbeKind::Reset;
  return v4;
}

ProbeResult probe_display_once(const std::string& spec, int timeout_ms) {
  const XDisplayParsed parsed = parse_x_display(spec);
  if (!parsed.ok || parsed.transport == XDisplayTransport::Remote) return {};
  if (xlib_uses_unix_socket(parsed)) return probe_unix_once(parsed.display, timeout_ms);
  return probe_tcp_once(parsed, timeout_ms);
}

// A healthy Xvfb resets a connect that arrives while it is still tearing
// down the previous client. Those failures are immediate. A silent socket
// is not retried: one poll timeout is the whole wait.
ProbeResult probe_display(const std::string& spec, int timeout_ms) {
  constexpr int kAttempts = 5;
  constexpr useconds_t kGapUs = 20000;
  ProbeResult last;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    last = probe_display_once(spec, timeout_ms);
    if (last.kind != ProbeKind::Reset) return last;
    if (attempt + 1 < kAttempts) ::usleep(kGapUs);
  }
  return last;
}

uid_t uid_of_pid(pid_t pid) {
  if (pid <= 0) return static_cast<uid_t>(-1);
  std::ifstream in("/proc/" + std::to_string(pid) + "/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("Uid:", 0) != 0) continue;
    std::istringstream iss(line.substr(4));
    unsigned long real_uid = 0;
    unsigned long euid = 0;
    if (!(iss >> real_uid >> euid)) return static_cast<uid_t>(-1);
    (void)real_uid;
    return static_cast<uid_t>(euid);
  }
  return static_cast<uid_t>(-1);
}

bool process_cmdline_has_display(pid_t pid, int display) {
  if (pid <= 0 || display < 0) return false;
  std::ifstream in("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
  if (!in) return false;
  const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::string suffix = ":" + std::to_string(display);
  std::string token;
  auto matches = [&](const std::string& tok) {
    if (tok.empty() || tok.size() < suffix.size()) return false;
    if (tok.compare(tok.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    if (tok.size() == suffix.size()) return true;
    const unsigned char prev = static_cast<unsigned char>(tok[tok.size() - suffix.size() - 1]);
    return !std::isdigit(prev);
  };
  for (char c : data) {
    if (c == '\0') {
      if (matches(token)) return true;
      token.clear();
      continue;
    }
    token.push_back(c);
  }
  return matches(token);
}

// Proc-fd walk for the unix socket. A root server's fd directory is not
// readable; that case is handled by SO_PEERCRED on the live connection.
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

std::string server_comm_via_tcp(int display) {
  const int port = x11_tcp_port(display);
  if (port < 0) return {};
  std::vector<TcpListenEntry> listeners;
  auto take = [&](const char* path) {
    std::ifstream in(path);
    if (!in) return;
    for (const TcpListenEntry& entry : parse_proc_net_tcp(in)) {
      if (!entry.listening || entry.inode == 0) continue;
      if (entry.port != static_cast<unsigned>(port)) continue;
      listeners.push_back(entry);
    }
  };
  take("/proc/net/tcp");
  take("/proc/net/tcp6");
  if (listeners.empty()) return {};

  std::unordered_set<unsigned long> want;
  std::unordered_set<uid_t> owner_uids;
  for (const TcpListenEntry& entry : listeners) {
    want.insert(entry.inode);
    owner_uids.insert(entry.uid);
  }

  std::vector<ProcFdRecord> procs;
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
    if (!fds) {
      const uid_t uid = uid_of_pid(pid);
      if (!owner_uids.count(uid)) continue;
      ProcFdRecord rec;
      rec.pid = pid;
      rec.uid = uid;
      rec.comm = comm_of_pid(pid);
      rec.fd_dir_readable = false;
      rec.cmdline_matches_display = process_cmdline_has_display(pid, display);
      if (!rec.comm.empty()) procs.push_back(std::move(rec));
      continue;
    }
    std::vector<unsigned long> held;
    while (dirent* fd = readdir(fds)) {
      if (fd->d_name[0] == '.') continue;
      const std::string link_path = fd_dir_path + "/" + fd->d_name;
      char buf[96];
      const ssize_t n = ::readlink(link_path.c_str(), buf, sizeof(buf));
      if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) continue;
      unsigned long inode = 0;
      if (!parse_socket_inode(std::string(buf, static_cast<size_t>(n)), inode)) continue;
      if (!want.count(inode)) continue;
      held.push_back(inode);
      break;
    }
    closedir(fds);
    if (held.empty()) continue;
    ProcFdRecord rec;
    rec.pid = pid;
    rec.uid = uid_of_pid(pid);
    rec.comm = comm_of_pid(pid);
    rec.fd_dir_readable = true;
    rec.socket_inodes = std::move(held);
    if (!rec.comm.empty()) procs.push_back(std::move(rec));
  }
  closedir(proc);
  return comm_owning_tcp_listeners(listeners, procs);
}

std::string server_comm_from_proc(int display) {
  const std::string unix_comm = server_comm_via_proc(display);
  if (!unix_comm.empty()) return unix_comm;
  return server_comm_via_tcp(display);
}

struct XOpenComm {
  bool opened = false;
  bool have_pid = false;
  bool timed_out = false;
  std::string comm;
};

// Name the server from a connection we open ourselves. XOpenDisplay is not
// used: it has no timeout, and a second XOpenDisplay(":N") in this process
// can fail after an earlier one was closed.
XOpenComm comm_from_opened_display(const std::string& spec) {
  XOpenComm out;
  if (spec.empty()) return out;
  const ProbeResult probe = probe_display(spec, kXReplyTimeoutMs);
  if (probe.kind == ProbeKind::TimedOut) {
    out.timed_out = true;
    return out;
  }
  if (probe.kind != ProbeKind::Answered) return out;
  out.opened = true;
  out.have_pid = probe.have_pid;
  out.comm = probe.comm;
  return out;
}

}  // namespace

std::string server_comm_owning_display(const std::string& display_spec) {
  const XDisplayParsed parsed = parse_x_display(display_spec);
  if (!parsed.ok || parsed.transport == XDisplayTransport::Remote) return {};
  if (parsed.transport == XDisplayTransport::LocalUnix) {
    XOpenComm opened = comm_from_opened_display(display_spec);
    // A silence timeout is the wedged server. Another spelling of the same
    // display would wait again, so stop dialing and use the proc walk.
    if (!opened.opened && !opened.timed_out) {
      const std::string simple = ":" + std::to_string(parsed.display);
      if (simple != display_spec) opened = comm_from_opened_display(simple);
    }
    if (!opened.opened && !opened.timed_out) {
      const std::string unix_form = "unix/:" + std::to_string(parsed.display);
      if (unix_form != display_spec) opened = comm_from_opened_display(unix_form);
    }
    if (opened.have_pid) {
      // The peer pid is the server. An unreadable comm stays empty rather
      // than being replaced by some other process from the proc walk.
      return opened.comm;
    }
    if (opened.opened) {
      // The connection is up, but SO_PEERCRED did not name a process.
      // Loopback TCP does that. The listening port is the server.
      const std::string tcp_comm = server_comm_via_tcp(parsed.display);
      if (!tcp_comm.empty()) return tcp_comm;
      return server_comm_via_proc(parsed.display);
    }
  }
  return server_comm_from_proc(parsed.display);
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
    // A zero peer pid (TCP) does not mean "Unknown GPU". Resolve the
    // display from its listening socket instead of opening it again.
    const XDisplayParsed parsed = parse_x_display(display_spec);
    if (!parsed.ok) return "Unknown GPU";
    if (parsed.transport == XDisplayTransport::Remote) return "Remote display";
    return virtual_display_label_for_server(server_comm_from_proc(parsed.display));
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
  // The alternate path exists only in test builds.
  if (!host.ok) {
#if defined(LUNDUKE_ABOUT_TEST_HOOKS)
    if (const char* env = std::getenv("LUNDUKE_ABOUT_OS_RELEASE")) {
      if (env[0] != '\0') {
        std::ifstream in(env);
        if (in) {
          fallback = parse_os_release(in);
          allow_fallback = true;
        }
      }
    }
#endif
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
