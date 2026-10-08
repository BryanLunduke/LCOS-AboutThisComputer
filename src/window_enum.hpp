// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "about_logic.hpp"

#include <gdkmm.h>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace lundukeabout {

struct AppEntry {
  std::string name;
  std::string tooltip;
  pid_t pid = 0;
  long rss_kb = 0;
  // False when the window has no _NET_WM_PID. The row shows an em dash.
  bool rss_known = false;
  Glib::RefPtr<Gdk::Pixbuf> icon;
  bool protected_app = false;
  std::string protect_reason;
  unsigned long xid = 0;
  // Refresh-time /proc identity. Force Close signals this comm + starttime.
  std::string comm;
  unsigned long long start_ticks = 0;
  bool identity_ok = false;
  // Raw UTF-8 WM_CLASS class, before a disambiguating title or pid suffix.
  std::string class_name;
  // "pid N" or "window N" when the visible name is shared. Empty otherwise.
  // Shown beside the name so ellipsis cannot hide it.
  std::string distinguish;
  // Root and descendants whose anonymous charge is included in rss_kb.
  std::vector<ProcPin> kill_pins;
  // The LCOS System remainder. Always the last list row. Not an application.
  bool lcos_system = false;
  // Themed icon from the desktop entry, used when the window has no
  // _NET_WM_ICON.
  std::string desktop_icon;
};

// Counts for one process scan. status_reads stays 0: ppid and rss come from
// stat, and smaps_rollup is opened only for a row and its descendants.
struct ProcScanStats {
  int stat_reads = 0;
  int status_reads = 0;
  int smaps_reads = 0;
  int cache_reuses = 0;
  int pids_seen = 0;
  int pids_skipped_uid = 0;
  int rollup_targets = 0;
};

void reset_proc_scan_stats();
ProcScanStats proc_scan_stats();

// One refresh of the client list, performed in short slices so the GTK
// thread can paint and handle input between X round trips.
class AppListRefresh {
 public:
  explicit AppListRefresh(pid_t self_pid);
  ~AppListRefresh();
  AppListRefresh(const AppListRefresh&) = delete;
  AppListRefresh& operator=(const AppListRefresh&) = delete;

  // Returns true when another slice is needed.
  bool step();
  bool on_x11() const;
  const std::vector<AppEntry>& entries() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Force Close: this window still belongs to this PID. Does not walk the
// rest of the client list.
bool window_xid_matches_pid(unsigned long xid, pid_t pid);

}  // namespace lundukeabout
