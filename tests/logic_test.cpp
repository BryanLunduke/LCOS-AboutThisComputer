// SPDX-License-Identifier: GPL-3.0-or-later
#include "about_logic.hpp"
#include "system_info.hpp"

#ifndef LUNDUKE_ABOUT_FIXTURE_DIR
#define LUNDUKE_ABOUT_FIXTURE_DIR ""
#endif

#include <X11/Xlib.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <dirent.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, int line) {
  if (cond) return;
  std::cerr << "FAIL " << line << ": " << expr << "\n";
  ++g_failures;
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

lundukeabout::WindowFact fact(unsigned long xid, pid_t pid, bool has_pid,
                              lundukeabout::WindowKind kind) {
  lundukeabout::WindowFact w;
  w.xid = xid;
  w.pid = pid;
  w.has_pid = has_pid;
  w.kind = kind;
  w.res_class = "App";
  w.title = "Document";
  w.has_title = true;
  return w;
}

struct ParsedAmount {
  long long scaled = 0;  // tenths, or whole * 10 when the text has no fraction
  std::string unit;
  bool fraction = false;
};

bool parse_amount(const std::string& text, ParsedAmount& out) {
  std::string number;
  std::string unit;
  bool in_unit = false;
  for (char c : text) {
    if (c == ' ') {
      in_unit = true;
      continue;
    }
    if (in_unit) unit.push_back(c);
    else number.push_back(c);
  }
  if (number.empty() || (unit != "MB" && unit != "GB")) return false;
  const bool neg = !number.empty() && number[0] == '-';
  if (neg) number.erase(number.begin());
  const auto dot = number.find('.');
  long long whole = 0;
  long long frac = 0;
  bool fraction = false;
  try {
    if (dot == std::string::npos) {
      whole = std::stoll(number);
    } else {
      fraction = true;
      whole = std::stoll(number.substr(0, dot));
      const std::string frac_text = number.substr(dot + 1);
      if (frac_text.size() != 1) return false;
      frac = std::stoll(frac_text);
    }
  } catch (...) {
    return false;
  }
  out.unit = unit;
  out.fraction = fraction;
  out.scaled = (whole * 10 + frac) * (neg ? -1 : 1);
  return true;
}

long elapsed_ms(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

bool cmdline_names_display(pid_t pid, int display) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
  if (!in) return false;
  const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::string needle = ":" + std::to_string(display);
  std::string token;
  bool saw_xvfb = false;
  bool saw_display = false;
  auto flush = [&]() {
    if (token == "Xvfb" || token == "sudo") saw_xvfb = true;
    if (token.size() >= needle.size() &&
        token.compare(token.size() - needle.size(), needle.size(), needle) == 0) {
      saw_display = true;
    }
    token.clear();
  };
  for (char c : data) {
    if (c == '\0') flush();
    else token.push_back(c);
  }
  flush();
  return saw_xvfb && saw_display;
}

void sudo_kill_display(int display) {
  if (display < 0) return;
  DIR* proc = opendir("/proc");
  if (!proc) return;
  std::vector<pid_t> victims;
  while (dirent* de = readdir(proc)) {
    if (!std::isdigit(static_cast<unsigned char>(de->d_name[0]))) continue;
    char* end = nullptr;
    const unsigned long pid_ul = std::strtoul(de->d_name, &end, 10);
    if (!end || *end != '\0' || pid_ul == 0) continue;
    const pid_t pid = static_cast<pid_t>(pid_ul);
    if (pid == getpid()) continue;
    if (cmdline_names_display(pid, display)) victims.push_back(pid);
  }
  closedir(proc);
  for (pid_t pid : victims) {
    const pid_t killer = fork();
    if (killer == 0) {
      const std::string text = std::to_string(pid);
      execlp("sudo", "sudo", "-n", "kill", "-TERM", text.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    if (killer > 0) waitpid(killer, nullptr, 0);
  }
}

bool tcp_port_accepts(int port) {
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
    close(fd);
    return false;
  }
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  const int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  bool ok = rc == 0;
  if (!ok && errno == EINPROGRESS) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    if (poll(&pfd, 1, 100) > 0) {
      int err = 0;
      socklen_t len = sizeof(err);
      ok = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
    }
  }
  close(fd);
  return ok;
}

}  // namespace

