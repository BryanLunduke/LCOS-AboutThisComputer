// SPDX-License-Identifier: GPL-3.0-or-later
#include "about_logic.hpp"
#include "system_info.hpp"

#include <algorithm>
#include <climits>
#include <iostream>
#include <sstream>
#include <string>

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
  // A protected class on another window of the PID still protects the row.
  {
    WindowFact main = fact(1, 10, true, WindowKind::Normal);
    main.res_class = "Firefox";
    main.title = "xfwm4 docs";
    main.has_icon = true;
    WindowFact splash = fact(2, 10, true, WindowKind::Splash);
    splash.res_class = "xfwm4";
    splash.active = true;
    splash.title = "Splash";
    const auto grouped = group_windows({main, splash});
    CHECK(grouped.size() == 1);
    if (!grouped.empty()) {
      CHECK(grouped[0].xid == 1);
      CHECK(grouped[0].name == "Firefox");
      CHECK(grouped[0].tooltip == "xfwm4 docs");
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
    CHECK(group_windows({fact(1, 8, true, WindowKind::Utility)}).empty());
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
    CHECK(preferred.find("Iris") == std::string::npos);
    CHECK(preferred.find("snd") == std::string::npos);

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

  if (g_failures != 0) {
    std::cerr << g_failures << " failure(s)\n";
    return 1;
  }
  std::cout << "ok\n";
  return 0;
}
