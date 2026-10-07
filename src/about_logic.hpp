// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sys/types.h>

namespace lundukeabout {

// CARD32 units. Large enough for a 256px icon plus the smaller sizes that
// usually precede it, and small enough that one property cannot exhaust RSS.
constexpr unsigned long kNetWmIconMaxItems = 131072;
constexpr long kMaxClientIds = 100000;

struct IconChoice {
  unsigned long width = 0;
  unsigned long height = 0;
  unsigned long pixel_offset = 0;
};

// Best ~32px frame, or nullopt. Malformed frames that overflow or would
// leave the cursor unmoved stop the walk; the function does not loop.
std::optional<IconChoice> choose_net_wm_icon(const unsigned long* icons,
                                            unsigned long nitems);

// After a page of CARD32 window ids has been consumed: true means another
// page should be read at next_offset. Stops at the end and at max_ids.
bool client_list_advance(long offset, unsigned long nitems,
                         unsigned long bytes_after, long max_ids,
                         long& next_offset);

// rss of root plus descendants. A descendant that is itself in row_pids is
// omitted, along with its subtree, so that process is counted on its own row.
long rollup_rss_anon(pid_t root,
                     const std::unordered_map<pid_t, long>& rss_kb,
                     const std::unordered_map<pid_t, std::vector<pid_t>>& children,
                     const std::unordered_set<pid_t>& row_pids);

struct ClassHint {
  std::string res_name;
  std::string res_class;
};

// Exact WM_CLASS (instance and class) or /proc comm. comm may be the kernel's
// 15-character truncation of a longer session binary. Titles are not compared.
bool is_protected_identity(const ClassHint& wm, const std::string& comm,
                           std::string& reason);

enum class WindowKind {
  Normal,
  SkipTaskbar,
  DesktopOrDock,
  Splash,
  Menu,
  Tooltip,
  Notification,
  Utility,
};

struct WindowFact {
  unsigned long xid = 0;
  pid_t pid = 0;
  bool has_pid = false;
  WindowKind kind = WindowKind::Normal;
  bool active = false;
  bool has_title = false;
  bool has_icon = false;
  std::string title;
  std::string res_name;
  std::string res_class;
  std::string comm;
};

struct GroupedApp {
  unsigned long xid = 0;
  pid_t pid = 0;
  bool has_pid = false;
  std::string name;
  std::string tooltip;
  bool protected_app = false;
  std::string protect_reason;
};

// One row per PID (or per XID when the window has no PID). The representative
// prefers the active window, then a window with a title and an icon.
// Splash, menu, tooltip, notification, utility, dock, desktop, and
// skip-taskbar windows are not representatives. Protection is OR'd across
// every window of the PID.
std::vector<GroupedApp> group_windows(const std::vector<WindowFact>& windows);

struct GpuDevice {
  std::string slot;
  unsigned class_code = 0;
  unsigned vendor = 0;
  unsigned device = 0;
  std::string driver;
};

struct PciDb {
  std::unordered_map<unsigned, std::string> vendors;
  std::unordered_map<unsigned, std::unordered_map<unsigned, std::string>> devices;
};

bool is_display_class(unsigned class_code);
PciDb parse_pci_ids(std::istream& in);
// Every display-class device. When a VGA controller and a 3D controller are
// both present, only the 3D controllers are named.
std::string gpu_label_from_devices(const std::vector<GpuDevice>& devices,
                                   const PciDb& db);

// view_height <= 1 is the only "not ready" guard. A short cap still crawls.
bool credits_should_crawl(int view_height, int layout_pixel_height, int fit_slack);

double clamp_scroll_value(double value, double upper, double page_size);

}  // namespace lundukeabout