int main() {
  using namespace lundukeabout;

  // Malformed _NET_WM_ICON must not spin. product + 2 wraps to 0 for this pair.
  {
    const unsigned long evil[] = {(1UL << 63) - 1, 2};
    CHECK(!choose_net_wm_icon(evil, ~0UL));
  }
  {
    const unsigned long zero[] = {0, 1, 32, 1};
    CHECK(!choose_net_wm_icon(zero, 4));
  }
  {
    std::vector<unsigned long> data{1, 1, 0x11, (1UL << 63) - 1, 2};
    const auto choice = choose_net_wm_icon(data.data(), ~0UL);
    CHECK(choice.has_value());
    if (choice) {
      CHECK(choice->width == 1);
      CHECK(choice->height == 1);
    }
  }
  {
    std::vector<unsigned long> data;
    data.push_back(8);
    data.push_back(1);
    for (int i = 0; i < 8; ++i) data.push_back(1);
    data.push_back(32);
    data.push_back(1);
    for (int i = 0; i < 32; ++i) data.push_back(2);
    const auto choice = choose_net_wm_icon(data.data(), data.size());
    CHECK(choice.has_value());
    if (choice) CHECK(choice->width == 32);
  }
  CHECK(kNetWmIconMaxItems <= 256ul * 1024ul);
  CHECK(kNetWmIconMaxItems >= 64ul * 1024ul);

  // Client list pages past the old 1024 cap and stops if the server lies.
  {
    constexpr long total = 2500;
    constexpr long chunk = 1024;
    long offset = 0;
    long got = 0;
    int fetches = 0;
    while (got < total + chunk) {
      ++fetches;
      const long count = std::max(0L, std::min(chunk, total - offset));
      got += count;
      const unsigned long bytes_after = (offset + count < total) ? 4ul : 0ul;
      long next = 0;
      if (!client_list_advance(offset, static_cast<unsigned long>(count), bytes_after,
                               kMaxClientIds, next)) {
        break;
      }
      offset = next;
    }
    CHECK(got == total);
    CHECK(fetches == 3);
  }
  {
    long offset = 0;
    int fetches = 0;
    while (fetches < 100000) {
      ++fetches;
      long next = 0;
      if (!client_list_advance(offset, 1024, 4, kMaxClientIds, next)) break;
      offset = next;
    }
    CHECK(fetches < 200);
    CHECK(fetches > 90);
  }

  // Descendant RssAnon: a child with its own row is counted once, on that row.
  {
    std::unordered_map<pid_t, long> rss{{10, 100}, {11, 50}, {12, 25}, {13, 5}};
    std::unordered_map<pid_t, std::vector<pid_t>> children{{10, {11, 12}}, {12, {13}}};
    const std::unordered_set<pid_t> rows{10, 12};
    CHECK(rollup_rss_anon(10, rss, children, rows) == 150);
    CHECK(rollup_rss_anon(12, rss, children, rows) == 30);
  }

  // Protection is exact class or comm, never a title substring.
  {
    std::string reason;
    CHECK(is_protected_identity(ClassHint{"Firefox", "Firefox"}, "", reason) == false);
    CHECK(is_protected_identity(ClassHint{"Document - xfwm4", "Firefox"}, "", reason) == false);
    CHECK(is_protected_identity(ClassHint{"xfwm4", "Firefox"}, "", reason));
    CHECK(reason == "Window manager");
    CHECK(is_protected_identity(ClassHint{"", "Firefox"}, "xfce4-notifyd", reason));
    CHECK(reason == "Notifications");
    CHECK(is_protected_identity(ClassHint{"", ""}, "polkit-gnome-au", reason));
    CHECK(reason == "System service");
    CHECK(is_protected_identity(ClassHint{"", ""}, "polkit-gnome-ax", reason) == false);
    CHECK(is_protected_identity(ClassHint{"", ""}, "xfce4-screensav", reason));
    CHECK(reason == "Screensaver");
    CHECK(is_protected_identity(ClassHint{"", ""}, "xfce4-screensaver", reason));
    CHECK(is_protected_identity(ClassHint{"", ""}, "xfce", reason) == false);
  }

  // Group by PID. Active window wins. Splash is not the representative.
  // A foreign WM_CLASS does not protect a pid whose comm is something else.
  // The real window manager is protected by its comm.
  {
    WindowFact main = fact(1, 10, true, WindowKind::Normal);
    main.res_class = "Firefox";
    main.title = "xfwm4 docs";
    main.has_icon = true;
    main.comm = "firefox";
    main.identity_ok = true;
    main.start_ticks = 50;
    WindowFact splash = fact(2, 10, true, WindowKind::Splash);
    splash.res_class = "xfwm4";
    splash.comm = "firefox";
    splash.identity_ok = true;
    splash.start_ticks = 50;
    splash.active = true;
    splash.title = "Splash";
    const auto grouped = group_windows({main, splash});
    CHECK(grouped.size() == 1);
    if (!grouped.empty()) {
      CHECK(grouped[0].xid == 1);
      CHECK(grouped[0].name == "Firefox");
      CHECK(grouped[0].tooltip == "xfwm4 docs");
      CHECK(grouped[0].protected_app == false);
      CHECK(grouped[0].comm == "firefox");
      CHECK(grouped[0].start_ticks == 50);
      CHECK(grouped[0].identity_ok);
    }
  }
  {
    WindowFact main = fact(1, 10, true, WindowKind::Normal);
    main.res_class = "Firefox";
    main.comm = "xfwm4";
    main.identity_ok = true;
    const auto grouped = group_windows({main});
    CHECK(grouped.size() == 1);
    if (!grouped.empty()) {
      CHECK(grouped[0].protected_app);
      CHECK(grouped[0].protect_reason == "Window manager");
    }
  }
  {
    WindowFact untitled = fact(3, 0, false, WindowKind::Normal);
    untitled.res_class.clear();
    untitled.res_name.clear();
    untitled.title.clear();
    untitled.has_title = false;
    CHECK(group_windows({untitled}).empty());
  }
  {
    WindowFact bare = fact(4, 0, false, WindowKind::Normal);
    bare.res_class.clear();
    bare.title = "Clipboard";
    bare.has_title = true;
    const auto grouped = group_windows({bare});
    CHECK(grouped.size() == 1);
    if (!grouped.empty()) {
      CHECK(grouped[0].pid == 0);
      CHECK(grouped[0].has_pid == false);
      CHECK(grouped[0].protect_reason == "No process ID");
      CHECK(grouped[0].protected_app == false);
      CHECK(grouped[0].name == "Clipboard");
    }
  }
  {
    WindowFact older = fact(1, 7, true, WindowKind::Normal);
    older.has_icon = false;
    older.title = "Older";
    WindowFact newer = fact(2, 7, true, WindowKind::Normal);
    newer.has_icon = true;
    newer.title = "Newer";
    newer.active = false;
    const auto grouped = group_windows({older, newer});
    CHECK(grouped.size() == 1);
    if (!grouped.empty()) CHECK(grouped[0].xid == 2);
  }
  {
    WindowFact plain = fact(1, 7, true, WindowKind::Normal);
    plain.has_title = true;
    plain.has_icon = true;
    plain.active = false;
    WindowFact active = fact(2, 7, true, WindowKind::Normal);
    active.has_title = false;
    active.has_icon = false;
    active.title.clear();
    active.active = true;
    active.res_class = "Editor";
    const auto grouped = group_windows({plain, active});
    CHECK(grouped.size() == 1);
    if (!grouped.empty()) CHECK(grouped[0].xid == 2);
  }
  {
    CHECK(group_windows({fact(1, 8, true, WindowKind::Utility)}).size() == 1);
    CHECK(group_windows({fact(1, 8, true, WindowKind::Menu)}).empty());
    CHECK(group_windows({fact(1, 8, true, WindowKind::Tooltip)}).empty());
    CHECK(group_windows({fact(1, 8, true, WindowKind::Notification)}).empty());
    CHECK(group_windows({fact(1, 8, true, WindowKind::Splash)}).empty());
    WindowFact normal = fact(2, 8, true, WindowKind::Normal);
    WindowFact utility = fact(3, 8, true, WindowKind::Utility);
    const auto grouped = group_windows({utility, normal});
    CHECK(grouped.size() == 1);
    if (!grouped.empty()) CHECK(grouped[0].xid == 2);
  }
  {
    WindowFact a = fact(1, 1, true, WindowKind::Normal);
    a.pid = 4;
    WindowFact b = fact(2, 5, true, WindowKind::Normal);
    CHECK(group_windows({a, b}).size() == 2);
  }
  {
    WindowFact noted = fact(9, 4, true, WindowKind::Normal);
    noted.res_class = "Firefox";
    noted.comm = "xfce4-notifyd";
    noted.title = "Hi";
    const auto grouped = group_windows({noted});
    CHECK(grouped.size() == 1);
    if (!grouped.empty()) {
      CHECK(grouped[0].protected_app);
      CHECK(grouped[0].name == "Firefox");
      CHECK(grouped[0].tooltip == "Hi");
    }
  }

  // Same human unit, and used + free equals the rounded total.
  {
    CHECK(format_memory_human(0) == "0 MB");
    CHECK(format_memory_human(9 * 1024) == "9.0 MB");
    CHECK(format_memory_human(10 * 1024) == "10 MB");
    CHECK(format_memory_human(1024 * 1024) == "1.0 GB");
    CHECK(format_memory_human(-1024) == "-1.0 MB");
    CHECK(format_memory_human(11234L * 1024L).find("GB") != std::string::npos);

    const auto readout = format_memory_readout(11234L * 1024L, 16L * 1024L * 1024L);
    CHECK(readout.used == "11.0 GB");
    CHECK(readout.free == "5.0 GB");
    CHECK(readout.total == "16.0 GB");

    const long long samples_used[] = {0, 100, 10 * 1024, 100 * 1024, 1536 * 1024,
                                       16L * 1024L * 1024L + 1000};
    const long long samples_total[] = {0, 8 * 1024, 100 * 1024, 512 * 1024,
                                        16L * 1024L * 1024L};
    for (long long total : samples_total) {
      for (long long used : samples_used) {
        const auto row = format_memory_readout(static_cast<long>(used), static_cast<long>(total));
        ParsedAmount u, f, t;
        CHECK(parse_amount(row.used, u));
        CHECK(parse_amount(row.free, f));
        CHECK(parse_amount(row.total, t));
        CHECK(u.unit == t.unit);
        CHECK(f.unit == t.unit);
        CHECK(u.fraction == t.fraction);
        CHECK(f.fraction == t.fraction);
        CHECK(u.scaled + f.scaled == t.scaled);
      }
    }
  }

  // A negative remainder is not reported as zero.
  {
    const long system = 1000 - 1500;
    CHECK(system == -500);
    const std::string text = format_memory_human(system);
    CHECK(text != "0 MB");
    CHECK(!text.empty() && text[0] == '-');
  }

  CHECK(credits_should_crawl(8, 40, 1));
  CHECK(!credits_should_crawl(1, 40, 1));
  CHECK(!credits_should_crawl(0, 40, 1));
  CHECK(!credits_should_crawl(100, 40, 1));
  CHECK(credits_should_crawl(100, 102, 1));
  CHECK(!credits_should_crawl(100, 101, 1));

  CHECK(clamp_scroll_value(500, 1000, 800) == 200);
  CHECK(clamp_scroll_value(50, 1000, 800) == 50);
  CHECK(clamp_scroll_value(-4, 1000, 800) == 0);
  CHECK(clamp_scroll_value(10, 100, 200) == 0);

  {
    std::istringstream ids(
        "8086  Intel Corporation\n"
        "\t9a49  TigerLake-LP GT2 [Iris Xe Graphics]\n"
        "1002  Advanced Micro Devices, Inc.\n"
        "\t67df  Ellesmere [Radeon RX 580]\n"
        "\t\t1234 abcd  Subsystem\n");
    const PciDb db = parse_pci_ids(ids);
    const std::vector<GpuDevice> both{
        GpuDevice{"0000:00:02.0", 0x030000, 0x8086, 0x9a49, "i915"},
        GpuDevice{"0000:01:00.0", 0x030200, 0x1002, 0x67df, "amdgpu"},
        GpuDevice{"0000:00:1f.3", 0x040300, 0x8086, 0x0001, "snd"},
    };
    const std::string preferred = gpu_label_from_devices(both, db);
    CHECK(preferred.find("Ellesmere") != std::string::npos);
    CHECK(preferred.find("Iris") != std::string::npos);
    CHECK(preferred.find("Iris") < preferred.find("Ellesmere"));
    CHECK(preferred.find("snd") == std::string::npos);
    CHECK(preferred.find(';') != std::string::npos);

    const std::vector<GpuDevice> vga_only{
        GpuDevice{"0000:00:02.0", 0x030000, 0x8086, 0x9a49, "i915"},
    };
    const std::string intel = gpu_label_from_devices(vga_only, db);
    CHECK(intel.find("Iris") != std::string::npos);
    CHECK(intel.find("Intel") != std::string::npos);

    const std::vector<GpuDevice> unknown{
        GpuDevice{"0000:02:00.0", 0x038000, 0x1234, 0xabcd, "bochs-drm"},
    };
    const std::string raw = gpu_label_from_devices(unknown, db);
    CHECK(raw.find("PCI 0x1234:0xabcd") != std::string::npos);
    CHECK(raw.find("bochs-drm") != std::string::npos);
  }

  // Finding 1: format 8 must not be indexed as Atom[].
  CHECK(x_property_indexable_as_longs(8, true, 128, 129) == false);
  CHECK(x_property_indexable_as_longs(16, true, 4, 1024) == false);
  CHECK(x_property_indexable_as_longs(32, false, 1, sizeof(unsigned long)) == false);
  CHECK(x_property_indexable_as_longs(32, true, 1, sizeof(unsigned long)));
  CHECK(x_property_indexable_as_longs(32, true, 2, sizeof(unsigned long)) == false);
  CHECK(x_property_utf8_copy_bytes(32, true, 4, 32) == 0);
  CHECK(x_property_utf8_copy_bytes(8, false, 4, 5) == 0);
  CHECK(x_property_utf8_copy_bytes(8, true, 4, 3) == 3);
  CHECK(x_property_utf8_copy_bytes(8, true, 4, 5) == 4);

  // Finding 4: refresh comm + starttime mismatches a recycled pid.
  {
    ProcSnapshot pinned;
    pinned.ok = true;
    pinned.comm = "firefox";
    pinned.start_ticks = 100;
    ProcSnapshot same = pinned;
    ProcSnapshot recycled = pinned;
    recycled.start_ticks = 200;
    ProcSnapshot renamed = pinned;
    renamed.comm = "bash";
    ProcSnapshot dead;
    CHECK(proc_identity_matches(pinned, same));
    CHECK(proc_identity_matches(pinned, recycled) == false);
    CHECK(proc_identity_matches(pinned, renamed) == false);
    CHECK(proc_identity_matches(pinned, dead) == false);
    CHECK(may_signal_pinned_pid(42, 7, 100, true, 100, false));
    CHECK(may_signal_pinned_pid(42, 7, 100, true, 200, false) == false);
    CHECK(may_signal_pinned_pid(42, 7, 100, false, 100, false) == false);
    CHECK(may_signal_pinned_pid(42, 42, 100, true, 100, false) == false);
    CHECK(may_signal_pinned_pid(42, 7, 100, true, 100, true) == false);
    CHECK(may_signal_pinned_pid(1, 7, 100, true, 100, false) == false);
  }
  {
    const std::string stat = "42 (firefox) S 1 42 42 0 0 0 0 0 0 0 0 0 0 0 20 0 1 0 100 0 0";
    const ProcSnapshot parsed = parse_proc_stat_line(stat);
    CHECK(parsed.ok);
    CHECK(parsed.comm == "firefox");
    CHECK(parsed.start_ticks == 100);
    CHECK(parsed.ppid == 1);
    CHECK(parsed.saw_rss);
    CHECK(parsed.rss_pages == 0);
    CHECK(parse_proc_stat_line("nope").ok == false);
    const std::string full =
        "1 (my app) S 5 1 1 0 -1 0 1 0 0 0 2 3 0 0 20 0 1 0 900 100 40";
    const ProcSnapshot rich = parse_proc_stat_line(full);
    CHECK(rich.ok);
    CHECK(rich.comm == "my app");
    CHECK(rich.ppid == 5);
    CHECK(rich.start_ticks == 900);
    CHECK(rich.saw_rss);
    CHECK(rich.rss_pages == 40);
    const ProcSnapshot short_line = parse_proc_stat_line("9 (short) S 2 1 1 0 0 0 0 0 0 0 0 0 0 0 20 0 1 0 7");
    CHECK(short_line.ok);
    CHECK(short_line.start_ticks == 7);
    CHECK(short_line.ppid == 2);
    CHECK(short_line.saw_rss == false);
  }

  // Finding 7: skip-taskbar-only and dock-only still produce a row.
  {
    WindowFact dock = fact(5, 42, true, WindowKind::DesktopOrDock);
    dock.res_class = "xfce4-panel";
    dock.comm = "xfce4-panel";
    dock.identity_ok = true;
    const auto grouped = group_windows({dock});
    CHECK(grouped.size() == 1);
    if (!grouped.empty()) {
      CHECK(grouped[0].name == "Xfce4 Panel");
      CHECK(grouped[0].protected_app);
      CHECK(grouped[0].protect_reason == "Desktop panel");
    }
    WindowFact skip = fact(6, 43, true, WindowKind::SkipTaskbar);
    skip.res_class = "Discord";
    skip.comm = "Discord";
    skip.identity_ok = true;
    const auto tray = group_windows({skip});
    CHECK(tray.size() == 1);
    if (!tray.empty()) {
      CHECK(tray[0].name == "Discord");
      CHECK(tray[0].protected_app == false);
      CHECK(tray[0].pid == 43);
    }
    WindowFact normal = fact(1, 43, true, WindowKind::Normal);
    normal.res_class = "Discord";
    const auto prefer = group_windows({skip, normal});
    CHECK(prefer.size() == 1);
    if (!prefer.empty()) CHECK(prefer[0].xid == 1);
    std::string why;
    WindowFact foreign = fact(9, 99, true, WindowKind::Normal);
    foreign.res_class = "xfwm4";
    foreign.comm = "firefox";
    CHECK(window_marks_protected(foreign, why) == false);
  }

  // Finding 9: a chunk that ends mid-frame skips to a following 32px frame.
  {
    constexpr unsigned long big_w = 384;
    constexpr unsigned long big_h = 384;
    constexpr unsigned long big_step = big_w * big_h + 2;
    constexpr unsigned long small_at = big_step;
    constexpr unsigned long total = big_step + 2 + 32;
    struct Sparse {
      unsigned long small_at;
    } sparse{small_at};
    auto read = [](void* ctx, unsigned long index, unsigned long& word) -> bool {
      const auto* s = static_cast<const Sparse*>(ctx);
      if (index == 0) {
        word = 384;
        return true;
      }
      if (index == 1) {
        word = 384;
        return true;
      }
      if (index == s->small_at) {
        word = 32;
        return true;
      }
      if (index == s->small_at + 1) {
        word = 1;
        return true;
      }
      return false;
    };
    const auto choice = choose_net_wm_icon_chunked(total, read, &sparse);
    CHECK(choice.has_value());
    if (choice) {
      CHECK(choice->width == 32);
      CHECK(choice->height == 1);
      CHECK(choice->pixel_offset == small_at + 2);
    }
    const unsigned long only_big[] = {300, 1, 0};
    CHECK(!choose_net_wm_icon(only_big, 3));
  }

  // Finding 12: an empty client-list page does not advance.
  {
    long next = 99;
    CHECK(client_list_advance(0, 0, 4, kMaxClientIds, next) == false);
    CHECK(next == 0);
    CHECK(client_list_advance(0, 0, 1, kMaxClientIds, next) == false);
  }

  // Parent cycle terminates, and each pid is summed once.
  {
    std::unordered_map<pid_t, long> rss{{1, 10}, {2, 20}};
    std::unordered_map<pid_t, std::vector<pid_t>> children{{1, {2}}, {2, {1}}};
    CHECK(rollup_rss_anon(1, rss, children, {}) == 30);
    CHECK(rollup_rss_anon(1, rss, children, {2}) == 10);
    std::unordered_map<pid_t, unsigned long long> starts{{10, 10}, {11, 20}, {12, 30}};
    std::unordered_map<pid_t, long> rss3{{10, 10}, {11, 20}, {12, 5}};
    std::unordered_map<pid_t, std::vector<pid_t>> tree{{10, {12}}, {11, {12}}};
    const auto pins = collect_kill_pins(10, rss3, starts, tree, {10, 11});
    bool saw_child = false;
    bool saw_other_row = false;
    for (const auto& pin : pins) {
      if (pin.pid == 12) saw_child = true;
      if (pin.pid == 11) saw_other_row = true;
    }
    CHECK(saw_child);
    CHECK(saw_other_row == false);
    std::vector<ProcPin> overlap = pins;
    overlap.push_back(ProcPin{12, 30, 99});
    CHECK(sum_rss_once(overlap) == sum_rss_once(pins));
  }

  // Finding 2: a posted menu keeps the row that left the snapshot.
  CHECK(may_delete_row(false, true, false) == false);
  CHECK(may_delete_row(false, false, true) == false);
  CHECK(may_delete_row(true, false, false) == false);
  CHECK(may_delete_row(false, false, false));
  CHECK(may_delete_row(false, true, true) == false);

  // Crawl stop clears the tick id and does not need remove_tick_callback.
  CHECK(credits_on_tick(false, false) == CreditsTickResult::StopAndClearId);
  CHECK(credits_on_tick(true, true) == CreditsTickResult::StopAndClearId);
  CHECK(credits_on_tick(true, false) == CreditsTickResult::Continue);

  // Finding 15: LONG_MIN formats, and a negative remainder is shown as 0.
  {
    const std::string min_text = format_memory_human(LONG_MIN);
    CHECK(!min_text.empty());
    CHECK(min_text[0] == '-');
    CHECK(min_text.find("GB") != std::string::npos);
    const SystemRemainder neg = system_remainder_kb(1000, 1500);
    CHECK(neg.raw_kb == -500);
    CHECK(neg.shown_kb == 0);
    CHECK(neg.clamped);
    const SystemRemainder pos = system_remainder_kb(2000, 500);
    CHECK(pos.raw_kb == 1500);
    CHECK(pos.shown_kb == 1500);
    CHECK(pos.clamped == false);
  }

  // MemAvailable absent falls back; MemAvailable 0 is real.
  {
    std::istringstream absent(
        "MemTotal:       1000 kB\n"
        "MemFree:         100 kB\n"
        "Buffers:          10 kB\n"
        "Cached:           20 kB\n");
    const MemoryUsage fallback = memory_usage_from_meminfo(parse_meminfo(absent));
    CHECK(fallback.total_kb == 1000);
    CHECK(fallback.available_kb == 130);
    CHECK(fallback.used_kb == 870);
    std::istringstream zero(
        "MemTotal:       1000 kB\n"
        "MemAvailable:      0 kB\n"
        "MemFree:         100 kB\n"
        "Buffers:          10 kB\n"
        "Cached:           20 kB\n");
    const MemoryUsage none = memory_usage_from_meminfo(parse_meminfo(zero));
    CHECK(none.available_kb == 0);
    CHECK(none.used_kb == 1000);
  }

  // Finding 13: a parsed host os-release is not replaced by a second stream.
  {
    std::istringstream host("PRETTY_NAME=\"Debian GNU/Linux\"\nNAME=Debian\nID=debian\n");
    std::istringstream sample("PRETTY_NAME=\"LCOS 0.7\"\nNAME=LCOS\nID=lcos\n");
    const OsRelease host_rel = parse_os_release(host);
    const OsRelease sample_rel = parse_os_release(sample);
    CHECK(host_rel.ok);
    CHECK(host_rel.id == "debian");
    CHECK(host_rel.pretty == "Debian GNU/Linux");
    CHECK(sample_rel.id == "lcos");
    CHECK(os_display_name(host_rel, sample_rel, true) == "Debian GNU/Linux");
    CHECK(os_display_name(host_rel, sample_rel, false) == "Debian GNU/Linux");
    OsRelease none;
    CHECK(os_display_name(none, sample_rel, false) == "Unknown OS");
    CHECK(os_display_name(none, sample_rel, true) == "LCOS 0.7");
    std::istringstream odd("PRETTY_NAME=\"Computer Operating System\"\nID=lcos\n");
    const OsRelease odd_rel = parse_os_release(odd);
    CHECK(odd_rel.id == "lcos");
    CHECK(os_display_name(odd_rel, sample_rel, true) == "Computer Operating System");
  }

  // Finding 1: pidfd_open errors other than ESRCH fall back to kill().
  // ESRCH is "already exited", not a guess. A refused pidfd_send_signal
  // keeps the pin and reports strerror plus the PID.
  {
    CHECK(pidfd_open_action(-1, EMFILE) == PidfdOpenAction::FallbackKill);
    CHECK(pidfd_open_action(-1, ENOSYS) == PidfdOpenAction::FallbackKill);
    CHECK(pidfd_open_action(-1, ENOMEM) == PidfdOpenAction::FallbackKill);
    CHECK(pidfd_open_action(-1, EPERM) == PidfdOpenAction::FallbackKill);
    CHECK(pidfd_open_action(-1, EINVAL) == PidfdOpenAction::FallbackKill);
    CHECK(pidfd_open_action(-1, ESRCH) == PidfdOpenAction::AlreadyExited);
    CHECK(pidfd_open_action(4, EMFILE) == PidfdOpenAction::SendOnPidfd);
    CHECK(pidfd_signal_failure(ENOSYS) == PidfdSignalFailure::FallbackKill);
    CHECK(pidfd_signal_failure(ESRCH) == PidfdSignalFailure::AlreadyExited);
    CHECK(pidfd_signal_failure(EPERM) == PidfdSignalFailure::ReportErrno);
    const std::string msg = force_close_errno_message(10941, EMFILE);
    CHECK(msg.find("10941") != std::string::npos);
    CHECK(msg.find("Too many open files") != std::string::npos);
    CHECK(msg.find("Already exited") == std::string::npos);
  }

  // Finding 2: the named PID is first. Helpers are not signalled when it fails.
  // A helper error after a successful close names both PIDs.
  {
    const std::vector<ProcPin> pins{ProcPin{11, 2, 50}, ProcPin{10, 1, 100}, ProcPin{12, 3, 5}};
    const ForceCloseOrder order = force_close_signal_order(10, pins);
    CHECK(order.root == 10);
    CHECK(order.helpers.size() == 2);
    CHECK(order.helpers[0] == 11);
    CHECK(order.helpers[1] == 12);
    CHECK(force_close_should_signal_helpers(false) == false);
    CHECK(force_close_should_signal_helpers(true));
    HelperCloseReport report;
    helper_close_note(report, 10, true, "");
    helper_close_note(report, 11, false, "Operation not permitted");
    helper_close_note(report, 12, false, "Already exited.");
    const std::string msg = helper_close_message(report);
    CHECK(msg.find("PID 10") != std::string::npos);
    CHECK(msg.find("PID 11") != std::string::npos);
    CHECK(msg.find("Operation not permitted") != std::string::npos);
    CHECK(msg.find("Already exited") == std::string::npos);
    HelperCloseReport skipped;
    helper_close_note(skipped, 10, true, "");
    helper_close_note(skipped, 11, false, "That PID belongs to another row and was not signalled.");
    CHECK(helper_close_message(skipped).empty());
  }

  // Finding 3: shared anonymous pages are charged once via Pss_Anon.
  {
    const char* parent =
        "Rss: 50000 kB\nPss_Anon: 20702 kB\nPrivate_Dirty: 68 kB\nAnonymous: 41188 kB\n";
    const char* child =
        "Rss: 70000 kB\nPss_Anon: 41106 kB\nPrivate_Dirty: 20540 kB\nAnonymous: 61660 kB\n";
    const SmapsRollup parent_roll = parse_smaps_rollup(parent);
    const SmapsRollup child_roll = parse_smaps_rollup(child);
    CHECK(parent_roll.saw_pss_anon);
    CHECK(parent_roll.pss_anon_kb == 20702);
    const long parent_kb = process_anon_charge_kb(true, parent_roll, 41188);
    const long child_kb = process_anon_charge_kb(true, child_roll, 61660);
    CHECK(parent_kb == 20702);
    CHECK(child_kb == 41106);
    CHECK(parent_kb + child_kb < 41188 + 61660);
    SmapsRollup dirty_only;
    dirty_only.saw_private_dirty = true;
    dirty_only.private_dirty_kb = 80;
    CHECK(process_anon_charge_kb(true, dirty_only, 400) == 80);
    SmapsRollup empty;
    CHECK(process_anon_charge_kb(true, empty, 400) == 400);
    CHECK(process_anon_charge_kb(false, parent_roll, 400) == 400);
    std::vector<ProcPin> charged{ProcPin{1, 1, parent_kb}, ProcPin{2, 2, child_kb}};
    CHECK(sum_rss_once(charged) == parent_kb + child_kb);
  }

  // Finding 4: a non-UTF-8 class and title fall back to the command.
  {
    CHECK(utf8_valid("Caf\xC3\xA9"));
    CHECK(utf8_valid("Caf\xE9") == false);
    CHECK(utf8_valid(std::string("a\0b", 3)) == false);
    WindowFact latin = fact(1, 20, true, WindowKind::Normal);
    latin.res_class = "Caf\xE9";
    latin.res_name = "Caf\xE9";
    latin.title = "Caf\xE9";
    latin.comm = "testwin";
    latin.identity_ok = true;
    const auto grouped = group_windows({latin});
    CHECK(grouped.size() == 1);
    if (!grouped.empty()) {
      CHECK(grouped[0].name == "testwin");
      CHECK(grouped[0].class_name.empty());
      CHECK(utf8_valid(grouped[0].name));
      CHECK(utf8_valid(grouped[0].tooltip));
      CHECK(grouped[0].tooltip == "testwin");
    }
    WindowFact titled = fact(2, 21, true, WindowKind::Normal);
    titled.res_class = "Caf\xE9";
    titled.res_name.clear();
    titled.title = "Hello";
    titled.comm = "testwin";
    const auto via_comm = group_windows({titled});
    CHECK(!via_comm.empty());
    if (!via_comm.empty()) CHECK(via_comm[0].name == "testwin");
    WindowFact cafe = fact(3, 22, true, WindowKind::Normal);
    cafe.res_class = "Caf\xC3\xA9";
    cafe.title = "Cup";
    cafe.comm = "testwin";
    const auto kept = group_windows({cafe});
    CHECK(!kept.empty());
    if (!kept.empty()) {
      CHECK(kept[0].name == "Caf\xC3\xA9");
      CHECK(kept[0].class_name == "Caf\xC3\xA9");
      CHECK(kept[0].tooltip == "Cup");
    }
  }

  // Finding 5: an underscore in the menu label is escaped, so it matches the row.
  {
    CHECK(escape_mnemonic("My_App") == "My__App");
    CHECK(force_close_menu_label("My_App", true, "") == "Force Close My__App");
    CHECK(force_close_menu_label("My_App", false, "No process ID") == "No process ID");
    CHECK(force_close_menu_label("Plain", true, "") == "Force Close Plain");
  }

  // Finding 6: a blank model name is skipped. Hardware is only a fallback.
  {
    std::istringstream blank_then_real("model name\t: \nmodel name\t: Different\n");
    CHECK(cpu_model_from_cpuinfo(blank_then_real) == "Different");
    std::istringstream only_blank("model name\t:\nprocessor\t: 0\n");
    CHECK(cpu_model_from_cpuinfo(only_blank) == "Unknown CPU");
    std::istringstream hardware("model name\t:\nHardware\t: Board\nProcessor\t: ARM\n");
    CHECK(cpu_model_from_cpuinfo(hardware) == "Board");
    std::istringstream arm("model name\t: \nProcessor\t: ARM926\n");
    CHECK(cpu_model_from_cpuinfo(arm) == "ARM926");
    std::istringstream normal("processor\t: 0\nmodel name\t: Real CPU\n");
    CHECK(cpu_model_from_cpuinfo(normal) == "Real CPU (1 core, 1 thread)");
  }

  // Finding 7: missing MemTotal is unknown. MemTotal 0 with MemAvailable 0 is real.
  {
    std::istringstream missing(
        "MemFree:         100 kB\n"
        "Buffers:          10 kB\n"
        "Cached:           20 kB\n");
    const MemoryUsage unknown = memory_usage_from_meminfo(parse_meminfo(missing));
    CHECK(unknown.known == false);
    CHECK(unknown.total_kb == 0);
    CHECK(unknown.used_kb == 0);
    std::istringstream zero_total("MemTotal: 0 kB\nMemAvailable: 0 kB\n");
    const MemoryUsage zero = memory_usage_from_meminfo(parse_meminfo(zero_total));
    CHECK(zero.known);
    CHECK(zero.total_kb == 0);
    CHECK(zero.available_kb == 0);
    CHECK(zero.used_kb == 0);
    std::istringstream present(
        "MemTotal:       1000 kB\n"
        "MemAvailable:    100 kB\n");
    CHECK(memory_usage_from_meminfo(parse_meminfo(present)).known);
  }

  // Finding 9: do not clamp the saved pixel until the anchored row fits.
  {
    CHECK(scroll_anchor_ready(450, 100) == false);
    CHECK(scroll_anchor_ready(450, 450));
    CHECK(scroll_anchor_ready(450, 500));
    CHECK(clamp_scroll_value(400, 100, 80) == 20);
    CHECK(!restore_anchored_scroll(false, 300, 15, 450, 800, 200));
    CHECK(!restore_anchored_scroll(true, 300, 15, 450, 100, 80));
    const auto ready = restore_anchored_scroll(true, 300, 15, 360, 800, 200);
    CHECK(ready.has_value());
    if (ready) CHECK(*ready == 315);
    const auto clamped = restore_anchored_scroll(true, 700, 0, 760, 800, 200);
    CHECK(clamped.has_value());
    if (clamped) CHECK(*clamped == 600);
  }

  // Finding 10: Menu, the keypad menu key, and Shift+F10. Plain F10 does not.
  {
    CHECK(is_force_close_popup_key(0xff67u, 0));
    CHECK(is_force_close_popup_key(0x1008ff65u, 0));
    CHECK(is_force_close_popup_key(0xffc7u, 1u));
    CHECK(is_force_close_popup_key(0xffc7u, 0) == false);
    CHECK(is_force_close_popup_key(0xff0du, 0) == false);
    CHECK(is_row_activate_key(0xff0du, 0));
    CHECK(is_row_activate_key(0xff8du, 0));
    CHECK(is_row_activate_key(0xfe34u, 0));
    CHECK(is_row_activate_key(0xff0du, 1u));
    CHECK(is_row_activate_key(0xff0du, 4u) == false);
    CHECK(is_row_activate_key(0xff0du, 8u) == false);
    CHECK(is_row_activate_key(0xffc7u, 1u) == false);
    const std::string tip = row_tooltip_text("Document", true, "", "");
    CHECK(tip.find("Document") != std::string::npos);
    CHECK(tip.find("Right-click or press the Menu key or Shift+F10 to Force Close") !=
          std::string::npos);
    CHECK(tip.find("Shift+F10") != std::string::npos);
    const std::string shared = row_tooltip_text("Document", true, "", "pid 32");
    CHECK(shared.find("pid 32") != std::string::npos);
    CHECK(shared.find("Right-click or press the Menu key or Shift+F10 to Force Close") !=
          std::string::npos);
    const std::string blocked = row_tooltip_text("GhostWin", false, "No process ID", "pid 9");
    CHECK(blocked == "No process ID");
    CHECK(blocked.find("Force Close") == std::string::npos);
    const std::string panel = row_tooltip_text("xfce4-panel", false, "Desktop panel", "");
    CHECK(panel == "Desktop panel");
    const std::string system =
        row_tooltip_text("LCOS System\nUnclamped remainder: -5 kB", false, "LCOS System", "");
    CHECK(system == "LCOS System\nUnclamped remainder: -5 kB");
    CHECK(system.find("Force Close") == std::string::npos);
    CHECK(row_tooltip_text("LCOS System", false, "", "") == "Protected");
  }

  // Finding 12: shared names gain a title, or a pid when the title matches too.
  {
    WindowFact one = fact(1, 30, true, WindowKind::Normal);
    one.res_class = "My_App";
    one.title = "Alpha";
    WindowFact two = fact(2, 31, true, WindowKind::Normal);
    two.res_class = "My_App";
    two.title = "Beta";
    const auto distinct = group_windows({one, two});
    CHECK(distinct.size() == 2);
    if (distinct.size() == 2) {
      CHECK(distinct[0].name.find("Alpha") != std::string::npos);
      CHECK(distinct[1].name.find("Beta") != std::string::npos);
      CHECK(distinct[0].name.find("pid") == std::string::npos);
      CHECK(distinct[1].name.find("pid") == std::string::npos);
      CHECK(distinct[0].class_name == "My_App");
      CHECK(distinct[0].name != distinct[1].name);
      // Titles differ, but a long title is still ellipsized away. The pid
      // stays in a column that does not ellipsize.
      CHECK(distinct[0].distinguish == "pid 30");
      CHECK(distinct[1].distinguish == "pid 31");
      CHECK(painted_row_name(distinct[0].name, distinct[0].distinguish) == distinct[0].name);
    }
    WindowFact same_a = fact(3, 32, true, WindowKind::Normal);
    same_a.res_class = "My_App";
    same_a.title = "Same";
    WindowFact same_b = fact(4, 33, true, WindowKind::Normal);
    same_b.res_class = "My_App";
    same_b.title = "Same";
    const auto same = group_windows({same_a, same_b});
    CHECK(same.size() == 2);
    if (same.size() == 2) {
      CHECK(same[0].name.find("pid 32") != std::string::npos);
      CHECK(same[1].name.find("pid 33") != std::string::npos);
      CHECK(same[0].name != same[1].name);
      CHECK(same[0].distinguish == "pid 32");
      CHECK(same[1].distinguish == "pid 33");
      const std::string painted0 = painted_row_name(same[0].name, same[0].distinguish);
      const std::string painted1 = painted_row_name(same[1].name, same[1].distinguish);
      CHECK(painted0 == painted1);
      CHECK(painted0.find("pid") == std::string::npos);
      CHECK(painted0.find("My App") != std::string::npos);
    }
    WindowFact only = fact(5, 34, true, WindowKind::Normal);
    only.res_class = "Only";
    only.title = "Doc";
    const auto single = group_windows({only});
    CHECK(!single.empty());
    if (!single.empty()) {
      CHECK(single[0].name == "Only");
      CHECK(single[0].distinguish.empty());
    }
    const std::string long_class(80, 'A');
    WindowFact long_a = fact(11, 40, true, WindowKind::Normal);
    long_a.res_class = long_class;
    long_a.title = std::string(40, 'T') + "ONE";
    WindowFact long_b = fact(12, 41, true, WindowKind::Normal);
    long_b.res_class = long_class;
    long_b.title = std::string(40, 'T') + "TWO";
    const auto longs = group_windows({long_a, long_b});
    CHECK(longs.size() == 2);
    if (longs.size() == 2) {
      CHECK(longs[0].distinguish == "pid 40");
      CHECK(longs[1].distinguish == "pid 41");
      CHECK(longs[0].distinguish != longs[1].distinguish);
      CHECK(longs[0].name.find("ONE") != std::string::npos);
      CHECK(longs[1].name.find("TWO") != std::string::npos);
    }
    WindowFact ghost_a = fact(21, 0, false, WindowKind::Normal);
    ghost_a.res_class = "Ghost";
    ghost_a.title = "Same";
    WindowFact ghost_b = fact(22, 0, false, WindowKind::Normal);
    ghost_b.res_class = "Ghost";
    ghost_b.title = "Same";
    const auto ghosts = group_windows({ghost_a, ghost_b});
    CHECK(ghosts.size() == 2);
    if (ghosts.size() == 2) {
      CHECK(ghosts[0].distinguish == "window 21");
      CHECK(ghosts[1].distinguish == "window 22");
      CHECK(painted_row_name(ghosts[0].name, ghosts[0].distinguish).find("window") ==
            std::string::npos);
      CHECK(ghosts[0].protect_reason == "No process ID");
    }
  }

  // Finding 13: the question uses the row name. The body is plain language.
  {
    const ForceClosePrompt prompt = force_close_prompt("GoodApp", "testwin", 10941, "GoodApp", true);
    CHECK(prompt.primary == "Force Close \"GoodApp\"?");
    CHECK(prompt.secondary.find("SIGKILL") == std::string::npos);
    CHECK(prompt.secondary.find("quit GoodApp immediately") != std::string::npos);
    CHECK(prompt.secondary.find("command testwin") != std::string::npos);
    CHECK(prompt.secondary.find("PID 10941") != std::string::npos);
    CHECK(prompt.secondary.find("helper processes counted in this row") != std::string::npos);
    CHECK(prompt.secondary.find("Unsaved work will be lost.") != std::string::npos);
    CHECK(prompt.secondary.find("does not match") != std::string::npos);
    CHECK(prompt.secondary.find("already gone") == std::string::npos);
    const ForceClosePrompt matched =
        force_close_prompt("Firefox", "firefox", 10, "Firefox", false);
    CHECK(matched.secondary.find("does not match") == std::string::npos);
    CHECK(matched.secondary.find("already gone") != std::string::npos);
    CHECK(matched.primary.find("Firefox") != std::string::npos);
  }

  // The GPU fallback names the server that owns DISPLAY, not every Xvfb.
  {
    CHECK(!local_x_display_number(""));
    CHECK(!local_x_display_number("1"));
    CHECK(!local_x_display_number(":abc"));
    CHECK(!local_x_display_number("otherhost:1"));
    const auto d0 = local_x_display_number(":0");
    const auto d1 = local_x_display_number(":1.0");
    const auto d22 = local_x_display_number("unix:22");
    const auto d_local = local_x_display_number("localhost:1.0");
    const auto d_loop = local_x_display_number("127.0.0.1:0");
    CHECK(d0 && *d0 == 0);
    CHECK(d1 && *d1 == 1);
    CHECK(d22 && *d22 == 22);
    CHECK(d_local && *d_local == 1);
    CHECK(d_loop && *d_loop == 0);

    std::istringstream table(
        "Num       RefCount Protocol Flags    Type St Inode Path\n"
        "0000000083a54465: 00000002 00000000 00010000 0001 01  5528 /tmp/.X11-unix/X1\n"
        "00000000f707763a: 00000002 00000000 00010000 0001 01  5527 @/tmp/.X11-unix/X1\n"
        "000000004ffd1ddf: 00000003 00000000 00000000 0001 03  4451 @/tmp/.X11-unix/X1\n"
        "0000000000000001: 00000002 00000000 00010000 0001 01  9999 /tmp/.X11-unix/X22\n"
        "0000000000000002: 00000002 00000000 00010000 0001 01  10001 /tmp/.X11-unix/X10\n");
    const auto sockets = parse_proc_net_unix(table);
    CHECK(sockets.size() == 4);
    bool saw_client = false;
    for (const auto& sock : sockets) {
      if (sock.inode == 4451) saw_client = true;
    }
    CHECK(!saw_client);
    CHECK(x_socket_path_matches_display("/tmp/.X11-unix/X1", 1));
    CHECK(x_socket_path_matches_display("@/tmp/.X11-unix/X1", 1));
    CHECK(!x_socket_path_matches_display("/tmp/.X11-unix/X10", 1));
    CHECK(!x_socket_path_matches_display("/tmp/.X11-unix/X1", 10));

    const std::vector<ProcessSocket> procs = {
        {1600, 5528, "Xtigervnc"},
        {1600, 5527, "Xtigervnc"},
        {50, 4451, "some-client"},
        {6606, 9999, "Xvfb"},
        {70, 10001, "Xvfb"},
    };
    CHECK(server_comm_for_display(1, sockets, procs) == "Xtigervnc");
    CHECK(server_comm_for_display(22, sockets, procs) == "Xvfb");
    CHECK(server_comm_for_display(10, sockets, procs) == "Xvfb");
    CHECK(server_comm_for_display(3, sockets, procs).empty());
    CHECK(virtual_display_label_for_server(server_comm_for_display(1, sockets, procs)) ==
          "Virtual display");
    CHECK(virtual_display_label_for_server("Xvfb") == "Virtual framebuffer (Xvfb)");
    CHECK(virtual_display_label_for_server("Xvnc") == "Virtual display");
    CHECK(virtual_display_label_for_server("Xephyr") == "Virtual display");
    CHECK(virtual_display_label_for_server("Xorg") == "Unknown GPU");
    CHECK(virtual_display_label_for_server("") == "Unknown GPU");
    CHECK(virtual_display_label_for_server("notXvfb") == "Unknown GPU");
    // Display 1 stays "Virtual display" while an Xvfb listens on :22 and :10.
    CHECK(virtual_display_label_for_server(server_comm_for_display(1, sockets, procs))
              .find("Xvfb") == std::string::npos);
  }

  // A real Xvfb on some other display must not become the server for DISPLAY.
  {
    int pipefd[2] = {-1, -1};
    bool started = false;
    pid_t child = -1;
    int xvfb_display = -1;
    if (pipe(pipefd) == 0) {
      child = fork();
      if (child == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], 3) < 0) _exit(127);
        if (pipefd[1] != 3) close(pipefd[1]);
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
          dup2(devnull, STDOUT_FILENO);
          dup2(devnull, STDERR_FILENO);
          if (devnull > 2 && devnull != 3) close(devnull);
        }
        execlp("Xvfb", "Xvfb", "-displayfd", "3", "-nolisten", "tcp", "-ac", "-screen", "0",
               "640x480x8", static_cast<char*>(nullptr));
        _exit(127);
      }
      close(pipefd[1]);
      pipefd[1] = -1;
      if (child > 0) {
        std::string acc;
        const auto deadline = []() {
          timeval tv;
          tv.tv_sec = 3;
          tv.tv_usec = 0;
          return tv;
        };
        while (acc.find('\n') == std::string::npos) {
          fd_set rfds;
          FD_ZERO(&rfds);
          FD_SET(pipefd[0], &rfds);
          timeval tv = deadline();
          const int sel = select(pipefd[0] + 1, &rfds, nullptr, nullptr, &tv);
          if (sel <= 0) break;
          char buf[64];
          const ssize_t n = read(pipefd[0], buf, sizeof(buf));
          if (n <= 0) break;
          acc.append(buf, buf + n);
        }
        if (!acc.empty()) {
          try {
            xvfb_display = std::stoi(acc);
            started = xvfb_display >= 0;
          } catch (...) {
            started = false;
          }
        }
      }
    }
    if (pipefd[0] >= 0) close(pipefd[0]);
    if (!started) {
      if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, nullptr, 0);
      }
      std::cout << "SKIP live display server (Xvfb did not start)\n";
    } else {
      std::string comm;
      const std::string spec = ":" + std::to_string(xvfb_display);
      for (int i = 0; i < 20 && comm != "Xvfb"; ++i) {
        comm = server_comm_owning_display(spec);
        if (comm == "Xvfb") break;
        usleep(50000);
      }
      CHECK(comm == "Xvfb");
      CHECK(virtual_display_label_for_server(comm) == "Virtual framebuffer (Xvfb)");
      // unix/:N and unix/host:N name that same server.
      const std::string unix_slash = "unix/:" + std::to_string(xvfb_display);
      const std::string unix_host = "unix/localhost:" + std::to_string(xvfb_display);
      CHECK(server_comm_owning_display(unix_slash) == "Xvfb");
      CHECK(server_comm_owning_display(unix_host) == "Xvfb");
      CHECK(server_comm_owning_display("unix/otherhost:" + std::to_string(xvfb_display)) == "Xvfb");
      // Local TCP cannot use SO_PEERCRED. The proc walk is the fallback.
      CHECK(server_comm_owning_display("tcp/127.0.0.1:" + std::to_string(xvfb_display)) == "Xvfb");
      CHECK(server_comm_owning_display("tcp/[::1]:" + std::to_string(xvfb_display)) == "Xvfb");
      // A remote spec with the same display number is not this server.
      CHECK(server_comm_owning_display("tcp/example.invalid:" + std::to_string(xvfb_display)).empty());
      CHECK(gpu_label_for_x_connection(-1, "tcp/example.invalid:" + std::to_string(xvfb_display)) ==
            "Remote display");

      const std::string colon_spec = ":" + std::to_string(xvfb_display);
      Display* dpy = XOpenDisplay(colon_spec.c_str());
      CHECK(dpy != nullptr);
      if (dpy) {
        const int fd = XConnectionNumber(dpy);
        const XPeerCred peer = x_peer_cred_from_fd(fd);
        CHECK(peer.supported);
        CHECK(peer.have_pid);
        CHECK(peer.pid == child);
        CHECK(peer.uid == getuid());
        CHECK(peer.comm == "Xvfb");
        CHECK(comm_of_pid(peer.pid) == "Xvfb");
        // The live connection wins even when the spec names another machine.
        CHECK(gpu_label_for_x_connection(fd, "tcp/example.invalid:1") ==
              "Virtual framebuffer (Xvfb)");
        // A non-socket fd does not pretend to be that server.
        int pipes[2] = {-1, -1};
        CHECK(pipe(pipes) == 0);
        if (pipes[0] >= 0) {
          CHECK(gpu_label_for_x_connection(pipes[0], unix_slash) == "Virtual framebuffer (Xvfb)");
          CHECK(gpu_label_for_x_connection(pipes[0], "otherhost:" + std::to_string(xvfb_display)) ==
                "Remote display");
          close(pipes[0]);
          close(pipes[1]);
        }
        XCloseDisplay(dpy);
      }
      // Do not walk a run of displays with XOpenDisplay. A stale server
      // that accepts and never speaks used to hang this loop. Only a
      // display with no unix socket is probed, and that probe is timed.
      bool saw_other = false;
      for (int extra = xvfb_display + 1; extra < xvfb_display + 8 && !saw_other; ++extra) {
        const std::string path = "/tmp/.X11-unix/X" + std::to_string(extra);
        if (access(path.c_str(), F_OK) == 0) continue;
        const auto t0 = std::chrono::steady_clock::now();
        const std::string other = server_comm_owning_display(":" + std::to_string(extra));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        CHECK(ms < 2000);
        if (other.empty()) saw_other = true;
      }
      CHECK(saw_other);
      if (const char* cur = std::getenv("DISPLAY")) {
        const auto cur_n = local_x_display_number(cur);
        if (cur_n && *cur_n != xvfb_display) {
          const std::string mine = server_comm_owning_display(cur);
          // The session server may itself be Xvfb (xvfb-run). A different
          // server must not be labeled as the background Xvfb.
          if (!mine.empty() && mine != "Xvfb") {
            CHECK(virtual_display_label_for_server(mine) != "Virtual framebuffer (Xvfb)");
          }
        }
      }
      kill(child, SIGTERM);
      waitpid(child, nullptr, 0);
      child = -1;
    }
  }

  // DISPLAY parser: [protocol/]host:display[.screen], including IPv6.
  {
    struct Row {
      const char* spec;
      bool ok;
      int display;
      int screen;
      bool has_screen;
      XDisplayTransport transport;
    };
    const Row rows[] = {
        {"", false, -1, 0, false, XDisplayTransport::Invalid},
        {"1", false, -1, 0, false, XDisplayTransport::Invalid},
        {":abc", false, -1, 0, false, XDisplayTransport::Invalid},
        {":1.", false, -1, 0, false, XDisplayTransport::Invalid},
        {":0", true, 0, 0, false, XDisplayTransport::LocalUnix},
        {":1.0", true, 1, 0, true, XDisplayTransport::LocalUnix},
        {":12.3", true, 12, 3, true, XDisplayTransport::LocalUnix},
        {"unix:22", true, 22, 0, false, XDisplayTransport::LocalUnix},
        {"unix:1.2", true, 1, 2, true, XDisplayTransport::LocalUnix},
        {"unix/:1", true, 1, 0, false, XDisplayTransport::LocalUnix},
        {"unix/:1.0", true, 1, 0, true, XDisplayTransport::LocalUnix},
        {"unix/localhost:1", true, 1, 0, false, XDisplayTransport::LocalUnix},
        {"unix/127.0.0.1:1", true, 1, 0, false, XDisplayTransport::LocalUnix},
        {"unix/otherhost:2", true, 2, 0, false, XDisplayTransport::LocalUnix},
        {"unix/[::1]:3", true, 3, 0, false, XDisplayTransport::LocalUnix},
        {"localhost:1.0", true, 1, 0, true, XDisplayTransport::LocalUnix},
        {"127.0.0.1:0", true, 0, 0, false, XDisplayTransport::LocalUnix},
        {"127.1.2.3:4", true, 4, 0, false, XDisplayTransport::LocalUnix},
        {"[::1]:0", true, 0, 0, false, XDisplayTransport::LocalTcp},
        {"[::1]:1.2", true, 1, 2, true, XDisplayTransport::LocalTcp},
        {"tcp/localhost:1", true, 1, 0, false, XDisplayTransport::LocalTcp},
        {"tcp/127.0.0.1:0.1", true, 0, 1, true, XDisplayTransport::LocalTcp},
        {"tcp/[::1]:2", true, 2, 0, false, XDisplayTransport::LocalTcp},
        {"TCP/localhost:8", true, 8, 0, false, XDisplayTransport::LocalTcp},
        {"inet/localhost:3", true, 3, 0, false, XDisplayTransport::LocalTcp},
        {"inet6/[::1]:4", true, 4, 0, false, XDisplayTransport::LocalTcp},
        {"tcp/example.invalid:1", true, 1, 0, false, XDisplayTransport::Remote},
        {"tcp/[2001:db8::1]:5", true, 5, 0, false, XDisplayTransport::Remote},
        {"otherhost:1", true, 1, 0, false, XDisplayTransport::Remote},
        {"otherhost:1.0", true, 1, 0, true, XDisplayTransport::Remote},
    };
    for (const Row& row : rows) {
      const XDisplayParsed parsed = parse_x_display(row.spec);
      CHECK(parsed.ok == row.ok);
      if (!row.ok) {
        CHECK(!local_x_display_number(row.spec));
        continue;
      }
      CHECK(parsed.display == row.display);
      CHECK(parsed.screen == row.screen);
      CHECK(parsed.has_screen == row.has_screen);
      CHECK(parsed.transport == row.transport);
      const auto number = local_x_display_number(row.spec);
      if (row.transport == XDisplayTransport::Remote) {
        CHECK(!number);
      } else {
        CHECK(number && *number == row.display);
      }
    }
    CHECK(gpu_label_for_x_connection(-1, "otherhost:1") == "Remote display");
    CHECK(gpu_label_for_x_connection(-1, "tcp/example.invalid:9") == "Remote display");
  }

  // Root-owned processes: comm is readable when /proc/<pid>/fd is not.
  {
    const std::string self = comm_of_pid(getpid());
    CHECK(!self.empty());
    std::ifstream comm_file("/proc/1/comm");
    std::string init_comm;
    const bool comm_readable = static_cast<bool>(std::getline(comm_file, init_comm));
    if (!init_comm.empty() && (init_comm.back() == '\n' || init_comm.back() == '\r')) {
      init_comm.pop_back();
    }
    DIR* fd_dir = opendir("/proc/1/fd");
    const bool fd_readable = fd_dir != nullptr;
    if (fd_dir) closedir(fd_dir);
    if (comm_readable && !fd_readable) {
      CHECK(comm_of_pid(1) == init_comm);
      CHECK(!comm_of_pid(1).empty());
    }
    CHECK(comm_of_pid(-1).empty());
    CHECK(comm_of_pid(0).empty());
  }

  // A root-owned Xvfb is named from SO_PEERCRED plus /proc/<pid>/comm.
  {
    int display = -1;
    for (int n = 40; n < 80; ++n) {
      const std::string path = "/tmp/.X11-unix/X" + std::to_string(n);
      if (access(path.c_str(), F_OK) != 0) {
        display = n;
        break;
      }
    }
    CHECK(display >= 0);
    pid_t child = -1;
    if (display >= 0) {
      child = fork();
      if (child == 0) {
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
          dup2(devnull, STDOUT_FILENO);
          dup2(devnull, STDERR_FILENO);
          if (devnull > 2) close(devnull);
        }
        const std::string spec = ":" + std::to_string(display);
        execlp("sudo", "sudo", "-n", "Xvfb", spec.c_str(), "-screen", "0", "640x480x24",
               "-nolisten", "tcp", "-ac", static_cast<char*>(nullptr));
        _exit(127);
      }
    }
    bool started = false;
    if (child > 0) {
      const std::string path = "/tmp/.X11-unix/X" + std::to_string(display);
      for (int i = 0; i < 40; ++i) {
        if (access(path.c_str(), F_OK) == 0) {
          started = true;
          break;
        }
        usleep(50000);
      }
    }
    auto stop_root = [&]() {
      if (display >= 0) {
        const std::string spec = ":" + std::to_string(display);
        Display* dpy = XOpenDisplay(spec.c_str());
        pid_t peer = -1;
        if (dpy) {
          peer = x_peer_cred_from_fd(XConnectionNumber(dpy)).pid;
          XCloseDisplay(dpy);
        }
        if (peer > 0) {
          const pid_t killer = fork();
          if (killer == 0) {
            const std::string pid_text = std::to_string(peer);
            execlp("sudo", "sudo", "-n", "kill", "-TERM", pid_text.c_str(),
                   static_cast<char*>(nullptr));
            _exit(127);
          }
          if (killer > 0) waitpid(killer, nullptr, 0);
        }
      }
      if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, nullptr, 0);
        child = -1;
      }
    };
    if (!started) {
      stop_root();
      std::cout << "SKIP root Xvfb (could not start)\n";
    } else {
      const std::string spec = ":" + std::to_string(display);
      Display* dpy = nullptr;
      for (int i = 0; i < 20 && !dpy; ++i) {
        dpy = XOpenDisplay(spec.c_str());
        if (!dpy) usleep(50000);
      }
      CHECK(dpy != nullptr);
      if (dpy) {
        const XPeerCred peer = x_peer_cred_from_fd(XConnectionNumber(dpy));
        XCloseDisplay(dpy);
        CHECK(peer.supported);
        CHECK(peer.have_pid);
        CHECK(peer.uid == 0);
        CHECK(peer.comm == "Xvfb");
        CHECK(server_comm_owning_display(spec) == "Xvfb");
        CHECK(server_comm_owning_display("unix/:" + std::to_string(display)) == "Xvfb");
        CHECK(virtual_display_label_for_server(peer.comm) == "Virtual framebuffer (Xvfb)");
        DIR* fd_dir = opendir(("/proc/" + std::to_string(peer.pid) + "/fd").c_str());
        const bool fd_readable = fd_dir != nullptr;
        if (fd_dir) closedir(fd_dir);
        CHECK(!fd_readable);
        CHECK(comm_of_pid(peer.pid) == "Xvfb");
      }
      stop_root();
    }
  }

  // Hybrid GPUs: both adapters, display controller first. One card keeps one name.
  {
    std::istringstream ids(
        "8086  Intel Corporation\n"
        "\t9a49  TigerLake-LP GT2 [Iris Xe Graphics]\n"
        "1002  Advanced Micro Devices, Inc. [AMD/ATI]\n"
        "\t67df  Ellesmere [Radeon RX 470/480/570/570X/580/580X/590]\n"
        "\t73bf  Navi 21 [Radeon RX 6800/6800 XT / 6900 XT]\n");
    const PciDb db = parse_pci_ids(ids);
    const std::vector<GpuDevice> same_card{
        GpuDevice{"0000:01:00.0", 0x030000, 0x1002, 0x73bf, "amdgpu"},
        GpuDevice{"0000:01:00.1", 0x030200, 0x1002, 0x73bf, "amdgpu"},
    };
    const std::string collapsed = gpu_label_from_devices(same_card, db);
    CHECK(collapsed.find("Navi") != std::string::npos);
    CHECK(collapsed.find(';') == std::string::npos);

    const std::vector<GpuDevice> two_3d{
        GpuDevice{"0000:02:00.0", 0x030200, 0x1002, 0x67df, "amdgpu"},
        GpuDevice{"0000:03:00.0", 0x030200, 0x1002, 0x73bf, "amdgpu"},
    };
    const std::string both_3d = gpu_label_from_devices(two_3d, db);
    CHECK(both_3d.find("Ellesmere") != std::string::npos);
    CHECK(both_3d.find("Navi") != std::string::npos);
    CHECK(both_3d.find("Ellesmere") < both_3d.find("Navi"));

    char tmpl[] = "/tmp/lcos-pci-XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr);
    if (dir) {
      const std::string root(dir);
      auto write_dev = [&](const std::string& slot, const char* cls, const char* vendor,
                           const char* device, const char* driver) {
        const std::string base = root + "/" + slot;
        CHECK(mkdir(base.c_str(), 0755) == 0);
        auto put = [&](const char* name, const char* text) {
          std::ofstream out(base + "/" + name);
          out << text << "\n";
          CHECK(static_cast<bool>(out));
        };
        put("class", cls);
        put("vendor", vendor);
        put("device", device);
        std::ofstream uevent(base + "/uevent");
        uevent << "DRIVER=" << driver << "\nPCI_ID=" << vendor << ":" << device << "\n";
        CHECK(static_cast<bool>(uevent));
      };
      write_dev("0000:00:02.0", "0x030000", "0x8086", "0x9a49", "i915");
      write_dev("0000:01:00.0", "0x030200", "0x1002", "0x67df", "amdgpu");
      write_dev("0000:00:1f.3", "0x040300", "0x8086", "0x0001", "snd_hda_intel");
      const std::vector<GpuDevice> from_sys = gpu_devices_from_sysfs(root);
      CHECK(from_sys.size() == 2);
      const std::string label = gpu_label_from_devices(from_sys, db);
      CHECK(label.find("Iris") != std::string::npos);
      CHECK(label.find("Ellesmere") != std::string::npos);
      CHECK(label.find("Iris") < label.find("Ellesmere"));
      CHECK(label.find("snd") == std::string::npos);
      CHECK(label.find("0001") == std::string::npos);
      for (const char* slot : {"0000:00:02.0", "0000:01:00.0", "0000:00:1f.3"}) {
        const std::string base = root + "/" + slot;
        unlink((base + "/class").c_str());
        unlink((base + "/vendor").c_str());
        unlink((base + "/device").c_str());
        unlink((base + "/uevent").c_str());
        rmdir(base.c_str());
      }
      rmdir(root.c_str());
    }
  }

  // CPU line: core and thread counts, including mixed model names.
  {
    std::ostringstream xeon;
    for (int i = 0; i < 4; ++i) {
      xeon << "processor\t: " << i << "\n"
           << "model name\t: Intel(R) Xeon(R) Processor\n"
           << "physical id\t: 0\n"
           << "core id\t: " << i << "\n"
           << "cpu cores\t: 4\n"
           << "siblings\t: 4\n\n";
    }
    std::istringstream xeon_in(xeon.str());
    CHECK(cpu_model_from_cpuinfo(xeon_in) ==
          "Intel(R) Xeon(R) Processor (4 cores, 4 threads)");

    std::ostringstream ht;
    for (int i = 0; i < 4; ++i) {
      ht << "processor\t: " << i << "\n"
         << "model name\t: HT CPU\n"
         << "physical id\t: 0\n"
         << "cpu cores\t: 2\n"
         << "siblings\t: 4\n\n";
    }
    std::istringstream ht_in(ht.str());
    CHECK(cpu_model_from_cpuinfo(ht_in) == "HT CPU (2 cores, 4 threads)");

    std::ostringstream ht_ids;
    for (int i = 0; i < 4; ++i) {
      ht_ids << "processor\t: " << i << "\n"
             << "model name\t: HT CPU\n"
             << "physical id\t: 0\n"
             << "core id\t: " << (i % 2) << "\n\n";
    }
    std::istringstream ht_ids_in(ht_ids.str());
    CHECK(cpu_model_from_cpuinfo(ht_ids_in) == "HT CPU (2 cores, 4 threads)");

    std::ostringstream mixed;
    mixed << "processor\t: 0\nmodel name\t: Cortex-A78\nphysical id\t: 0\ncore id\t: 0\n\n"
          << "processor\t: 1\nmodel name\t: Cortex-A78\nphysical id\t: 0\ncore id\t: 1\n\n"
          << "processor\t: 2\nmodel name\t: Cortex-A55\nphysical id\t: 1\ncore id\t: 0\n\n"
          << "processor\t: 3\nmodel name\t: Cortex-A55\nphysical id\t: 1\ncore id\t: 1\n\n"
          << "processor\t: 4\nmodel name\t: Cortex-A55\nphysical id\t: 1\ncore id\t: 2\n\n"
          << "processor\t: 5\nmodel name\t: Cortex-A55\nphysical id\t: 1\ncore id\t: 3\n\n";
    std::istringstream mixed_in(mixed.str());
    CHECK(cpu_model_from_cpuinfo(mixed_in) ==
          "Cortex-A78 (2 cores, 2 threads); Cortex-A55 (4 cores, 4 threads)");

    std::ostringstream many;
    for (int i = 0; i < 128; ++i) {
      many << "processor\t: " << i << "\n"
           << "model name\t: Same CPU\n"
           << "physical id\t: " << (i / 64) << "\n"
           << "core id\t: " << (i % 64) << "\n\n";
    }
    std::istringstream many_in(many.str());
    const std::string many_line = cpu_model_from_cpuinfo(many_in);
    CHECK(many_line == "Same CPU (128 cores, 128 threads)");
    CHECK(many_line != "Same CPU");

    std::istringstream blank_block(
        "processor\t: 0\nmodel name\t:\n\nprocessor\t: 1\nmodel name\t: Kept\ncore id\t: 0\n\n");
    CHECK(cpu_model_from_cpuinfo(blank_block) == "Kept (1 core, 1 thread)");
  }

  // Loopback TCP: the listening port in /proc/net/tcp names the server.
  // A readable fd inode wins. A root server whose fd directory cannot be
  // read is named from the socket uid and comm.
  {
    std::istringstream tcp(
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  "
        "timeout inode\n"
        "   0: 0100007F:178F 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        "
        "0 4242 1 0000000000000000 100 0 0 10 0\n"
        "   1: 00000000:0050 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        "
        "0 9999 1 0000000000000000 100 0 0 10 0\n"
        "   2: 0100007F:178F 0100007F:1234 01 00000000:00000000 00:00000000 00000000  1000        "
        "0 7777 1 0000000000000000 100 0 0 10 0\n");
    const auto rows = parse_proc_net_tcp(tcp);
    const TcpListenEntry* listen = nullptr;
    bool saw_established = false;
    bool saw_http = false;
    for (const auto& row : rows) {
      if (row.port == 80 && row.listening) saw_http = true;
      if (row.port == 6031 && row.listening) listen = &row;
      if (row.port == 6031 && !row.listening) saw_established = true;
    }
    CHECK(listen != nullptr);
    CHECK(saw_http);
    CHECK(saw_established);
    if (listen) {
      CHECK(listen->inode == 4242);
      CHECK(listen->uid == 0);
    }
    std::istringstream tcp6(
        "  sl  local_address                         remote_address                        st "
        "tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
        "   0: 00000000000000000000000001000000:178F 00000000000000000000000000000000:0000 0A "
        "00000000:00000000 00:00000000 00000000  1000        0 5151 1 0000000000000000 100 0 0 10 0\n");
    const auto v6 = parse_proc_net_tcp(tcp6);
    CHECK(v6.size() == 1);
    if (!v6.empty()) {
      CHECK(v6[0].listening);
      CHECK(v6[0].port == 6031);
      CHECK(v6[0].inode == 5151);
      CHECK(v6[0].uid == 1000);
    }

    TcpListenEntry owner;
    owner.port = 6031;
    owner.inode = 4242;
    owner.uid = 0;
    owner.listening = true;
    ProcFdRecord holder;
    holder.pid = 50;
    holder.uid = 1000;
    holder.comm = "Xvfb";
    holder.fd_dir_readable = true;
    holder.socket_inodes = {4242};
    ProcFdRecord other;
    other.pid = 1;
    other.uid = 0;
    other.comm = "systemd";
    other.fd_dir_readable = false;
    CHECK(comm_owning_tcp_listeners({owner}, {holder, other}) == "Xvfb");

    ProcFdRecord root_xvfb;
    root_xvfb.pid = 100;
    root_xvfb.uid = 0;
    root_xvfb.comm = "Xvfb";
    root_xvfb.fd_dir_readable = false;
    root_xvfb.cmdline_matches_display = true;
    ProcFdRecord root_sudo = root_xvfb;
    root_sudo.pid = 90;
    root_sudo.comm = "sudo";
    ProcFdRecord root_init = other;
    CHECK(comm_owning_tcp_listeners({owner}, {root_sudo, root_xvfb, root_init}) == "Xvfb");

    ProcFdRecord plain_xvfb = root_xvfb;
    plain_xvfb.cmdline_matches_display = false;
    ProcFdRecord plain_xorg = plain_xvfb;
    plain_xorg.pid = 110;
    plain_xorg.comm = "Xorg";
    CHECK(comm_owning_tcp_listeners({owner}, {plain_xvfb, plain_xorg, root_init}).empty());

    ProcFdRecord marked = plain_xvfb;
    marked.pid = 120;
    marked.cmdline_matches_display = true;
    CHECK(comm_owning_tcp_listeners({owner}, {plain_xorg, marked, root_sudo}) == "Xvfb");

    CHECK(comm_owning_tcp_listeners({owner}, {plain_xvfb, root_init}) == "Xvfb");
    owner.listening = false;
    CHECK(comm_owning_tcp_listeners({owner}, {holder}).empty());
  }

  // A socket that accepts and never speaks must not hang the display probe.
  {
    int display = -1;
    for (int n = 120; n < 150; ++n) {
      const std::string path = "/tmp/.X11-unix/X" + std::to_string(n);
      if (access(path.c_str(), F_OK) == 0) continue;
      const int probe = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (probe < 0) continue;
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(static_cast<uint16_t>(6000 + n));
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      const int bound = bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
      close(probe);
      if (bound != 0) continue;
      display = n;
      break;
    }
    CHECK(display >= 0);
    if (display >= 0) {
      const std::string path = "/tmp/.X11-unix/X" + std::to_string(display);
      const int unix_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      sockaddr_un un{};
      un.sun_family = AF_UNIX;
      CHECK(path.size() + 1 < sizeof(un.sun_path));
      std::memcpy(un.sun_path, path.c_str(), path.size() + 1);
      const bool unix_ok = unix_fd >= 0 &&
                           bind(unix_fd, reinterpret_cast<sockaddr*>(&un), sizeof(un)) == 0 &&
                           listen(unix_fd, 4) == 0;
      CHECK(unix_ok);
      if (unix_ok) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::string comm = server_comm_owning_display(":" + std::to_string(display));
        const long ms = elapsed_ms(t0);
        if (ms >= 2000) std::cerr << "silent unix display took " << ms << "ms\n";
        CHECK(ms < 2000);
        (void)comm;
      }
      if (unix_fd >= 0) close(unix_fd);
      unlink(path.c_str());

      const int tcp_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(static_cast<uint16_t>(6000 + display));
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      const bool tcp_ok = tcp_fd >= 0 &&
                          bind(tcp_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                          listen(tcp_fd, 4) == 0;
      CHECK(tcp_ok);
      if (tcp_ok) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::string comm =
            server_comm_owning_display("127.0.0.1:" + std::to_string(display));
        const long ms = elapsed_ms(t0);
        if (ms >= 2000) std::cerr << "silent tcp display took " << ms << "ms\n";
        CHECK(ms < 2000);
        (void)comm;
      }
      if (tcp_fd >= 0) close(tcp_fd);
    }
  }

  // Live Xvfb listening on TCP only. SO_PEERCRED does not name it.
  {
    int pipefd[2] = {-1, -1};
    pid_t child = -1;
    int xvfb_display = -1;
    bool started = false;
    if (pipe(pipefd) == 0) {
      child = fork();
      if (child == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], 3) < 0) _exit(127);
        if (pipefd[1] != 3) close(pipefd[1]);
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
          dup2(devnull, STDOUT_FILENO);
          dup2(devnull, STDERR_FILENO);
          if (devnull > 2 && devnull != 3) close(devnull);
        }
        execlp("Xvfb", "Xvfb", "-displayfd", "3", "-nolisten", "unix", "-listen", "tcp", "-ac",
               "-screen", "0", "640x480x8", static_cast<char*>(nullptr));
        _exit(127);
      }
      close(pipefd[1]);
      if (child > 0) {
        std::string acc;
        while (acc.find('\n') == std::string::npos) {
          fd_set rfds;
          FD_ZERO(&rfds);
          FD_SET(pipefd[0], &rfds);
          timeval tv{};
          tv.tv_sec = 3;
          const int sel = select(pipefd[0] + 1, &rfds, nullptr, nullptr, &tv);
          if (sel <= 0) break;
          char buf[64];
          const ssize_t n = read(pipefd[0], buf, sizeof(buf));
          if (n <= 0) break;
          acc.append(buf, buf + n);
        }
        if (!acc.empty()) {
          try {
            xvfb_display = std::stoi(acc);
            started = xvfb_display >= 0;
          } catch (...) {
            started = false;
          }
        }
      }
    }
    if (pipefd[0] >= 0) close(pipefd[0]);
    if (started) {
      bool up = false;
      for (int i = 0; i < 40 && !up; ++i) {
        up = tcp_port_accepts(6000 + xvfb_display);
        if (!up) usleep(50000);
      }
      started = up;
    }
    auto stop = [&]() {
      if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, nullptr, 0);
        child = -1;
      }
    };
    if (!started) {
      stop();
      std::cout << "SKIP live TCP Xvfb (did not start)\n";
    } else {
      const std::string n = std::to_string(xvfb_display);
      const std::string specs[] = {
          "127.0.0.1:" + n,
          "localhost:" + n,
          "[::1]:" + n,
          "tcp/127.0.0.1:" + n,
          "tcp/[::1]:" + n,
          "tcp/localhost:" + n,
      };
      for (const std::string& spec : specs) {
        std::string comm;
        for (int i = 0; i < 20 && comm != "Xvfb"; ++i) {
          comm = server_comm_owning_display(spec);
          if (comm == "Xvfb") break;
          usleep(50000);
        }
        CHECK(comm == "Xvfb");
        CHECK(virtual_display_label_for_server(comm) == "Virtual framebuffer (Xvfb)");
      }
      const char* opens[] = {"127.0.0.1:", "localhost:", "[::1]:"};
      for (const char* prefix : opens) {
        const std::string spec = std::string(prefix) + n;
        Display* dpy = nullptr;
        for (int i = 0; i < 20 && !dpy; ++i) {
          dpy = XOpenDisplay(spec.c_str());
          if (!dpy) usleep(50000);
        }
        CHECK(dpy != nullptr);
        if (!dpy) continue;
        const int fd = XConnectionNumber(dpy);
        const XPeerCred peer = x_peer_cred_from_fd(fd);
        CHECK(peer.have_pid == false);
        CHECK(peer.supported == false);
        CHECK(gpu_label_for_x_connection(fd, spec) == "Virtual framebuffer (Xvfb)");
        CHECK(gpu_label_for_x_connection(fd, spec) != "Unknown GPU");
        XCloseDisplay(dpy);
      }
      stop();
    }
  }

  // Root-owned TCP Xvfb: /proc/<pid>/fd is unreadable, the port uid and comm are not.
  {
    int display = -1;
    for (int n = 150; n < 180; ++n) {
      const std::string path = "/tmp/.X11-unix/X" + std::to_string(n);
      if (access(path.c_str(), F_OK) == 0) continue;
      const int probe = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (probe < 0) continue;
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(static_cast<uint16_t>(6000 + n));
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      const int bound = bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
      close(probe);
      if (bound != 0) continue;
      display = n;
      break;
    }
    CHECK(display >= 0);
    pid_t child = -1;
    if (display >= 0) {
      child = fork();
      if (child == 0) {
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
          dup2(devnull, STDOUT_FILENO);
          dup2(devnull, STDERR_FILENO);
          if (devnull > 2) close(devnull);
        }
        const std::string spec = ":" + std::to_string(display);
        execlp("sudo", "sudo", "-n", "Xvfb", spec.c_str(), "-screen", "0", "640x480x8",
               "-nolisten", "unix", "-listen", "tcp", "-ac", static_cast<char*>(nullptr));
        _exit(127);
      }
    }
    bool started = false;
    if (child > 0 && display >= 0) {
      for (int i = 0; i < 40; ++i) {
        if (tcp_port_accepts(6000 + display)) {
          started = true;
          break;
        }
        usleep(50000);
      }
    }
    auto stop = [&]() {
      if (display >= 0) sudo_kill_display(display);
      if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, nullptr, 0);
        child = -1;
      }
    };
    if (!started) {
      stop();
      std::cout << "SKIP root TCP Xvfb (could not start)\n";
    } else {
      const std::string spec = "127.0.0.1:" + std::to_string(display);
      Display* dpy = nullptr;
      for (int i = 0; i < 20 && !dpy; ++i) {
        dpy = XOpenDisplay(spec.c_str());
        if (!dpy) usleep(50000);
      }
      CHECK(dpy != nullptr);
      if (dpy) {
        const XPeerCred peer = x_peer_cred_from_fd(XConnectionNumber(dpy));
        XCloseDisplay(dpy);
        CHECK(peer.have_pid == false);
        CHECK(gpu_label_for_x_connection(-1, spec) == "Virtual framebuffer (Xvfb)");
        CHECK(server_comm_owning_display(spec) == "Xvfb");
        CHECK(server_comm_owning_display("tcp/127.0.0.1:" + std::to_string(display)) == "Xvfb");
        CHECK(server_comm_owning_display("[::1]:" + std::to_string(display)) == "Xvfb");
      }
      stop();
    }
  }

  // Session plumbing is system. Real applications stay applications.
  // Each name is classified with and without a top-level window.
  {
    struct Fix {
      const char* comm;
      const char* cmdline;
      ProcessClass expect;
      bool both_ways;
      bool window_override;
      bool window;
    };
    const Fix fixture[] = {
        {"xfce4-panel", "xfce4-panel", ProcessClass::System, true, false, false},
        {"panel-6-systray-plugin", "panel-6-systray-plugin", ProcessClass::System, true, false, false},
        {"panel-1-whiskermenu-plugin", "", ProcessClass::System, true, false, false},
        {"panel-6-systray", "panel-6-systray", ProcessClass::System, true, false, false},
        {"wrapper-2.0", "/usr/lib/xfce4/panel/wrapper-2.0", ProcessClass::System, true, false, false},
        {"plugin-helper", "/usr/lib/x86_64-linux-gnu/libxfce4panel.so.3", ProcessClass::System, true,
         false, false},
        {"xfdesktop", "xfdesktop", ProcessClass::System, true, false, false},
        {"xfwm4", "xfwm4", ProcessClass::System, true, false, false},
        {"xfce4-session", "xfce4-session", ProcessClass::System, true, false, false},
        {"xfsettingsd", "xfsettingsd", ProcessClass::System, true, false, false},
        {"xfconfd", "xfconfd", ProcessClass::System, true, false, false},
        {"xfce4-notifyd", "xfce4-notifyd", ProcessClass::System, true, false, false},
        {"lightdm", "lightdm", ProcessClass::System, true, false, false},
        {"lightdm-gtk-greeter", "lightdm-gtk-greeter", ProcessClass::System, true, false, false},
        {"lightdm-gtk-gre", "lightdm-gtk-greeter", ProcessClass::System, true, false, false},
        {"at-spi-bus-launcher", "at-spi-bus-launcher", ProcessClass::System, true, false, false},
        {"at-spi-bus-lau", "at-spi-bus-launcher", ProcessClass::System, true, false, false},
        {"at-spi2-registryd", "at-spi2-registryd", ProcessClass::System, true, false, false},
        {"at-spi2-registr", "at-spi2-registryd", ProcessClass::System, true, false, false},
        {"polkit-gnome-authentication-agent-1", "polkit-gnome-authentication-agent-1",
         ProcessClass::System, true, false, false},
        {"polkit-gnome-au", "polkit-gnome-authentication-agent-1", ProcessClass::System, true, false,
         false},
        {"xfce-polkit", "xfce-polkit", ProcessClass::System, true, false, false},
        {"lxpolkit", "lxpolkit", ProcessClass::System, true, false, false},
        {"xfce4-power-manager", "xfce4-power-manager", ProcessClass::System, true, false, false},
        {"xfce4-power-man", "xfce4-power-manager", ProcessClass::System, true, false, false},
        {"xfce4-screensaver", "xfce4-screensaver", ProcessClass::System, true, false, false},
        {"xfce4-screensav", "xfce4-screensaver", ProcessClass::System, true, false, false},
        {"light-locker", "light-locker", ProcessClass::System, true, false, false},
        {"xiccd", "xiccd", ProcessClass::System, true, false, false},
        {"gvfsd", "gvfsd", ProcessClass::System, true, false, false},
        {"gvfsd-trash", "gvfsd-trash", ProcessClass::System, true, false, false},
        {"gvfs-udisks2-volume-monitor", "gvfs-udisks2-volume-monitor", ProcessClass::System, true,
         false, false},
        {"gvfs-udisks2-vo", "gvfs-udisks2-volume-monitor", ProcessClass::System, true, false, false},
        {"dbus-daemon", "dbus-daemon --session", ProcessClass::System, true, false, false},
        {"dbus-broker", "dbus-broker", ProcessClass::System, true, false, false},
        {"dbus-broker-launch", "dbus-broker-launch", ProcessClass::System, true, false, false},
        {"pulseaudio", "pulseaudio", ProcessClass::System, true, false, false},
        {"pipewire", "pipewire", ProcessClass::System, true, false, false},
        {"pipewire-pulse", "pipewire-pulse", ProcessClass::System, true, false, false},
        {"wireplumber", "wireplumber", ProcessClass::System, true, false, false},
        {"nm-applet", "nm-applet", ProcessClass::System, true, false, false},
        {"blueman-applet", "blueman-applet", ProcessClass::System, true, false, false},
        {"xdg-desktop-portal", "xdg-desktop-portal", ProcessClass::System, true, false, false},
        {"xdg-desktop-portal-gtk", "xdg-desktop-portal-gtk", ProcessClass::System, true, false, false},
        {"xdg-desktop-por", "xdg-desktop-portal", ProcessClass::System, true, false, false},
        {"Thunar", "Thunar --daemon", ProcessClass::System, false, true, false},
        {"thunar", "thunar --daemon", ProcessClass::App, false, true, true},
        {"thunar", "thunar /home/user", ProcessClass::App, true, false, false},
        {"Thunar", "Thunar", ProcessClass::App, true, false, false},
        {"xfce4-terminal", "xfce4-terminal", ProcessClass::App, true, false, false},
        {"mousepad", "mousepad", ProcessClass::App, true, false, false},
        {"brave", "brave", ProcessClass::App, true, false, false},
        {"brave-browser", "brave-browser", ProcessClass::App, true, false, false},
        {"lunduke-about", "lunduke-about", ProcessClass::App, true, false, false},
        {"lunduke-paint", "lunduke-paint", ProcessClass::App, true, false, false},
        {"lunduke-edit", "lunduke-edit", ProcessClass::App, true, false, false},
        {"Edit", "Edit", ProcessClass::App, true, false, false},
        {"Paint", "Paint", ProcessClass::App, true, false, false},
        {"firefox", "firefox", ProcessClass::App, true, false, false},
        {"xterm", "xterm", ProcessClass::App, true, false, false},
    };
    for (const auto& row : fixture) {
      const bool windows[2] = {false, true};
      const int nwin = row.both_ways ? 2 : 1;
      for (int i = 0; i < nwin; ++i) {
        ProcessView view;
        view.comm = row.comm ? row.comm : "";
        view.cmdline = row.cmdline ? row.cmdline : "";
        view.owns_toplevel_window = row.both_ways ? windows[i] : row.window;
        const ProcessClass got = classify_process(view);
        if (got != row.expect) {
          std::cerr << "classify comm=" << view.comm << " cmdline=[" << view.cmdline
                    << "] window=" << view.owns_toplevel_window << " got="
                    << (got == ProcessClass::System ? "system" : "app") << "\n";
        }
        CHECK(got == row.expect);
      }
    }
    const std::string nul_daemon("Thunar\0--daemon", 15);
    ProcessView daemon;
    daemon.comm = "Thunar";
    daemon.cmdline = nul_daemon;
    daemon.owns_toplevel_window = false;
    CHECK(classify_process(daemon) == ProcessClass::System);
    daemon.owns_toplevel_window = true;
    CHECK(classify_process(daemon) == ProcessClass::App);

    ProcessView foreign;
    foreign.comm = "firefox";
    foreign.wm_res_class = "xfce4-panel";
    foreign.owns_toplevel_window = true;
    CHECK(classify_process(foreign) == ProcessClass::App);
    foreign.owns_toplevel_window = false;
    CHECK(classify_process(foreign) == ProcessClass::App);

    ProcessView nameless_panel;
    nameless_panel.wm_res_class = "xfdesktop";
    nameless_panel.owns_toplevel_window = true;
    CHECK(classify_process(nameless_panel) == ProcessClass::System);
    nameless_panel.owns_toplevel_window = false;
    CHECK(classify_process(nameless_panel) == ProcessClass::System);

    ProcessView edit_doc;
    edit_doc.comm = "mousepad";
    edit_doc.cmdline = "mousepad /tmp/xfce4-panel-notes.txt";
    edit_doc.owns_toplevel_window = true;
    CHECK(classify_process(edit_doc) == ProcessClass::App);
  }

  // System-class RAM is part of the LCOS System total, not its own rows.
  {
    const long used = 10000;
    std::vector<SessionRamSample> samples;
    auto add = [&](const char* comm, const char* cmdline, bool window, long rss) {
      SessionRamSample sample;
      sample.process.comm = comm;
      sample.process.cmdline = cmdline;
      sample.process.owns_toplevel_window = window;
      sample.rss_kb = rss;
      samples.push_back(sample);
    };
    add("mousepad", "mousepad", true, 1500);
    add("brave", "brave", true, 2500);
    add("thunar", "thunar /home/user", true, 900);
    add("xfce4-panel", "xfce4-panel", true, 800);
    add("Thunar", "Thunar --daemon", false, 200);
    add("xfdesktop", "xfdesktop", true, 300);
    add("dbus-daemon", "dbus-daemon --session", false, 100);
    const SessionRamSplit split = split_session_ram(used, samples);
    CHECK(split.app_kb == 1500 + 2500 + 900);
    CHECK(split.system_kb == 800 + 200 + 300 + 100);
    CHECK(split.lcos.shown_kb == used - split.app_kb);
    const SystemRemainder if_listed =
        system_remainder_kb(used, split.app_kb + split.system_kb);
    CHECK(split.lcos.shown_kb == if_listed.shown_kb + split.system_kb);
    CHECK(split.lcos.clamped == false);
  }

  // LCOS System stays last for name and RAM, both directions, including when
  // its RAM is the largest value and when it is the smallest.
  {
    std::vector<OrderedRow> rows(4);
    rows[0].name = "Mango";
    rows[0].pid = 11;
    rows[0].rss_kb = 50;
    rows[0].rss_known = true;
    rows[1].name = "Alpha";
    rows[1].pid = 10;
    rows[1].rss_kb = 100;
    rows[1].rss_known = true;
    rows[2].name = "LCOS System";
    rows[2].lcos_system = true;
    rows[2].rss_kb = 1;
    rows[2].rss_known = true;
    rows[3].name = "Zebra";
    rows[3].pid = 12;
    rows[3].rss_kb = 10;
    rows[3].rss_known = true;

    auto names_of = [&](AppSortColumn column, AppSortDirection direction) {
      std::vector<std::string> names;
      for (size_t index : order_app_row_indices(rows, column, direction)) {
        names.push_back(rows[index].name);
      }
      return names;
    };
    const std::vector<std::string> name_asc = names_of(AppSortColumn::Name,
                                                       AppSortDirection::Ascending);
    const std::vector<std::string> name_desc = names_of(AppSortColumn::Name,
                                                        AppSortDirection::Descending);
    const std::vector<std::string> ram_asc =
        names_of(AppSortColumn::Ram, AppSortDirection::Ascending);
    const std::vector<std::string> ram_desc =
        names_of(AppSortColumn::Ram, AppSortDirection::Descending);
    CHECK(name_asc == std::vector<std::string>({"Alpha", "Mango", "Zebra", "LCOS System"}));
    CHECK(name_desc == std::vector<std::string>({"Zebra", "Mango", "Alpha", "LCOS System"}));
    CHECK(ram_asc == std::vector<std::string>({"Zebra", "Mango", "Alpha", "LCOS System"}));
    CHECK(ram_desc == std::vector<std::string>({"Alpha", "Mango", "Zebra", "LCOS System"}));

    rows[2].rss_kb = 999999;
    const std::vector<std::string> huge =
        names_of(AppSortColumn::Ram, AppSortDirection::Descending);
    CHECK(huge.back() == "LCOS System");
    CHECK(huge.front() == "Alpha");

    rows[2].rss_kb = 0;
    const std::vector<std::string> tiny =
        names_of(AppSortColumn::Ram, AppSortDirection::Ascending);
    CHECK(tiny.back() == "LCOS System");
    CHECK(tiny.front() == "Zebra");
  }

  // Supporters: Steve Rockefeller is inserted at index 1. The previous
  // second entry, Steven P., moves to index 2. Jack Beckman is last.
  // The list had 4 names and grows by exactly 2. The file and the
  // built-in fallback used when that file is missing stay the same list.
  {
    const std::vector<std::string> expected = {
        "\"Fuzzy\"",
        "Steve Rockefeller",
        "Steven P.",
        "Chris Hammond",
        "Mike Beasley",
        "Jack Beckman",
    };
    constexpr std::size_t kPreviousCount = 4;
    std::string path;
    if (const char* env = std::getenv("LUNDUKE_SUPPORTERS_TXT")) path = env;
    if (path.empty()) {
      const std::string here = __FILE__;
      const auto slash = here.find_last_of("/\\");
      const std::string dir = slash == std::string::npos ? std::string(".") : here.substr(0, slash);
      path = dir + "/../data/supporters.txt";
    }
    std::ifstream supporters_file(path);
    CHECK(static_cast<bool>(supporters_file));
    const std::vector<std::string> entries = parse_supporter_entries(supporters_file);
    CHECK(entries.size() == kPreviousCount + 2);
    CHECK(builtin_supporter_entries().size() == kPreviousCount + 2);
    if (entries.size() >= 3) {
      CHECK(entries[1] == "Steve Rockefeller");
      CHECK(entries[2] == "Steven P.");
      CHECK(entries.back() == "Jack Beckman");
    }
    CHECK(entries == expected);
    CHECK(builtin_supporter_entries() == expected);
    CHECK(join_supporter_entries(builtin_supporter_entries()) ==
          "\"Fuzzy\", Steve Rockefeller, Steven P., Chris Hammond, Mike Beasley, Jack Beckman");
  }

  // Round 7: desktop Name, prettified class, row-tree rollup, refresh pace.
  {
    CHECK(prettify_class_name("Lunduke-paint") == "Lunduke Paint");
    CHECK(prettify_class_name("lunduke_paint") == "Lunduke Paint");
    CHECK(prettify_class_name("Xfce4-terminal") == "Xfce4 Terminal");
    CHECK(prettify_class_name("Brave-browser") == "Brave Browser");
    CHECK(prettify_class_name("Firefox") == "Firefox");
    CHECK(prettify_class_name("My_App") == "My App");

    const std::string fixture_dir = LUNDUKE_ABOUT_FIXTURE_DIR;
    CHECK(!fixture_dir.empty());
    auto load = [&](const std::string& file) {
      std::ifstream in(fixture_dir + "/" + file);
      CHECK(static_cast<bool>(in));
      std::stringstream buffer;
      buffer << in.rdbuf();
      const auto dot = file.rfind('.');
      const std::string id = dot == std::string::npos ? file : file.substr(0, dot);
      return parse_desktop_entry(buffer.str(), id);
    };
    const DesktopAppRecord paint = load("org.lunduke.LundukePaint.desktop");
    const DesktopAppRecord edit = load("org.lunduke.LundukeEdit.desktop");
    const DesktopAppRecord terminal = load("xfce4-terminal.desktop");
    const DesktopAppRecord files = load("thunar.desktop");
    const DesktopAppRecord message = load("xmessage.desktop");
    CHECK(paint.name == "Lunduke Paint");
    CHECK(paint.startup_wm_class == "lunduke-paint");
    CHECK(paint.icon == "org.lunduke.LundukePaint");
    CHECK(terminal.name == "Terminal");
    CHECK(terminal.startup_wm_class == "Xfce4-terminal");
    CHECK(message.name == "Message");
    CHECK(message.startup_wm_class.empty());
    const std::vector<DesktopAppRecord> catalog = {paint, edit, terminal, files, message};

    WindowFact paint_win = fact(1, 10, true, WindowKind::Normal);
    paint_win.res_class = "Lunduke-paint";
    paint_win.res_name = "lunduke-paint";
    paint_win.comm = "lunduke-paint";
    std::string icon;
    CHECK(display_name_for_window(paint_win, catalog, &icon) == "Lunduke Paint");
    CHECK(icon == "org.lunduke.LundukePaint");

    WindowFact term = fact(2, 11, true, WindowKind::Normal);
    term.res_class = "Xfce4-terminal";
    term.res_name = "xfce4-terminal";
    term.comm = "xfce4-terminal";
    icon.clear();
    CHECK(display_name_for_window(term, catalog, &icon) == "Terminal");
    CHECK(icon == "org.xfce.terminal");

    WindowFact thunar = fact(3, 12, true, WindowKind::Normal);
    thunar.res_class = "Thunar";
    thunar.res_name = "thunar";
    thunar.comm = "thunar";
    CHECK(display_name_for_window(thunar, catalog, &icon) == "File Manager");

    WindowFact xmsg = fact(4, 0, false, WindowKind::Normal);
    xmsg.res_class = "Xmessage";
    xmsg.res_name = "xmessage";
    xmsg.comm.clear();
    CHECK(display_name_for_window(xmsg, catalog, &icon) == "Message");

    WindowFact brave = fact(5, 13, true, WindowKind::Normal);
    brave.res_class = "Brave-browser";
    brave.res_name = "brave-browser";
    brave.comm = "brave";
    icon = "stale";
    CHECK(display_name_for_window(brave, catalog, &icon) == "Brave Browser");
    CHECK(icon.empty());

    // StartupWMClass wins over a desktop id that would name a different app.
    DesktopAppRecord decoy = paint;
    decoy.id = "lunduke-paint";
    decoy.name = "Wrong Paint";
    decoy.startup_wm_class.clear();
    DesktopAppRecord real = paint;
    const std::vector<DesktopAppRecord> prefer = {decoy, real};
    CHECK(display_name_for_window(paint_win, prefer, nullptr) == "Lunduke Paint");

    const auto grouped = group_windows({paint_win, term, brave}, catalog);
    CHECK(grouped.size() == 3);
    if (grouped.size() == 3) {
      CHECK(grouped[0].name == "Lunduke Paint");
      CHECK(grouped[0].class_name == "Lunduke-paint");
      CHECK(grouped[0].desktop_icon == "org.lunduke.LundukePaint");
      CHECK(grouped[1].name == "Terminal");
      CHECK(grouped[1].class_name == "Xfce4-terminal");
      CHECK(grouped[2].name == "Brave Browser");
    }
    const ForceClosePrompt prompt =
        force_close_prompt("Lunduke Paint", "lunduke-paint", 10, "Lunduke-paint", true);
    CHECK(prompt.primary == "Force Close \"Lunduke Paint\"?");
    CHECK(prompt.secondary.find("quit Lunduke Paint immediately") != std::string::npos);
    CHECK(prompt.secondary.find("command lunduke-paint") != std::string::npos);
    CHECK(prompt.secondary.find("Lunduke-paint") == std::string::npos ||
          prompt.secondary.find("Window class") != std::string::npos);
  }

  // Round 7: smaps_rollup is limited to the row tree, and only when the
  // cached charge is stale. A hidden window stops; an unfocused one slows.
  {
    std::unordered_map<pid_t, std::vector<pid_t>> children;
    std::unordered_set<pid_t> rows;
    rows.insert(10);
    rows.insert(20);
    children[10] = {11, 12};
    children[12] = {13, 20};
    children[20] = {21};
    children[1] = {10, 20, 30};
    for (pid_t pid = 100; pid < 580; ++pid) children[1].push_back(pid);
    const std::vector<pid_t> tree = row_tree_pids(rows, children);
    std::unordered_set<pid_t> tree_set(tree.begin(), tree.end());
    CHECK(tree_set.count(10));
    CHECK(tree_set.count(11));
    CHECK(tree_set.count(12));
    CHECK(tree_set.count(13));
    CHECK(tree_set.count(20));
    CHECK(tree_set.count(21));
    // 20 is another row, so it is not pulled in under 10, and neither is 21
    // via that edge. 21 is included because 20 is itself a row.
    CHECK(tree_set.count(30) == 0);
    CHECK(tree_set.count(100) == 0);
    CHECK(tree.size() < 30);
    CHECK(tree.size() == 6);

    CHECK(proc_charge_action(false, false) == ProcChargeAction::SkipRollup);
    CHECK(proc_charge_action(false, true) == ProcChargeAction::SkipRollup);
    CHECK(proc_charge_action(true, true) == ProcChargeAction::SkipRollup);
    CHECK(proc_charge_action(true, false) == ProcChargeAction::ReadRollup);

    CHECK(refresh_pace(false, false, true) == RefreshPace::Stopped);
    CHECK(refresh_pace(true, true, true) == RefreshPace::Stopped);
    CHECK(refresh_pace(true, false, false) == RefreshPace::Slow);
    CHECK(refresh_pace(true, false, true) == RefreshPace::Live);
    CHECK(refresh_interval_ms(RefreshPace::Stopped) == 0);
    CHECK(refresh_interval_ms(RefreshPace::Slow) == 10000);
    CHECK(refresh_interval_ms(RefreshPace::Live) == 3000);
  }

  if (g_failures != 0) {
    std::cerr << g_failures << " failure(s)\n";
    return 1;
  }
  std::cout << "ok\n";
  return 0;
}
