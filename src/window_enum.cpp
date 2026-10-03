// SPDX-License-Identifier: GPL-3.0-or-later
#include "window_enum.hpp"

#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <fstream>
#include <set>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <unistd.h>

namespace lundukeabout {
namespace {

long read_proc_kb(pid_t pid, const char* key) {
  std::string path = "/proc/" + std::to_string(pid) + "/status";
  std::ifstream in(path);
  if (!in) return 0;
  std::string line;
  const std::string prefix = std::string(key);
  while (std::getline(in, line)) {
    if (line.compare(0, prefix.size(), prefix) != 0) continue;
    // "RssAnon:\t  12345 kB" (also VmRSS:/VmSize:/RssShmem:)
    auto pos = line.find_first_of("0123456789");
    if (pos == std::string::npos) return 0;
    try {
      return std::stol(line.substr(pos));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

std::string read_proc_comm(pid_t pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/comm");
  std::string s;
  std::getline(in, s);
  return s;
}

struct WmClass {
  std::string res_name;
  std::string res_class;
};

bool iequals(const std::string& field, const char* needle) {
  if (!needle) return false;
  const size_t n = std::strlen(needle);
  if (field.size() != n) return false;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char a = static_cast<unsigned char>(field[i]);
    const unsigned char b = static_cast<unsigned char>(needle[i]);
    if (std::tolower(a) != std::tolower(b)) return false;
  }
  return true;
}

// Match the window class (WM_CLASS instance and class) exactly. Do not search
// the window title: a browser page titled "... xfwm4 ..." must stay closable.
// Comparison is case-insensitive so "Xfwm4" still matches the "xfwm4" needle.
bool is_protected_class(const WmClass& wm, std::string& reason) {
  auto hit = [&](const char* needle, const char* why) {
    if (iequals(wm.res_name, needle) || iequals(wm.res_class, needle)) {
      reason = why;
      return true;
    }
    return false;
  };

  if (hit("lunduke-about", "This application")) return true;
  if (hit("about-this-computer", "This application")) return true;
  if (hit("xfce4-panel", "Desktop panel")) return true;
  if (hit("xfce4-session", "Session manager")) return true;
  if (hit("xfwm4", "Window manager")) return true;
  if (hit("xfdesktop", "Desktop")) return true;
  if (hit("xfsettingsd", "Settings daemon")) return true;
  if (hit("xfconfd", "Settings daemon")) return true;
  if (hit("polkit", "System service")) return true;
  return false;
}

// Swallow async X errors (BadWindow when a client dies mid-refresh) so GDK's
// default handler does not fatal-exit. Pop syncs before the trap is lifted.
class X11ErrorTrap {
 public:
  explicit X11ErrorTrap(GdkDisplay* display) : display_(display) {
    if (display_) gdk_x11_display_error_trap_push(display_);
  }
  ~X11ErrorTrap() {
    if (display_) gdk_x11_display_error_trap_pop_ignored(display_);
  }
  X11ErrorTrap(const X11ErrorTrap&) = delete;
  X11ErrorTrap& operator=(const X11ErrorTrap&) = delete;

 private:
  GdkDisplay* display_ = nullptr;
};

Glib::RefPtr<Gdk::Pixbuf> pixbuf_from_net_wm_icon(Display* dpy, Window w) {
  Atom net_wm_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
  Atom actual_type;
  int actual_format;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;

  if (XGetWindowProperty(dpy, w, net_wm_icon, 0, (~0L), False, XA_CARDINAL,
                         &actual_type, &actual_format, &nitems, &bytes_after,
                         &data) != Success ||
      !data || nitems < 2 || actual_format != 32) {
    if (data) XFree(data);
    return {};
  }

  auto* icons = reinterpret_cast<unsigned long*>(data);
  unsigned long best_w = 0, best_h = 0;
  unsigned long best_off = 0;
  unsigned long off = 0;
  while (off + 2 < nitems) {
    unsigned long iw = icons[off];
    unsigned long ih = icons[off + 1];
    if (iw == 0 || ih == 0 || off + 2 + iw * ih > nitems) break;
    // Prefer ~32px
    if (best_w == 0 ||
        (std::abs(static_cast<long>(iw) - 32) < std::abs(static_cast<long>(best_w) - 32))) {
      best_w = iw;
      best_h = ih;
      best_off = off + 2;
    }
    off += 2 + iw * ih;
  }

  Glib::RefPtr<Gdk::Pixbuf> result;
  if (best_w > 0 && best_h > 0 && best_w < 512 && best_h < 512) {
    auto pb = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8,
                                  static_cast<int>(best_w), static_cast<int>(best_h));
    auto* pixels = pb->get_pixels();
    int rowstride = pb->get_rowstride();
    for (unsigned long y = 0; y < best_h; ++y) {
      for (unsigned long x = 0; x < best_w; ++x) {
        unsigned long argb = icons[best_off + y * best_w + x];
        guchar* p = pixels + y * rowstride + x * 4;
        p[0] = (argb >> 16) & 0xff;  // R
        p[1] = (argb >> 8) & 0xff;   // G
        p[2] = argb & 0xff;          // B
        p[3] = (argb >> 24) & 0xff;  // A
      }
    }
    if (best_w != 32 || best_h != 32) {
      result = pb->scale_simple(32, 32, Gdk::INTERP_BILINEAR);
    } else {
      result = pb;
    }
  }
  XFree(data);
  return result;
}

std::string get_window_title(Display* dpy, Window w) {
  Atom net_name = XInternAtom(dpy, "_NET_WM_NAME", False);
  Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
  Atom actual_type;
  int actual_format;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;

  if (XGetWindowProperty(dpy, w, net_name, 0, 1024, False, utf8, &actual_type,
                         &actual_format, &nitems, &bytes_after, &data) == Success &&
      data && nitems > 0) {
    std::string s(reinterpret_cast<char*>(data), nitems);
    XFree(data);
    return s;
  }
  if (data) XFree(data);

  XTextProperty tp;
  if (XGetWMName(dpy, w, &tp) && tp.value) {
    std::string s(reinterpret_cast<char*>(tp.value));
    XFree(tp.value);
    return s;
  }
  return {};
}

WmClass get_wm_class(Display* dpy, Window w) {
  WmClass wm;
  XClassHint hint;
  std::memset(&hint, 0, sizeof(hint));
  if (XGetClassHint(dpy, w, &hint)) {
    if (hint.res_name) wm.res_name = hint.res_name;
    if (hint.res_class) wm.res_class = hint.res_class;
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
  }
  return wm;
}

pid_t get_net_wm_pid(Display* dpy, Window w) {
  Atom atom = XInternAtom(dpy, "_NET_WM_PID", False);
  Atom actual_type;
  int actual_format;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  pid_t pid = 0;
  if (XGetWindowProperty(dpy, w, atom, 0, 1, False, XA_CARDINAL, &actual_type,
                         &actual_format, &nitems, &bytes_after, &data) == Success &&
      data && nitems == 1) {
    pid = static_cast<pid_t>(*reinterpret_cast<unsigned long*>(data));
  }
  if (data) XFree(data);
  return pid;
}

bool has_wm_state(Display* dpy, Window w) {
  Atom wm_state = XInternAtom(dpy, "WM_STATE", False);
  Atom actual_type;
  int actual_format;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  bool ok = false;
  if (XGetWindowProperty(dpy, w, wm_state, 0, 2, False, AnyPropertyType, &actual_type,
                         &actual_format, &nitems, &bytes_after, &data) == Success &&
      data && nitems > 0) {
    ok = true;
  }
  if (data) XFree(data);
  return ok;
}

bool is_skip_taskbar_or_desktop(Display* dpy, Window w) {
  Atom type_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
  Atom desktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
  Atom dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
  Atom actual_type;
  int actual_format;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  bool skip = false;
  if (XGetWindowProperty(dpy, w, type_atom, 0, 8, False, XA_ATOM, &actual_type,
                         &actual_format, &nitems, &bytes_after, &data) == Success &&
      data && nitems > 0) {
    auto* atoms = reinterpret_cast<Atom*>(data);
    for (unsigned long i = 0; i < nitems; ++i) {
      if (atoms[i] == desktop || atoms[i] == dock) skip = true;
    }
  }
  if (data) XFree(data);

  Atom state_atom = XInternAtom(dpy, "_NET_WM_STATE", False);
  Atom skip_tb = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
  data = nullptr;
  nitems = 0;
  if (XGetWindowProperty(dpy, w, state_atom, 0, 32, False, XA_ATOM, &actual_type,
                         &actual_format, &nitems, &bytes_after, &data) == Success &&
      data && nitems > 0) {
    auto* atoms = reinterpret_cast<Atom*>(data);
    for (unsigned long i = 0; i < nitems; ++i) {
      if (atoms[i] == skip_tb) skip = true;
    }
  }
  if (data) XFree(data);
  return skip;
}

void collect_clients(Display* dpy, Window root, std::vector<Window>& out) {
  Atom client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
  Atom actual_type;
  int actual_format;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;

  if (XGetWindowProperty(dpy, root, client_list, 0, 1024, False, XA_WINDOW,
                         &actual_type, &actual_format, &nitems, &bytes_after,
                         &data) == Success &&
      data && nitems > 0) {
    auto* wins = reinterpret_cast<Window*>(data);
    for (unsigned long i = 0; i < nitems; ++i) out.push_back(wins[i]);
    XFree(data);
    return;
  }
  if (data) XFree(data);

  // Fallback: walk children looking for WM_STATE
  Window rr, parent;
  Window* children = nullptr;
  unsigned int nchildren = 0;
  if (XQueryTree(dpy, root, &rr, &parent, &children, &nchildren)) {
    for (unsigned int i = 0; i < nchildren; ++i) {
      if (has_wm_state(dpy, children[i])) out.push_back(children[i]);
    }
    if (children) XFree(children);
  }
}

}  // namespace

std::vector<AppEntry> enumerate_graphical_apps(pid_t self_pid) {
  std::vector<AppEntry> result;
  auto* gdk_display = gdk_display_get_default();
  if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display)) return result;

