// SPDX-License-Identifier: GPL-3.0-or-later
#include "system_info.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

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

long read_mem_total_kb() {
  std::ifstream in("/proc/meminfo");
  std::string key;
  long value = 0;
  std::string unit;
  while (in >> key >> value >> unit) {
    if (key == "MemTotal:") return value;
  }
  return 0;
}

std::string read_gpu_lspci() {
  std::string best;

  auto try_lspci = [&](const char* cmd) {
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return;
    char buf[512];
    std::vector<std::string> lines;
    while (fgets(buf, sizeof(buf), pipe)) lines.emplace_back(buf);
    pclose(pipe);

    for (const auto& raw : lines) {
      std::string line = trim(raw);
      std::string lower = line;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      bool match = lower.find("vga") != std::string::npos ||
                   lower.find("\"3d") != std::string::npos ||
                   lower.find(" 3d ") != std::string::npos ||
                   lower.find("display controller") != std::string::npos;
      if (!match) continue;

      std::string rest = line;
      auto colon = line.find(": ");
      if (colon != std::string::npos) rest = line.substr(colon + 2);

      if (line.find('"') != std::string::npos) {
        std::vector<std::string> fields;
        bool in = false;
        std::string cur;
        for (char c : line) {
          if (c == '"') {
            if (in) {
              fields.push_back(cur);
              cur.clear();
            }
            in = !in;
          } else if (in) {
            cur.push_back(c);
          }
        }
        if (fields.size() >= 3) rest = fields[1] + " " + fields[2];
      } else {
        auto c2 = rest.find(": ");
        if (c2 != std::string::npos) rest = rest.substr(c2 + 2);
        auto rev = rest.rfind(" (rev ");
        if (rev != std::string::npos) rest = rest.substr(0, rev);
      }
      best = trim(rest);
      break;
    }
  };

  try_lspci("lspci -mm 2>/dev/null");
  if (best.empty()) try_lspci("lspci 2>/dev/null");

  // sysfs PCI class 0x03xxxx (VGA / 3D / display)
  if (best.empty()) {
    std::ifstream dir;
    // Walk /sys/bus/pci/devices
    FILE* pipe = popen("ls -1 /sys/bus/pci/devices 2>/dev/null", "r");
    if (pipe) {
      char slot[128];
      while (fgets(slot, sizeof(slot), pipe)) {
        std::string s = trim(slot);
        std::ifstream cls("/sys/bus/pci/devices/" + s + "/class");
        std::string c;
        if (!(cls >> c)) continue;
        // 0x030000 VGA, 0x030200 3D, 0x038000 display
        if (c.rfind("0x03", 0) != 0) continue;
        std::ifstream vend("/sys/bus/pci/devices/" + s + "/vendor");
        std::ifstream dev("/sys/bus/pci/devices/" + s + "/device");
        std::string v, d;
        vend >> v;
        dev >> d;
        std::ifstream uevent("/sys/bus/pci/devices/" + s + "/uevent");
        std::string line, driver;
        while (std::getline(uevent, line)) {
          if (line.rfind("DRIVER=", 0) == 0) driver = line.substr(7);
        }
        best = "PCI " + v + ":" + d;
        if (!driver.empty()) best += " (" + driver + ")";
        break;
      }
      pclose(pipe);
    }
  }

  // DRM uevent
  if (best.empty()) {
    std::ifstream drm("/sys/class/drm/card0/device/uevent");
    std::string line;
    while (std::getline(drm, line)) {
      if (line.rfind("DRIVER=", 0) == 0) {
        best = "DRM: " + line.substr(7);
        break;
      }
    }
  }

  // glxinfo renderer (works under Xvfb / LLVMpipe)
  if (best.empty()) {
    FILE* pipe = popen("glxinfo 2>/dev/null | grep -m1 'OpenGL renderer'", "r");
    if (pipe) {
      char buf[256];
      if (fgets(buf, sizeof(buf), pipe)) {
        std::string line = trim(buf);
        auto colon = line.find(':');
        if (colon != std::string::npos) best = trim(line.substr(colon + 1));
      }
      pclose(pipe);
    }
  }

  // Xvfb / virtual display hint
  if (best.empty()) {
    FILE* pipe = popen("ps -eo comm= 2>/dev/null | grep -x Xvfb", "r");
    bool xvfb = false;
    if (pipe) {
      char buf[64];
      if (fgets(buf, sizeof(buf), pipe)) xvfb = true;
      pclose(pipe);
    }
    if (xvfb) best = "Virtual framebuffer (Xvfb)";
  }

  return best.empty() ? "Unknown GPU" : best;
}

}  // namespace

std::string format_memory_mb(long kb) {
  if (kb <= 0) return "0 MB";
  double mb = static_cast<double>(kb) / 1024.0;
  char buf[64];
  if (mb < 10.0)
    std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
  else
    std::snprintf(buf, sizeof(buf), "%.0f MB", mb);
  return buf;
}

std::string format_memory_human(long kb) {
  if (kb <= 0) return "0 MB";
  double mb = static_cast<double>(kb) / 1024.0;
  char buf[64];
  if (mb >= 1024.0) {
    std::snprintf(buf, sizeof(buf), "%.1f GB", mb / 1024.0);
  } else if (mb < 10.0) {
    std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
  } else {
    std::snprintf(buf, sizeof(buf), "%.0f MB", mb);
  }
  return buf;
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

  info.total_memory_kb = read_mem_total_kb();
  info.total_memory = format_memory_human(info.total_memory_kb);
  info.cpu_model = read_cpu_model();
  info.gpu = read_gpu_lspci();
  return info;
}

}  // namespace lundukeabout
