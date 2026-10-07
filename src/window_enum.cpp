// SPDX-License-Identifier: GPL-3.0-or-later
#include "window_enum.hpp"
#include "about_logic.hpp"

#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <dirent.h>
#include <cctype>
#include <cstdlib>

namespace lundukeabout {
namespace {

struct IconCacheEntry {
  bool known = false;
  Glib::RefPtr<Gdk::Pixbuf> pixbuf;
};

std::unordered_map<unsigned long, IconCacheEntry>& icon_cache() {
  static std::unordered_map<unsigned long, IconCacheEntry> cache;
  return cache;
}

void prune_icon_cache(const std::unordered_set<unsigned long>& live) {
  auto& cache = icon_cache();
  for (auto it = cache.begin(); it != cache.end();) {
    if (!live.count(it->first)) it = cache.erase(it);
    else ++it;
  }
}

long parse_status_number(const std::string& line) {
  const auto pos = line.find_first_of("0123456789");
  if (pos == std::string::npos) return 0;
  try {
    return std::stol(line.substr(pos));
  } catch (...) {
    return 0;
  }
}

std::string read_proc_comm(pid_t pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/comm");
  std::string s;
  std::getline(in, s);
  if (!s.empty() && s.back() == '\n') s.pop_back();
  return s;
}

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

Glib::RefPtr<Gdk::Pixbuf> decode_net_wm_icon(Display* dpy, Window w) {
  Atom net_wm_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  const long length = static_cast<long>(kNetWmIconMaxItems);

  if (XGetWindowProperty(dpy, w, net_wm_icon, 0, length, False, XA_CARDINAL,
                         &actual_type, &actual_format, &nitems, &bytes_after,
                         &data) != Success ||
      !data || nitems < 2 || actual_format != 32) {
    if (data) XFree(data);
    return {};
  }

  auto* icons = reinterpret_cast<unsigned long*>(data);
  const auto choice = choose_net_wm_icon(icons, nitems);
  Glib::RefPtr<Gdk::Pixbuf> result;
  try {
    if (choice && choice->width > 0 && choice->height > 0 && choice->width < 512 &&
        choice->height < 512) {
      auto pb = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8,
                                    static_cast<int>(choice->width),
                                    static_cast<int>(choice->height));
      if (pb) {
        auto* pixels = pb->get_pixels();
        const int rowstride = pb->get_rowstride();
        for (unsigned long y = 0; y < choice->height; ++y) {
          for (unsigned long x = 0; x < choice->width; ++x) {
            const unsigned long argb =
                icons[choice->pixel_offset + y * choice->width + x];
            guchar* p = pixels + y * rowstride + x * 4;
            p[0] = (argb >> 16) & 0xff;
            p[1] = (argb >> 8) & 0xff;
            p[2] = argb & 0xff;
            p[3] = (argb >> 24) & 0xff;
          }
        }
        if (choice->width != 32 || choice->height != 32) {
          result = pb->scale_simple(32, 32, Gdk::INTERP_BILINEAR);
        } else {
          result = pb;
        }
      }
    }
  } catch (...) {
    XFree(data);
    return {};
  }
  XFree(data);
  return result;
}

Glib::RefPtr<Gdk::Pixbuf> cached_icon(Display* dpy, unsigned long xid) {
  auto& slot = icon_cache()[xid];
  if (slot.known) return slot.pixbuf;
  slot.pixbuf = decode_net_wm_icon(dpy, static_cast<Window>(xid));
  slot.known = true;
  return slot.pixbuf;
}

bool net_wm_icon_present(Display* dpy, Window w) {
  const auto it = icon_cache().find(static_cast<unsigned long>(w));
  if (it != icon_cache().end() && it->second.known) {
    return static_cast<bool>(it->second.pixbuf);
  }

  Atom net_wm_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  const int rc = XGetWindowProperty(dpy, w, net_wm_icon, 0, 0, False, XA_CARDINAL,
                                    &actual_type, &actual_format, &nitems,
                                    &bytes_after, &data);
  if (data) XFree(data);
  return rc == Success && actual_type == XA_CARDINAL && (nitems > 0 || bytes_after > 0);
}

std::string get_window_title(Display* dpy, Window w) {
  Atom net_name = XInternAtom(dpy, "_NET_WM_NAME", False);
  Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
  Atom actual_type = None;
  int actual_format = 0;
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
  std::memset(&tp, 0, sizeof(tp));
  if (XGetWMName(dpy, w, &tp) && tp.value) {
    std::string s(reinterpret_cast<char*>(tp.value));
    XFree(tp.value);
    return s;
  }
  return {};
}

struct WmClass {
  std::string res_name;
  std::string res_class;
};

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
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  pid_t pid = 0;
  if (XGetWindowProperty(dpy, w, atom, 0, 1, False, XA_CARDINAL, &actual_type,
                         &actual_format, &nitems, &bytes_after, &data) == Success &&
      data && nitems >= 1) {
    pid = static_cast<pid_t>(*reinterpret_cast<unsigned long*>(data));
  }
  if (data) XFree(data);
  return pid;
}

