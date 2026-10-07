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
  // Root and descendants whose RssAnon is included in rss_kb.
  std::vector<ProcPin> kill_pins;
};

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
