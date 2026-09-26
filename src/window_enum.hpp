// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gdkmm.h>
#include <string>
#include <vector>
#include <cstdint>

namespace lundukeabout {

struct AppEntry {
  std::string name;
  pid_t pid = 0;
  long rss_kb = 0;      // VmRSS
  long vsize_kb = 0;    // VmSize (approx "allocated")
  Glib::RefPtr<Gdk::Pixbuf> icon;
  bool protected_app = false;  // cannot force-close
  std::string protect_reason;
  unsigned long xid = 0;
};

// Enumerate toplevel client windows with desktop presence (X11).
std::vector<AppEntry> enumerate_graphical_apps(pid_t self_pid);

}  // namespace lundukeabout
