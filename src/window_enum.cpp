// SPDX-License-Identifier: GPL-3.0-or-later
#include "window_enum.hpp"
#include "about_logic.hpp"
#include "system_info.hpp"

#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <dirent.h>
#include <cctype>
#include <cerrno>
#include <cstdlib>

namespace lundukeabout {
namespace {

struct IconCacheEntry {
  bool known = false;
  pid_t pid = -1;
  bool present = false;
  unsigned long long fingerprint = 0;
  Glib::RefPtr<Gdk::Pixbuf> pixbuf;
};

constexpr int kMaxWmTreeDepth = 6;

unsigned long property_alloc_nbytes(int format, unsigned long nitems) {
  if (format == 32) {
    if (sizeof(unsigned long) == 0 || nitems > (ULONG_MAX - 1) / sizeof(unsigned long)) return 0;
    return nitems * sizeof(unsigned long) + 1;
  }
  if (format == 16) {
    if (nitems > (ULONG_MAX - 1) / 2) return 0;
    return nitems * 2 + 1;
  }
  if (format == 8) {
    if (nitems == ULONG_MAX) return 0;
    return nitems + 1;
  }
  return 0;
}

unsigned long long fnv_mix(unsigned long long hash, unsigned long long value) {
  hash ^= value;
  hash *= 1099511628211ull;
  return hash;
}

std::string read_fd_limited(int fd) {
  std::string data;
  char buf[4096];
  while (data.size() < (1u << 20)) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    data.append(buf, buf + n);
  }
  return data;
}

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

bool is_toplevel_kind(WindowKind kind) {
  // A window the user can treat as belonging to the process. Menus, tooltips,
  // splashes, and notifications do not keep a Thunar daemon on the app list.
  return kind == WindowKind::Normal || kind == WindowKind::DesktopOrDock ||
         kind == WindowKind::SkipTaskbar || kind == WindowKind::Utility;
}

std::string read_proc_cmdline(pid_t pid) {
  if (pid <= 1) return {};
  std::ifstream in("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
  if (!in) return {};
  std::string data;
  char buf[4096];
  while (data.size() < (1u << 20)) {
    in.read(buf, sizeof(buf));
    const auto n = in.gcount();
    if (n <= 0) break;
    data.append(buf, static_cast<size_t>(n));
  }
  return data;
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

struct IconFetch {
  Display* dpy = nullptr;
  Window window = None;
  Atom prop = None;
  std::unordered_map<unsigned long, unsigned long> words;
};

bool read_cardinal_words(Display* dpy, Window window, Atom prop, unsigned long offset,
                         unsigned long count, std::vector<unsigned long>& out,
                         unsigned long* total_items) {
  out.clear();
  if (count == 0 || count > kNetWmIconMaxItems) return false;
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  const int rc = XGetWindowProperty(dpy, window, prop, static_cast<long>(offset),
                                    static_cast<long>(count), False, XA_CARDINAL,
                                    &actual_type, &actual_format, &nitems, &bytes_after,
                                    &data);
  if (rc != Success || !data) {
    if (data) XFree(data);
    return false;
  }
  const unsigned long alloc = property_alloc_nbytes(actual_format, nitems);
  if (!x_property_indexable_as_longs(actual_format, actual_type == XA_CARDINAL, nitems,
                                     alloc)) {
    XFree(data);
    return false;
  }
  auto* longs = reinterpret_cast<unsigned long*>(data);
  out.assign(longs, longs + nitems);
  if (total_items) *total_items = offset + nitems + bytes_after / 4;
  XFree(data);
  return true;
}

bool fetch_icon_word(void* ctx, unsigned long index, unsigned long& word) {
  auto* fetch = static_cast<IconFetch*>(ctx);
  const auto found = fetch->words.find(index);
  if (found != fetch->words.end()) {
    word = found->second;
    return true;
  }
  std::vector<unsigned long> got;
  if (!read_cardinal_words(fetch->dpy, fetch->window, fetch->prop, index, 2, got, nullptr) ||
      got.empty()) {
    return false;
  }
  for (unsigned long i = 0; i < got.size(); ++i) fetch->words[index + i] = got[i];
  const auto again = fetch->words.find(index);
  if (again == fetch->words.end()) return false;
  word = again->second;
  return true;
}

bool icon_property_total(Display* dpy, Window window, Atom prop, unsigned long& total) {
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  const int rc = XGetWindowProperty(dpy, window, prop, 0, 0, False, XA_CARDINAL,
                                    &actual_type, &actual_format, &nitems, &bytes_after,
                                    &data);
  if (data) XFree(data);
  if (rc != Success || actual_type != XA_CARDINAL || actual_format != 32) return false;
  total = nitems + bytes_after / 4;
  return true;
}

Glib::RefPtr<Gdk::Pixbuf> pixbuf_from_argb(const std::vector<unsigned long>& argb,
                                           unsigned long width, unsigned long height) {
  if (width == 0 || height == 0 || width >= 512 || height >= 512) return {};
  if (argb.size() < width * height) return {};
  try {
    auto pb = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, true, 8, static_cast<int>(width),
                                  static_cast<int>(height));
    if (!pb) return {};
    auto* pixels = pb->get_pixels();
    const int rowstride = pb->get_rowstride();
    for (unsigned long y = 0; y < height; ++y) {
      for (unsigned long x = 0; x < width; ++x) {
        const unsigned long pixel = argb[y * width + x];
        guchar* p = pixels + y * rowstride + x * 4;
        p[0] = (pixel >> 16) & 0xff;
        p[1] = (pixel >> 8) & 0xff;
        p[2] = pixel & 0xff;
        p[3] = (pixel >> 24) & 0xff;
      }
    }
    if (width != 32 || height != 32) return pb->scale_simple(32, 32, Gdk::INTERP_BILINEAR);
    return pb;
  } catch (...) {
    return {};
  }
}

Glib::RefPtr<Gdk::Pixbuf> decode_net_wm_icon(Display* dpy, Window w) {
  Atom net_wm_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
  unsigned long total = 0;
  if (!icon_property_total(dpy, w, net_wm_icon, total) || total < 2) return {};
  // One hostile property must not walk an unbounded CARD32 space.
  constexpr unsigned long kMaxIconWords = 4ul * 1024ul * 1024ul;
  if (total > kMaxIconWords) total = kMaxIconWords;

  IconFetch fetch;
  fetch.dpy = dpy;
  fetch.window = w;
  fetch.prop = net_wm_icon;
  const auto choice = choose_net_wm_icon_chunked(total, fetch_icon_word, &fetch);
  if (!choice || choice->width == 0 || choice->height == 0 ||
      choice->width > kIconSelectMaxEdge || choice->height > kIconSelectMaxEdge ||
      choice->width >= 512 || choice->height >= 512) {
    return {};
  }
  const unsigned long pixels = choice->width * choice->height;
  if (pixels == 0 || pixels > kNetWmIconMaxItems) return {};
  std::vector<unsigned long> argb;
  if (!read_cardinal_words(dpy, w, net_wm_icon, choice->pixel_offset, pixels, argb, nullptr)) {
    return {};
  }
  return pixbuf_from_argb(argb, choice->width, choice->height);
}

struct IconStamp {
  bool present = false;
  unsigned long long fingerprint = 1;
};

IconStamp stamp_net_wm_icon(Display* dpy, Window w) {
  IconStamp stamp;
  Atom prop = XInternAtom(dpy, "_NET_WM_ICON", False);
  unsigned long total = 0;
  if (!icon_property_total(dpy, w, prop, total) || total == 0) return stamp;
  stamp.present = true;
  unsigned long long hash = 14695981039346656037ull;
  hash = fnv_mix(hash, total);
  auto mix = [&](unsigned long offset, unsigned long count) {
    std::vector<unsigned long> words;
    if (!read_cardinal_words(dpy, w, prop, offset, count, words, nullptr)) return;
    for (unsigned long word : words) hash = fnv_mix(hash, word);
  };
  const unsigned long head = std::min(total, 32ul);
  mix(0, head);
  if (total > 32) {
    const unsigned long tail = std::min(total, 32ul);
    mix(total - tail, tail);
  }
  stamp.fingerprint = hash;
  return stamp;
}

Glib::RefPtr<Gdk::Pixbuf> cached_icon(Display* dpy, unsigned long xid, pid_t pid) {
  const IconStamp stamp = stamp_net_wm_icon(dpy, static_cast<Window>(xid));
  auto& slot = icon_cache()[xid];
  if (slot.known && slot.pid == pid && slot.present == stamp.present &&
      slot.fingerprint == stamp.fingerprint) {
    return slot.pixbuf;
  }
  slot.known = true;
  slot.pid = pid;
  slot.present = stamp.present;
  slot.fingerprint = stamp.fingerprint;
  slot.pixbuf = stamp.present ? decode_net_wm_icon(dpy, static_cast<Window>(xid)) : Glib::RefPtr<Gdk::Pixbuf>{};
  return slot.pixbuf;
}

bool net_wm_icon_present(Display* dpy, Window w) {
  Atom net_wm_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
  unsigned long total = 0;
  return icon_property_total(dpy, w, net_wm_icon, total) && total > 0;
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
    const unsigned long copy = x_property_utf8_copy_bytes(
        actual_format, actual_type == utf8, nitems, property_alloc_nbytes(actual_format, nitems));
    if (copy > 0) {
      std::string s(reinterpret_cast<char*>(data), static_cast<size_t>(copy));
      XFree(data);
      return s;
    }
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
      data &&
      x_property_indexable_as_longs(actual_format, actual_type == XA_CARDINAL, nitems,
                                    property_alloc_nbytes(actual_format, nitems))) {
    const unsigned long raw = *reinterpret_cast<unsigned long*>(data);
    if (raw > 1 && raw <= static_cast<unsigned long>(INT_MAX)) {
      pid = static_cast<pid_t>(raw);
    }
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
      data &&
      x_property_indexable_as_longs(actual_format, actual_type == XA_WINDOW, nitems,
                                    property_alloc_nbytes(actual_format, nitems))) {
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
      data &&
      x_property_indexable_as_longs(actual_format, actual_type == XA_ATOM, nitems,
                                    property_alloc_nbytes(actual_format, nitems))) {
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
      data &&
      x_property_indexable_as_longs(actual_format, actual_type == XA_ATOM, nitems,
                                    property_alloc_nbytes(actual_format, nitems))) {
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
  void begin_stacking_or_finish(bool allow_tree);
  void finish_collect(bool allow_tree);
  void query_tree();
  void walk_clients(Window window, int depth);
  void inspect_one();
  void read_one_proc();
  void assemble();
  void load_one_icon();

  pid_t self_ = 0;
  bool display_ready_ = false;
  bool x11_ = false;
  bool tree_tried_ = false;
  bool reading_stacking_ = false;
  bool stacking_tried_ = false;
  GdkDisplay* gdk_ = nullptr;
  Display* dpy_ = nullptr;
  Window root_ = None;
  Window active_ = None;
  Phase phase_ = Phase::Collect;
  long client_offset_ = 0;
  std::vector<Window> clients_;
  size_t index_ = 0;
  std::vector<WindowFact> facts_;
  std::unordered_map<pid_t, ProcSnapshot> comms_;
  std::vector<pid_t> proc_ids_;
  bool procs_listed_ = false;
  std::unordered_map<pid_t, long> rss_kb_;
  std::unordered_map<pid_t, unsigned long long> start_ticks_;
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

void AppListRefresh::Impl::walk_clients(Window window, int depth) {
  if (depth > kMaxWmTreeDepth) return;
  if (static_cast<long>(clients_.size()) >= kMaxClientIds) return;
  Window rr = None, parent = None;
  Window* children = nullptr;
  unsigned int nchildren = 0;
  if (!XQueryTree(dpy_, window, &rr, &parent, &children, &nchildren)) return;
  for (unsigned int i = 0; i < nchildren; ++i) {
    if (static_cast<long>(clients_.size()) >= kMaxClientIds) break;
    const Window child = children[i];
    if (has_wm_state(dpy_, child)) {
      clients_.push_back(child);
      continue;
    }
    if (depth < kMaxWmTreeDepth) walk_clients(child, depth + 1);
  }
  if (children) XFree(children);
}

void AppListRefresh::Impl::query_tree() {
  tree_tried_ = true;
  walk_clients(root_, 0);
}

static bool client_page_usable(int rc, Atom actual_type, int actual_format, unsigned long nitems,
                               unsigned char* data) {
  if (rc != Success || actual_format != 32 || actual_type != XA_WINDOW) return false;
  if (nitems == 0) return true;
  return data && x_property_indexable_as_longs(actual_format, true, nitems,
                                               property_alloc_nbytes(actual_format, nitems));
}

void AppListRefresh::Impl::begin_stacking_or_finish(bool allow_tree) {
  if (!reading_stacking_ && !stacking_tried_ && clients_.empty()) {
    reading_stacking_ = true;
    stacking_tried_ = true;
    client_offset_ = 0;
    return;
  }
  finish_collect(allow_tree && clients_.empty());
}

void AppListRefresh::Impl::collect_page() {
  const char* atom_name =
      reading_stacking_ ? "_NET_CLIENT_LIST_STACKING" : "_NET_CLIENT_LIST";
  Atom client_list = XInternAtom(dpy_, atom_name, False);
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  const int rc =
      XGetWindowProperty(dpy_, root_, client_list, client_offset_, 1024, False, XA_WINDOW,
                         &actual_type, &actual_format, &nitems, &bytes_after, &data);
  if (!client_page_usable(rc, actual_type, actual_format, nitems, data)) {
    if (data) XFree(data);
    begin_stacking_or_finish(true);
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
    begin_stacking_or_finish(true);
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
    ProcSnapshot id;
    if (it == comms_.end()) {
      id = read_proc_snapshot(pid);
      comms_.emplace(pid, id);
    } else {
      id = it->second;
    }
    if (id.ok) {
      fact.comm = id.comm;
      fact.start_ticks = id.start_ticks;
      fact.identity_ok = true;
    }
  }
  if (fact.kind == WindowKind::Normal) fact.has_icon = net_wm_icon_present(dpy_, w);
  facts_.push_back(std::move(fact));
}

void AppListRefresh::Impl::read_one_proc() {
  const pid_t pid = proc_ids_[index_++];
  const int dirfd = open_proc_pid_dir(pid);
  if (dirfd < 0) {
    rss_kb_.erase(pid);
    start_ticks_.erase(pid);
    return;
  }
  const ProcSnapshot before = read_proc_snapshot_at(dirfd);
  pid_t ppid = 0;
  long rss = 0;
  bool saw_status = false;
  const int status_fd = ::openat(dirfd, "status", O_RDONLY | O_CLOEXEC);
  if (status_fd >= 0) {
    const std::string text = read_fd_limited(status_fd);
    ::close(status_fd);
    saw_status = true;
    std::string line;
    for (size_t i = 0; i <= text.size(); ++i) {
      if (i == text.size() || text[i] == '\n') {
        if (line.compare(0, 5, "PPid:") == 0) {
          ppid = static_cast<pid_t>(parse_status_number(line));
        } else if (line.compare(0, 8, "RssAnon:") == 0) {
          rss = parse_status_number(line);
        }
        line.clear();
      } else {
        line.push_back(text[i]);
      }
    }
  }
  SmapsRollup rollup;
  bool have_rollup = false;
  const int rollup_fd = ::openat(dirfd, "smaps_rollup", O_RDONLY | O_CLOEXEC);
  if (rollup_fd >= 0) {
    const std::string rollup_text = read_fd_limited(rollup_fd);
    ::close(rollup_fd);
    rollup = parse_smaps_rollup(rollup_text);
    have_rollup = true;
  }
  const ProcSnapshot after = read_proc_snapshot_at(dirfd);
  ::close(dirfd);
  // The pid was recycled between the two stat reads, or status never opened.
  // Drop it so the row shows an em dash instead of the new process's RAM.
  if (!saw_status || !before.ok || !after.ok || before.comm != after.comm ||
      before.start_ticks != after.start_ticks) {
    rss_kb_.erase(pid);
    start_ticks_.erase(pid);
    return;
  }
  // Pss_Anon counts a shared anonymous page once across this process and the
  // helpers rolled into the same row. RssAnon repeats those pages.
  rss = process_anon_charge_kb(have_rollup, rollup, rss);
  rss_kb_[pid] = rss;
  start_ticks_[pid] = before.start_ticks;
  if (ppid > 0 && ppid != pid) children_[ppid].push_back(pid);
}

void AppListRefresh::Impl::assemble() {
  const std::vector<GroupedApp> groups = group_windows(facts_);
  std::unordered_set<pid_t> row_pids;
  for (const auto& g : groups) {
    if (g.has_pid) row_pids.insert(g.pid);
  }
  std::unordered_map<pid_t, bool> toplevel_pid;
  std::unordered_map<unsigned long, bool> toplevel_xid;
  for (const auto& fact : facts_) {
    if (!is_toplevel_kind(fact.kind)) continue;
    if (fact.has_pid && fact.pid > 1) toplevel_pid[fact.pid] = true;
    else toplevel_xid[fact.xid] = true;
  }
  std::unordered_map<pid_t, std::string> cmdlines;
  entries_.clear();
  entries_.reserve(groups.size());
  for (const auto& g : groups) {
    // Session plumbing stays out of the list. Its pid remains in row_pids
    // so a parent application does not absorb that memory; nothing subtracts
    // it, and LCOS System keeps it.
    ProcessView view;
    view.comm = g.comm;
    view.wm_res_class = g.class_name;
    view.owns_toplevel_window = false;
    if (g.has_pid && g.pid > 1) {
      view.owns_toplevel_window = toplevel_pid[g.pid];
      const auto have = cmdlines.find(g.pid);
      if (have == cmdlines.end()) {
        view.cmdline = read_proc_cmdline(g.pid);
        cmdlines.emplace(g.pid, view.cmdline);
      } else {
        view.cmdline = have->second;
      }
    } else {
      view.owns_toplevel_window = toplevel_xid[g.xid];
    }
    if (classify_process(view) == ProcessClass::System) continue;

    AppEntry entry;
    entry.name = g.name;
    entry.tooltip = g.tooltip;
    entry.pid = g.has_pid ? g.pid : 0;
    entry.xid = g.xid;
    entry.protected_app = g.protected_app;
    entry.protect_reason = g.protect_reason;
    entry.comm = g.comm;
    entry.class_name = g.class_name;
    entry.distinguish = g.distinguish;
    entry.start_ticks = g.start_ticks;
    entry.identity_ok = g.identity_ok;
    if (g.has_pid && g.identity_ok) {
      const auto start = start_ticks_.find(g.pid);
      if (start == start_ticks_.end() || start->second != g.start_ticks) {
        // Inspect and the proc slice disagree: the pid was reused.
        entry.identity_ok = false;
        entry.rss_known = false;
        entry.rss_kb = 0;
      } else {
        entry.kill_pins =
            collect_kill_pins(g.pid, rss_kb_, start_ticks_, children_, row_pids);
        const long long sum = sum_rss_once(entry.kill_pins);
        if (sum > LONG_MAX) entry.rss_kb = LONG_MAX;
        else if (sum < LONG_MIN) entry.rss_kb = LONG_MIN;
        else entry.rss_kb = static_cast<long>(sum);
        entry.rss_known = true;
      }
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
  entry.icon = cached_icon(dpy_, entry.xid, entry.pid);
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