  Display* dpy = GDK_DISPLAY_XDISPLAY(gdk_display);
  Window root = DefaultRootWindow(dpy);

  // Covers raw XGetWindowProperty / XGetWMName / XGetClassHint below. A window
  // that closes during this pass must not abort the process, and the windows
  // that are still alive must still be listed.
  X11ErrorTrap trap(gdk_display);

  std::vector<Window> clients;
  collect_clients(dpy, root, clients);

  std::set<pid_t> seen_pids;

  for (Window w : clients) {
    if (is_skip_taskbar_or_desktop(dpy, w)) continue;

    // pid <= 0 means the window has no _NET_WM_PID. Do not invent one.
    const pid_t pid = get_net_wm_pid(dpy, w);
    const bool has_pid = pid > 1;
    if (has_pid) {
      if (pid == self_pid) continue;
      if (!seen_pids.insert(pid).second) continue;
    }

    const std::string title = get_window_title(dpy, w);
    const WmClass wm = get_wm_class(dpy, w);

    // Keep the real title, including long UTF-8 names. The row label ellipsizes.
    std::string name = title;
    if (name.empty() && has_pid) name = read_proc_comm(pid);
    if (name.empty()) name = wm.res_name;
    if (name.empty()) name = wm.res_class;
    if (name.empty()) {
      // Nothing to show, and no real PID to attach. Skip it.
      if (!has_pid) continue;
      name = "pid " + std::to_string(pid);
    }

    AppEntry e;
    e.name = name;
    e.pid = has_pid ? pid : 0;
    if (has_pid) {
      e.rss_kb = read_proc_kb(pid, "RssAnon:");
      e.vsize_kb = read_proc_kb(pid, "VmSize:");
      if (e.vsize_kb < e.rss_kb) e.vsize_kb = e.rss_kb;
    }
    e.icon = pixbuf_from_net_wm_icon(dpy, w);
    e.xid = static_cast<unsigned long>(w);

    std::string reason;
    if (is_protected_class(wm, reason)) {
      e.protected_app = true;
      e.protect_reason = reason.empty() ? "Protected" : reason;
    }

    result.push_back(std::move(e));
  }

  std::stable_sort(result.begin(), result.end(),
                   [](const AppEntry& a, const AppEntry& b) {
                     if (a.rss_kb != b.rss_kb) return a.rss_kb > b.rss_kb;
                     if (a.pid != b.pid) return a.pid < b.pid;
                     return a.xid < b.xid;
                   });
  return result;
}

}  // namespace lundukeabout