Window get_active_window(Display* dpy, Window root) {
  Atom atom = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  Window active = None;
  if (XGetWindowProperty(dpy, root, atom, 0, 1, False, XA_WINDOW, &actual_type,
                         &actual_format, &nitems, &bytes_after, &data) == Success &&
      data && nitems >= 1) {
    active = *reinterpret_cast<Window*>(data);
  }
  if (data) XFree(data);
  return active;
}

bool has_wm_state(Display* dpy, Window w) {
  Atom wm_state = XInternAtom(dpy, "WM_STATE", False);
  Atom actual_type = None;
  int actual_format = 0;
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

bool has_skip_taskbar(Display* dpy, Window w) {
  Atom state_atom = XInternAtom(dpy, "_NET_WM_STATE", False);
  Atom skip_tb = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  bool skip = false;
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

WindowKind get_window_kind(Display* dpy, Window w) {
  Atom type_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
  Atom desktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
  Atom dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
  Atom splash = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
  Atom menu = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU", False);
  Atom dropdown = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
  Atom popup = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
  Atom tooltip = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
  Atom notification = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
  Atom utility = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);

  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  WindowKind kind = WindowKind::Normal;
  if (XGetWindowProperty(dpy, w, type_atom, 0, 16, False, XA_ATOM, &actual_type,
                         &actual_format, &nitems, &bytes_after, &data) == Success &&
      data && nitems > 0) {
    auto* atoms = reinterpret_cast<Atom*>(data);
    for (unsigned long i = 0; i < nitems; ++i) {
      const Atom t = atoms[i];
      if (t == desktop || t == dock) {
        kind = WindowKind::DesktopOrDock;
        break;
      }
      if (t == splash) {
        kind = WindowKind::Splash;
        break;
      }
      if (t == menu || t == dropdown || t == popup) {
        kind = WindowKind::Menu;
        break;
      }
      if (t == tooltip) {
        kind = WindowKind::Tooltip;
        break;
      }
      if (t == notification) {
        kind = WindowKind::Notification;
        break;
      }
      if (t == utility) {
        kind = WindowKind::Utility;
        break;
      }
    }
  }
  if (data) XFree(data);
  if (kind == WindowKind::Normal && has_skip_taskbar(dpy, w)) {
    kind = WindowKind::SkipTaskbar;
  }
  return kind;
}

}  // namespace

enum class Phase { Collect, Inspect, Procs, Assemble, Icons, Done };

struct AppListRefresh::Impl {
  explicit Impl(pid_t self) : self_(self) {}

  bool step();
  void pump();
  void collect_page();
  void finish_collect(bool allow_tree);
  void query_tree();
  void inspect_one();
  void read_one_proc();
  void assemble();
  void load_one_icon();

  pid_t self_ = 0;
  bool display_ready_ = false;
  bool x11_ = false;
  bool tree_tried_ = false;
  GdkDisplay* gdk_ = nullptr;
  Display* dpy_ = nullptr;
  Window root_ = None;
  Window active_ = None;
  Phase phase_ = Phase::Collect;
  long client_offset_ = 0;
  std::vector<Window> clients_;
  size_t index_ = 0;
  std::vector<WindowFact> facts_;
  std::unordered_map<pid_t, std::string> comms_;
  std::vector<pid_t> proc_ids_;
  bool procs_listed_ = false;
  std::unordered_map<pid_t, long> rss_kb_;
  std::unordered_map<pid_t, std::vector<pid_t>> children_;
  std::vector<AppEntry> entries_;
  size_t icon_index_ = 0;
};

