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
#include <dirent.h>
#include <cctype>

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

bool read_os_release(const std::string& path, std::string& pretty, std::string& name,
                     std::string& version) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    auto key = line.substr(0, eq);
    auto val = unquote(line.substr(eq + 1));
    if (key == "PRETTY_NAME") pretty = val;
    else if (key == "NAME") name = val;
    else if (key == "VERSION") version = val;
  }
  return !pretty.empty() || !name.empty();
}

std::string read_cpu_model() {
  std::ifstream in("/proc/cpuinfo");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("model name", 0) == 0) {
      auto colon = line.find(':');
      if (colon != std::string::npos)
        return trim(line.substr(colon + 1));
    }
  }
  // Fallback (e.g. some ARM)
  in.clear();
  in.open("/proc/cpuinfo");
  while (std::getline(in, line)) {
    if (line.rfind("Hardware", 0) == 0 || line.rfind("Processor", 0) == 0) {
      auto colon = line.find(':');
      if (colon != std::string::npos)
        return trim(line.substr(colon + 1));
    }
  }
  return "Unknown CPU";
}

struct MemSnapshot {
  bool saw_available = false;
  long total = 0;
  long available = 0;
  long free_kb = 0;
  long buffers = 0;
  long cached = 0;
};

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

MemSnapshot read_mem_snapshot() {
  MemSnapshot snap;
  std::ifstream in("/proc/meminfo");
  std::string line;
  while (std::getline(in, line)) {
    std::string key;
    long value = 0;
    if (!parse_meminfo_line(line, key, value)) continue;
    if (key == "MemTotal:") {
      snap.total = value;
    } else if (key == "MemAvailable:") {
      snap.available = value;
      snap.saw_available = true;
    } else if (key == "MemFree:") {
      snap.free_kb = value;
    } else if (key == "Buffers:") {
      snap.buffers = value;
    } else if (key == "Cached:") {
      snap.cached = value;
    }
  }
  return snap;
}

void apply_memory_fields(SystemInfo& info) {
  const MemSnapshot snap = read_mem_snapshot();
  info.total_memory_kb = snap.total;
  // MemAvailable: 0 is real (nothing reclaimable). Only an absent field
  // falls back to the older MemFree + Buffers + Cached estimate.
  if (snap.saw_available) info.available_memory_kb = snap.available;
  else info.available_memory_kb = snap.free_kb + snap.buffers + snap.cached;
  if (info.available_memory_kb > info.total_memory_kb)
    info.available_memory_kb = info.total_memory_kb;
  info.used_memory_kb = info.total_memory_kb - info.available_memory_kb;
  if (info.used_memory_kb < 0) info.used_memory_kb = 0;
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

bool comm_is_xvfb() {
  DIR* dir = opendir("/proc");
  if (!dir) return false;
  bool found = false;
  while (dirent* de = readdir(dir)) {
    if (!std::isdigit(static_cast<unsigned char>(de->d_name[0]))) continue;
    std::ifstream in(std::string("/proc/") + de->d_name + "/comm");
    std::string comm;
    if (!std::getline(in, comm)) continue;
    if (!comm.empty() && comm.back() == '\n') comm.pop_back();
    if (comm == "Xvfb") {
      found = true;
      break;
    }
  }
  closedir(dir);
  return found;
}

// sysfs only on the startup path. lspci / glxinfo are not launched.
// The string is cached for the process.
std::string read_gpu() {
  static bool cached = false;
  static std::string value;
  if (cached) return value;
  cached = true;

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
  if (value.empty() && comm_is_xvfb()) value = "Virtual framebuffer (Xvfb)";
  if (value.empty()) value = "Unknown GPU";
  return value;
}

enum class MemUnit { MbTenth, MbInt, GbTenth };

MemUnit unit_for_kb(long long mag_kb) {
  if (mag_kb < 0) mag_kb = -mag_kb;
  if (mag_kb >= 1024LL * 1024LL) return MemUnit::GbTenth;
  if (mag_kb >= 10LL * 1024LL) return MemUnit::MbInt;
  return MemUnit::MbTenth;
}

long long round_units(long long kb, MemUnit unit) {
  const long long sign = kb < 0 ? -1 : 1;
  const long long mag = kb < 0 ? -kb : kb;
  long long denom = 1024;
  long long scale = 1;
  if (unit == MemUnit::GbTenth) {
    denom = 1024LL * 1024LL;
    scale = 10;
  } else if (unit == MemUnit::MbTenth) {
    scale = 10;
  }
  return sign * ((mag * scale + denom / 2) / denom);
}

std::string format_units(long long units, MemUnit unit) {
  const bool neg = units < 0;
  const long long mag = neg ? -units : units;
  const char* sign = neg ? "-" : "";
  char buf[64];
  if (unit == MemUnit::MbInt) {
    std::snprintf(buf, sizeof(buf), "%s%lld MB", sign, mag);
  } else {
    const char* suffix = unit == MemUnit::GbTenth ? "GB" : "MB";
    std::snprintf(buf, sizeof(buf), "%s%lld.%lld %s", sign, mag / 10, mag % 10, suffix);
  }
  return buf;
}

}  // namespace

std::string format_memory_human(long kb) {
  if (kb == 0) return "0 MB";
  const MemUnit unit = unit_for_kb(kb);
  return format_units(round_units(kb, unit), unit);
}

MemoryReadout format_memory_readout(long used_kb, long total_kb) {
  MemoryReadout out;
  if (used_kb == 0 && total_kb == 0) {
    out.total = out.used = out.free = "0 MB";
    return out;
  }
  const MemUnit unit = unit_for_kb(total_kb);
  const long long total_units = round_units(total_kb, unit);
  const long long used_units = round_units(used_kb, unit);
  out.total = format_units(total_units, unit);
  out.used = format_units(used_units, unit);
  out.free = format_units(total_units - used_units, unit);
  return out;
}

SystemInfo gather_system_info() {
  SystemInfo info;
  std::string pretty, name, version;

  // Prefer live /etc/os-release; if not LCOS, try recipe sample for demo authenticity.
  const char* sample =
      "/workspace/lcos-live-07/config/includes.chroot/etc/os-release";

  bool ok = read_os_release("/etc/os-release", pretty, name, version);
  bool is_lcos = false;
  if (ok) {
    std::string lower = pretty + " " + name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    is_lcos = lower.find("lcos") != std::string::npos;
  }

  if (!is_lcos) {
    std::string sp, sn, sv;
    if (read_os_release(sample, sp, sn, sv)) {
      pretty = sp;
      name = sn;
      version = sv;
      is_lcos = true;
    }
  }

  if (!pretty.empty()) {
    info.os_pretty = pretty;
  } else if (!name.empty()) {
    info.os_pretty = version.empty() ? name : (name + " " + version);
  } else {
    info.os_pretty = "Unknown OS";
  }

  apply_memory_fields(info);
  info.cpu_model = read_cpu_model();
  info.gpu = read_gpu();
  return info;
}

void refresh_memory_usage(SystemInfo& info) {
  apply_memory_fields(info);
}

}  // namespace lundukeabout