bool AppListRefresh::Impl::step() {
  if (phase_ == Phase::Done) return false;
  if (!display_ready_) {
    display_ready_ = true;
    gdk_ = gdk_display_get_default();
    if (!gdk_ || !GDK_IS_X11_DISPLAY(gdk_)) {
      x11_ = false;
      phase_ = Phase::Done;
      return false;
    }
    x11_ = true;
    dpy_ = GDK_DISPLAY_XDISPLAY(gdk_);
    root_ = DefaultRootWindow(dpy_);
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
  const auto uses_x = [](Phase phase) {
    return phase == Phase::Collect || phase == Phase::Inspect || phase == Phase::Icons;
  };
  // Proc walks do not need an X error trap. Popping the trap syncs with the
  // server, so keep that sync on the slices that actually talk to X.
  if (uses_x(phase_)) {
    X11ErrorTrap trap(gdk_);
    do {
      pump();
    } while (phase_ != Phase::Done && uses_x(phase_) &&
             std::chrono::steady_clock::now() < deadline);
  } else {
    do {
      pump();
    } while (phase_ != Phase::Done && !uses_x(phase_) &&
             std::chrono::steady_clock::now() < deadline);
  }
  return phase_ != Phase::Done;
}

void AppListRefresh::Impl::pump() {
  switch (phase_) {
    case Phase::Collect:
      collect_page();
      break;
    case Phase::Inspect:
      if (index_ >= clients_.size()) {
        phase_ = Phase::Procs;
        index_ = 0;
      } else {
        inspect_one();
      }
      break;
    case Phase::Procs:
      if (!procs_listed_) {
        procs_listed_ = true;
        if (DIR* dir = opendir("/proc")) {
          while (dirent* de = readdir(dir)) {
            if (!std::isdigit(static_cast<unsigned char>(de->d_name[0]))) continue;
            proc_ids_.push_back(static_cast<pid_t>(std::atoi(de->d_name)));
          }
          closedir(dir);
        }
      } else if (index_ >= proc_ids_.size()) {
        phase_ = Phase::Assemble;
      } else {
        read_one_proc();
      }
      break;
    case Phase::Assemble:
      assemble();
      break;
    case Phase::Icons:
      if (icon_index_ >= entries_.size()) phase_ = Phase::Done;
      else load_one_icon();
      break;
    case Phase::Done:
      break;
  }
}

void AppListRefresh::Impl::finish_collect(bool allow_tree) {
  if (allow_tree && clients_.empty() && !tree_tried_) query_tree();
  active_ = get_active_window(dpy_, root_);
  std::unordered_set<unsigned long> live;
  live.reserve(clients_.size());
  for (Window w : clients_) live.insert(static_cast<unsigned long>(w));
  prune_icon_cache(live);
  index_ = 0;
  phase_ = Phase::Inspect;
}

void AppListRefresh::Impl::query_tree() {
  tree_tried_ = true;
  Window rr = None, parent = None;
  Window* children = nullptr;
  unsigned int nchildren = 0;
  if (!XQueryTree(dpy_, root_, &rr, &parent, &children, &nchildren)) return;
  for (unsigned int i = 0; i < nchildren; ++i) {
    if (has_wm_state(dpy_, children[i])) clients_.push_back(children[i]);
  }
  if (children) XFree(children);
}

void AppListRefresh::Impl::collect_page() {
  Atom client_list = XInternAtom(dpy_, "_NET_CLIENT_LIST", False);
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  const int rc =
      XGetWindowProperty(dpy_, root_, client_list, client_offset_, 1024, False, XA_WINDOW,
                         &actual_type, &actual_format, &nitems, &bytes_after, &data);
  if (rc != Success || actual_type != XA_WINDOW) {
    if (data) XFree(data);
    finish_collect(true);
    return;
  }
  if (data && nitems > 0) {
    auto* wins = reinterpret_cast<Window*>(data);
    for (unsigned long i = 0; i < nitems; ++i) {
      if (static_cast<long>(clients_.size()) >= kMaxClientIds) break;
      clients_.push_back(wins[i]);
    }
  }
  if (data) XFree(data);

  if (static_cast<long>(clients_.size()) >= kMaxClientIds) {
    finish_collect(false);
    return;
  }
  long next = client_offset_;
  if (!client_list_advance(client_offset_, nitems, bytes_after, kMaxClientIds, next)) {
    finish_collect(clients_.empty());
    return;
  }
  client_offset_ = next;
}

void AppListRefresh::Impl::inspect_one() {
  const Window w = clients_[index_++];
  const pid_t pid = get_net_wm_pid(dpy_, w);
  if (pid == self_) return;

  WindowFact fact;
  fact.xid = static_cast<unsigned long>(w);
  fact.kind = get_window_kind(dpy_, w);
  fact.active = (active_ != None && w == active_);
  fact.has_pid = pid > 1;
  fact.pid = fact.has_pid ? pid : 0;
  fact.title = get_window_title(dpy_, w);
  fact.has_title = !fact.title.empty();
  const WmClass wm = get_wm_class(dpy_, w);
  fact.res_name = wm.res_name;
  fact.res_class = wm.res_class;
  if (fact.has_pid) {
    const auto it = comms_.find(pid);
    if (it == comms_.end()) {
      fact.comm = read_proc_comm(pid);
      comms_.emplace(pid, fact.comm);
    } else {
      fact.comm = it->second;
    }
  }
  if (fact.kind == WindowKind::Normal) fact.has_icon = net_wm_icon_present(dpy_, w);
  facts_.push_back(std::move(fact));
}

void AppListRefresh::Impl::read_one_proc() {
  const pid_t pid = proc_ids_[index_++];
  std::ifstream in("/proc/" + std::to_string(pid) + "/status");
  if (!in) return;
  std::string line;
  pid_t ppid = 0;
  long rss = 0;
  while (std::getline(in, line)) {
    if (line.compare(0, 5, "PPid:") == 0) {
      ppid = static_cast<pid_t>(parse_status_number(line));
    } else if (line.compare(0, 8, "RssAnon:") == 0) {
      rss = parse_status_number(line);
    }
  }
  // Missing RssAnon (kernel threads) stays 0. One status read per process.
  rss_kb_[pid] = rss;
  if (ppid > 0 && ppid != pid) children_[ppid].push_back(pid);
}

void AppListRefresh::Impl::assemble() {
  const std::vector<GroupedApp> groups = group_windows(facts_);
  std::unordered_set<pid_t> row_pids;
  for (const auto& g : groups) {
    if (g.has_pid) row_pids.insert(g.pid);
  }
  entries_.clear();
  entries_.reserve(groups.size());
  for (const auto& g : groups) {
    AppEntry entry;
    entry.name = g.name;
    entry.tooltip = g.tooltip;
    entry.pid = g.has_pid ? g.pid : 0;
    entry.xid = g.xid;
    entry.protected_app = g.protected_app;
    entry.protect_reason = g.protect_reason;
    if (g.has_pid) {
      entry.rss_known = true;
      entry.rss_kb = rollup_rss_anon(g.pid, rss_kb_, children_, row_pids);
    }
    entries_.push_back(std::move(entry));
  }
  std::stable_sort(entries_.begin(), entries_.end(),
                   [](const AppEntry& a, const AppEntry& b) {
                     const long ar = a.rss_known ? a.rss_kb : -1;
                     const long br = b.rss_known ? b.rss_kb : -1;
                     if (ar != br) return ar > br;
                     if (a.pid != b.pid) return a.pid < b.pid;
                     return a.xid < b.xid;
                   });
  icon_index_ = 0;
  phase_ = Phase::Icons;
}

void AppListRefresh::Impl::load_one_icon() {
  AppEntry& entry = entries_[icon_index_++];
  if (entry.xid == 0) return;
  entry.icon = cached_icon(dpy_, entry.xid);
}

AppListRefresh::AppListRefresh(pid_t self_pid) : impl_(std::make_unique<Impl>(self_pid)) {}

AppListRefresh::~AppListRefresh() = default;

bool AppListRefresh::step() { return impl_->step(); }

bool AppListRefresh::on_x11() const { return impl_->x11_; }

const std::vector<AppEntry>& AppListRefresh::entries() const { return impl_->entries_; }

bool window_xid_matches_pid(unsigned long xid, pid_t pid) {
  if (xid == 0 || pid <= 1) return false;
  auto* gdk = gdk_display_get_default();
  if (!gdk || !GDK_IS_X11_DISPLAY(gdk)) return false;
  Display* dpy = GDK_DISPLAY_XDISPLAY(gdk);
  X11ErrorTrap trap(gdk);
  return get_net_wm_pid(dpy, static_cast<Window>(xid)) == pid;
}

}  // namespace lundukeabout
