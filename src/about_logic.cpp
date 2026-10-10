// SPDX-License-Identifier: GPL-3.0-or-later
#include "about_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <istream>
#include <sstream>

namespace lundukeabout {
namespace {

bool equals_id(const std::string& field, const char* needle) {
  if (!needle || field.empty()) return false;
  const size_t n = std::strlen(needle);
  if (field.size() != n) return false;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char a = static_cast<unsigned char>(field[i]);
    const unsigned char b = static_cast<unsigned char>(needle[i]);
    if (std::tolower(a) != std::tolower(b)) return false;
  }
  return true;
}

// /proc/<pid>/comm is at most 15 bytes. A longer binary still matches when
// those 15 bytes are its prefix; anything shorter or different does not.
bool equals_comm(const std::string& comm, const char* needle) {
  if (comm.empty() || !needle) return false;
  if (equals_id(comm, needle)) return true;
  const size_t n = std::strlen(needle);
  if (n <= 15 || comm.size() != 15) return false;
  for (size_t i = 0; i < 15; ++i) {
    const unsigned char a = static_cast<unsigned char>(comm[i]);
    const unsigned char b = static_cast<unsigned char>(needle[i]);
    if (std::tolower(a) != std::tolower(b)) return false;
  }
  return true;
}

struct ProtectRule {
  const char* id;
  const char* reason;
};

const ProtectRule kProtectRules[] = {
    {"lunduke-about", "This application"},
    {"about-this-computer", "This application"},
    {"xfce4-panel", "Desktop panel"},
    {"xfce4-session", "Session manager"},
    {"xfwm4", "Window manager"},
    {"xfdesktop", "Desktop"},
    {"xfsettingsd", "Settings daemon"},
    {"xfconfd", "Settings daemon"},
    {"polkit", "System service"},
    {"polkit-gnome-authentication-agent-1", "System service"},
    {"xfce4-notifyd", "Notifications"},
    {"xfce4-screensaver", "Screensaver"},
};

// Session plumbing versus an application. One table decides the class.
//
// A name matches when /proc comm or argv0's basename equals `id`, or when
// comm is the kernel's 15-byte truncation of a longer `id`. `hyphen_prefix`
// also matches `id` + '-' + more (lightdm-gtk-greeter, gvfsd-trash,
// pipewire-pulse, xdg-desktop-portal-gtk). Titles are not names.
//
// Anything absent from this table is an application. That includes
// xfce4-terminal, mousepad (Edit), brave, and every lunduke-* program.
// xfce4-terminal must not inherit a prefix of xfce4-panel.
//
// Thunar is not in this table. It is system only when the command line
// has `--daemon` as its own argument and the process owns no top-level
// window. A Thunar window stays an application, daemon flag or not.
//
// Panel plugins and the libxfce4panel wrapper are matched separately:
//   panel-*-plugin
//   panel-<digits>-<name>   (the 15-byte comm of a longer plugin name)
//   wrapper-2.0
//   an argument whose basename is wrapper-2.0 or starts with libxfce4panel
const struct SystemName {
  const char* id;
  bool hyphen_prefix;
  const char* covers;
} kSystemNames[] = {
    {"xfce4-panel", false, "panel"},
    {"xfdesktop", false, "desktop"},
    {"xfwm4", false, "window manager"},
    {"xfce4-session", false, "session manager"},
    {"xfsettingsd", false, "settings daemon"},
    {"xfconfd", false, "settings daemon"},
    {"xfce4-notifyd", false, "notifications"},
    {"lightdm", true, "display manager and greeters"},
    {"at-spi-bus-launcher", false, "at-spi bus"},
    {"at-spi2-registryd", false, "at-spi registry"},
    {"at-spi-registryd", false, "at-spi registry"},
    {"polkit-gnome-authentication-agent-1", false, "polkit agent"},
    {"xfce-polkit", false, "polkit agent"},
    {"lxpolkit", false, "polkit agent"},
    {"xfce4-power-manager", false, "power manager"},
    {"xfce4-screensaver", false, "screensaver"},
    {"light-locker", false, "locker"},
    {"xiccd", false, "color daemon"},
    {"gvfsd", true, "gvfs daemon"},
    {"gvfs", true, "gvfs monitor"},
    {"dbus-daemon", false, "session bus"},
    {"dbus-broker", true, "session bus"},
    {"pulseaudio", false, "audio"},
    {"pipewire", true, "audio, including pipewire-pulse"},
    {"wireplumber", false, "audio session"},
    {"nm-applet", false, "network applet"},
    {"blueman-applet", false, "bluetooth applet"},
    {"xdg-desktop-portal", true, "desktop portals"},
};

bool istarts_with(const std::string& text, const char* prefix) {
  if (!prefix) return false;
  const size_t n = std::strlen(prefix);
  if (text.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char a = static_cast<unsigned char>(text[i]);
    const unsigned char b = static_cast<unsigned char>(prefix[i]);
    if (std::tolower(a) != std::tolower(b)) return false;
  }
  return true;
}

bool iends_with(const std::string& text, const char* suffix) {
  if (!suffix) return false;
  const size_t n = std::strlen(suffix);
  if (text.size() < n) return false;
  const size_t off = text.size() - n;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char a = static_cast<unsigned char>(text[off + i]);
    const unsigned char b = static_cast<unsigned char>(suffix[i]);
    if (std::tolower(a) != std::tolower(b)) return false;
  }
  return true;
}

bool hyphen_continuation(const std::string& text, const char* id) {
  if (!id) return false;
  const size_t n = std::strlen(id);
  if (text.size() <= n || text[n] != '-') return false;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char a = static_cast<unsigned char>(text[i]);
    const unsigned char b = static_cast<unsigned char>(id[i]);
    if (std::tolower(a) != std::tolower(b)) return false;
  }
  return true;
}

bool matches_system_name(const std::string& name) {
  if (name.empty()) return false;
  for (const auto& rule : kSystemNames) {
    if (equals_id(name, rule.id) || equals_comm(name, rule.id)) return true;
    if (rule.hyphen_prefix && hyphen_continuation(name, rule.id)) return true;
  }
  return false;
}

// panel-*-plugin, and panel-<digits>-<rest> so a 15-byte comm still hits.
bool is_panel_plugin_name(const std::string& name) {
  if (name.empty()) return false;
  const size_t panel = std::strlen("panel-");
  const size_t plugin = std::strlen("-plugin");
  if (istarts_with(name, "panel-") && iends_with(name, "-plugin") &&
      name.size() > panel + plugin) {
    return true;
  }
  if (!istarts_with(name, "panel-") || name.size() <= panel) return false;
  size_t i = panel;
  if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
  while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) ++i;
  return i + 1 < name.size() && name[i] == '-';
}

std::string path_basename(const std::string& path) {
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos) return path;
  return path.substr(slash + 1);
}

std::vector<std::string> command_arguments(const std::string& cmdline) {
  std::vector<std::string> args;
  if (cmdline.empty()) return args;
  const bool nul = cmdline.find('\0') != std::string::npos;
  std::string cur;
  auto flush = [&]() {
    if (!cur.empty()) args.push_back(cur);
    cur.clear();
  };
  for (unsigned char c : cmdline) {
    if (c == '\0' || (!nul && (c == ' ' || c == '\t' || c == '\n'))) {
      flush();
      continue;
    }
    cur.push_back(static_cast<char>(c));
  }
  flush();
  return args;
}

bool is_thunar_name(const std::string& name) { return equals_id(name, "thunar"); }

bool is_panel_wrapper_arg(const std::string& arg) {
  const std::string base = path_basename(arg);
  if (equals_id(base, "wrapper-2.0")) return true;
  if (istarts_with(base, "libxfce4panel")) return true;
  return false;
}

bool name_is_session_plumbing(const std::string& name) {
  if (name.empty()) return false;
  if (matches_system_name(name)) return true;
  if (is_panel_plugin_name(name)) return true;
  if (equals_id(name, "wrapper-2.0")) return true;
  return false;
}

int representative_score(const WindowFact& w) {
  int score = 0;
  if (w.active) score += 100;
  if (w.has_title && w.has_icon) score += 10;
  else if (w.has_title) score += 4;
  else if (w.has_icon) score += 2;
  return score;
}

bool is_3d_controller(unsigned class_code) {
  return ((class_code >> 8) & 0xFFFFu) == 0x0302u;
}

bool is_vga_controller(unsigned class_code) {
  return ((class_code >> 8) & 0xFFFFu) == 0x0300u;
}

std::string trim_cr(std::string s) {
  if (!s.empty() && s.back() == '\r') s.pop_back();
  return s;
}

bool parse_pci_token(const std::string& line, size_t start, unsigned& id,
                     size_t& name_at) {
  if (line.size() < start + 4) return false;
  for (size_t i = 0; i < 4; ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(line[start + i]))) return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(line.c_str() + start, &end, 16);
  if (end != line.c_str() + start + 4) return false;
  id = static_cast<unsigned>(parsed);
  size_t i = start + 4;
  if (i >= line.size() || (line[i] != ' ' && line[i] != '\t')) return false;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  name_at = i;
  return true;
}

}  // namespace

namespace {

struct IconArray {
  const unsigned long* data = nullptr;
  unsigned long nitems = 0;
};

bool read_icon_array(void* ctx, unsigned long index, unsigned long& word) {
  const auto* array = static_cast<const IconArray*>(ctx);
  if (!array || !array->data || index >= array->nitems) return false;
  word = array->data[index];
  return true;
}

}  // namespace

std::optional<IconChoice> choose_net_wm_icon_chunked(unsigned long total_items,
                                                    IconWordReader read_word,
                                                    void* ctx) {
  if (!read_word || total_items < 2) return std::nullopt;

  IconChoice best;
  bool have = false;
  unsigned long off = 0;
  unsigned long steps = 0;
  while (off < total_items) {
    // A hostile width/height used to wrap the step to 0 and spin forever.
    if (++steps > 1000000ul) break;
    if (total_items - off < 2) break;
    unsigned long iw = 0;
    unsigned long ih = 0;
    // Header missing from this chunk: stop. Do not invent the next frame.
    if (!read_word(ctx, off, iw) || !read_word(ctx, off + 1, ih)) break;
    if (iw == 0 || ih == 0) break;
    if (iw > total_items || ih > total_items) break;
    if (ih != 0 && iw > ULONG_MAX / ih) break;
    const unsigned long pixels = iw * ih;
    // product + 2 must itself fit and move the cursor forward.
    if (pixels > ULONG_MAX - 2) break;
    const unsigned long step = pixels + 2;
    if (step <= 2) break;
    if (off > ULONG_MAX - step) break;
    // Declared frame does not fit in the property. Stop; do not wrap.
    if (off + step > total_items) break;

    // Edges above the select cap are skipped, not decoded. The cursor still
    // advances by the declared step so a later 32px frame in the property
    // (or in a following chunk) can be chosen. Pixels of a skipped or
    // not-yet-fetched frame do not have to be readable.
    if (iw <= kIconSelectMaxEdge && ih <= kIconSelectMaxEdge && iw < 512 && ih < 512) {
      const long dist = std::labs(static_cast<long>(iw) - 32L);
      const long best_dist =
          have ? std::labs(static_cast<long>(best.width) - 32L) : LONG_MAX;
      if (!have || dist < best_dist) {
        best.width = iw;
        best.height = ih;
        best.pixel_offset = off + 2;
        have = true;
      }
    }
    off += step;
  }
  if (!have) return std::nullopt;
  return best;
}

std::optional<IconChoice> choose_net_wm_icon(const unsigned long* icons,
                                            unsigned long nitems) {
  if (!icons || nitems < 2) return std::nullopt;
  IconArray array{icons, nitems};
  return choose_net_wm_icon_chunked(nitems, read_icon_array, &array);
}

bool client_list_advance(long offset, unsigned long nitems,
                         unsigned long bytes_after, long max_ids,
                         long& next_offset) {
  next_offset = offset;
  // nitems == 0 does not advance, including when bytes_after > 0. An empty
  // _NET_CLIENT_LIST page is "no clients yet", not a request for the next page.
  if (nitems == 0 || bytes_after == 0) return false;
  if (offset < 0 || max_ids < 1) return false;
  if (nitems > static_cast<unsigned long>(max_ids)) return false;
  if (offset > max_ids - static_cast<long>(nitems)) return false;
  next_offset = offset + static_cast<long>(nitems);
  if (next_offset >= max_ids) return false;
  return true;
}

bool x_property_indexable_as_longs(int format, bool type_matches,
                                   unsigned long nitems, unsigned long nbytes) {
  if (format != 32 || !type_matches || nitems == 0) return false;
  if (sizeof(unsigned long) == 0) return false;
  if (nitems > ULONG_MAX / sizeof(unsigned long)) return false;
  const unsigned long need = nitems * sizeof(unsigned long);
  return nbytes >= need;
}

unsigned long x_property_utf8_copy_bytes(int format, bool type_matches,
                                         unsigned long nitems, unsigned long nbytes) {
  if (format != 8 || !type_matches || nitems == 0 || nbytes == 0) return 0;
  return nitems < nbytes ? nitems : nbytes;
}

long rollup_rss_anon(pid_t root,
                     const std::unordered_map<pid_t, long>& rss_kb,
                     const std::unordered_map<pid_t, std::vector<pid_t>>& children,
                     const std::unordered_set<pid_t>& row_pids) {
  long long total = 0;
  std::vector<pid_t> stack;
  stack.push_back(root);
  std::unordered_set<pid_t> seen;
  while (!stack.empty()) {
    const pid_t pid = stack.back();
    stack.pop_back();
    if (!seen.insert(pid).second) continue;
    const auto rss = rss_kb.find(pid);
    if (rss != rss_kb.end()) total += rss->second;
    const auto kids = children.find(pid);
    if (kids == children.end()) continue;
    for (pid_t child : kids->second) {
      if (child != root && row_pids.find(child) != row_pids.end()) continue;
      stack.push_back(child);
    }
  }
  if (total > LONG_MAX) return LONG_MAX;
  if (total < LONG_MIN) return LONG_MIN;
  return static_cast<long>(total);
}

ProcSnapshot parse_proc_stat_line(const std::string& stat_line) {
  ProcSnapshot id;
  const auto lparen = stat_line.find('(');
  const auto rparen = stat_line.rfind(')');
  if (lparen == std::string::npos || rparen == std::string::npos || rparen <= lparen) {
    return id;
  }
  id.comm = stat_line.substr(lparen + 1, rparen - lparen - 1);
  std::istringstream iss(stat_line.substr(rparen + 1));
  std::vector<std::string> tokens;
  std::string tok;
  while (iss >> tok) tokens.push_back(tok);
  // Field 22 is the 20th token after comm (fields 3..22).
  if (tokens.size() < 20) return id;
  try {
    id.start_ticks = std::stoull(tokens[19]);
  } catch (...) {
    return id;
  }
  id.ok = true;
  if (tokens.size() > 1) {
    try {
      id.ppid = static_cast<pid_t>(std::stol(tokens[1]));
    } catch (...) {
      id.ppid = 0;
    }
  }
  if (tokens.size() > 21) {
    try {
      id.rss_pages = std::stol(tokens[21]);
      id.saw_rss = true;
    } catch (...) {
      id.saw_rss = false;
      id.rss_pages = 0;
    }
  }
  return id;
}

bool proc_identity_matches(const ProcSnapshot& pinned, const ProcSnapshot& now) {
  if (!pinned.ok || !now.ok) return false;
  return pinned.comm == now.comm && pinned.start_ticks == now.start_ticks;
}

bool may_signal_pinned_pid(pid_t pid, pid_t self_pid,
                           unsigned long long expected_start,
                           bool reread_ok, unsigned long long reread_start,
                           bool belongs_to_other_row) {
  if (pid <= 1 || pid == self_pid) return false;
  if (belongs_to_other_row) return false;
  if (!reread_ok) return false;
  return reread_start == expected_start;
}

std::vector<ProcPin> collect_kill_pins(
    pid_t root,
    const std::unordered_map<pid_t, long>& rss_kb,
    const std::unordered_map<pid_t, unsigned long long>& start_ticks,
    const std::unordered_map<pid_t, std::vector<pid_t>>& children,
    const std::unordered_set<pid_t>& row_pids) {
  std::vector<ProcPin> pins;
  if (root <= 1) return pins;
  std::vector<pid_t> stack;
  stack.push_back(root);
  std::unordered_set<pid_t> seen;
  while (!stack.empty()) {
    const pid_t pid = stack.back();
    stack.pop_back();
    if (!seen.insert(pid).second) continue;
    if (pid != root && row_pids.find(pid) != row_pids.end()) continue;
    const auto start = start_ticks.find(pid);
    if (start != start_ticks.end()) {
      ProcPin pin;
      pin.pid = pid;
      pin.start_ticks = start->second;
      const auto rss = rss_kb.find(pid);
      pin.rss_kb = rss == rss_kb.end() ? 0 : rss->second;
      pins.push_back(pin);
    }
    const auto kids = children.find(pid);
    if (kids == children.end()) continue;
    for (pid_t child : kids->second) {
      if (child != root && row_pids.find(child) != row_pids.end()) continue;
      stack.push_back(child);
    }
  }
  return pins;
}

long long sum_rss_once(const std::vector<ProcPin>& pins) {
  long long total = 0;
  std::unordered_set<pid_t> seen;
  for (const auto& pin : pins) {
    if (!seen.insert(pin.pid).second) continue;
    total += pin.rss_kb;
  }
  return total;
}

SystemRemainder system_remainder_kb(long used_kb, long long apps_rss) {
  SystemRemainder out;
  out.raw_kb = static_cast<long long>(used_kb) - apps_rss;
  if (out.raw_kb < 0) {
    out.shown_kb = 0;
    out.clamped = true;
    return out;
  }
  if (out.raw_kb > static_cast<long long>(LONG_MAX)) out.shown_kb = LONG_MAX;
  else out.shown_kb = static_cast<long>(out.raw_kb);
  return out;
}

bool is_protected_identity(const ClassHint& wm, const std::string& comm,
                           std::string& reason) {
  for (const auto& rule : kProtectRules) {
    if (equals_id(wm.res_name, rule.id) || equals_id(wm.res_class, rule.id) ||
        equals_comm(comm, rule.id)) {
      reason = rule.reason;
      return true;
    }
  }
  reason.clear();
  return false;
}

bool window_marks_protected(const WindowFact& window, std::string& reason) {
  reason.clear();
  if (!window.has_pid || window.comm.empty()) return false;
  std::string class_reason;
  const bool class_hit = is_protected_identity(
      ClassHint{window.res_name, window.res_class}, "", class_reason);
  std::string comm_reason;
  const bool comm_hit = is_protected_identity(ClassHint{}, window.comm, comm_reason);
  // A protected class on a process whose comm is not a protected binary
  // must not hide Force Close for that pid.
  if (class_hit && !comm_hit) return false;
  if (!comm_hit) return false;
  reason = comm_reason;
  return true;
}

ProcessClass classify_process(const ProcessView& process) {
  const std::vector<std::string> args = command_arguments(process.cmdline);
  std::vector<std::string> names;
  if (!process.comm.empty()) names.push_back(process.comm);
  if (!args.empty()) names.push_back(path_basename(args[0]));
  // A class is a name only when the kernel didn't give us a comm. A browser
  // whose class happens to say xfce4-panel is still a browser.
  if (process.comm.empty()) {
    if (!process.wm_res_name.empty()) names.push_back(process.wm_res_name);
    if (!process.wm_res_class.empty()) names.push_back(process.wm_res_class);
  }

  bool thunar = false;
  for (const auto& name : names) {
    if (is_thunar_name(name)) thunar = true;
  }
  if (thunar) {
    bool daemon = false;
    for (const auto& arg : args) {
      if (arg == "--daemon") daemon = true;
    }
    // Hidden only for the daemon with no window of its own.
    if (daemon && !process.owns_toplevel_window) return ProcessClass::System;
    return ProcessClass::App;
  }

  for (const auto& name : names) {
    if (name_is_session_plumbing(name)) return ProcessClass::System;
  }
  for (const auto& arg : args) {
    if (is_panel_wrapper_arg(arg)) return ProcessClass::System;
  }
  return ProcessClass::App;
}

SessionRamSplit split_session_ram(long used_kb, const std::vector<SessionRamSample>& samples) {
  SessionRamSplit out;
  for (const auto& sample : samples) {
    if (classify_process(sample.process) == ProcessClass::System) out.system_kb += sample.rss_kb;
    else out.app_kb += sample.rss_kb;
  }
  // Session RSS is not part of app_kb, so the remainder keeps it.
  out.lcos = system_remainder_kb(used_kb, out.app_kb);
  return out;
}

namespace {

bool row_is_lcos_system(const OrderedRow& row) {
  return row.lcos_system || row.name == "LCOS System";
}

int compare_ci(const std::string& a, const std::string& b) {
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  for (size_t i = 0; i < n; ++i) {
    const unsigned char ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
    const unsigned char cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
    if (ca < cb) return -1;
    if (ca > cb) return 1;
  }
  if (a.size() < b.size()) return -1;
  if (a.size() > b.size()) return 1;
  return 0;
}

bool tie_before(const OrderedRow& a, size_t ai, const OrderedRow& b, size_t bi) {
  if (a.pid != b.pid) return a.pid < b.pid;
  if (a.xid != b.xid) return a.xid < b.xid;
  return ai < bi;
}

}  // namespace

std::vector<size_t> order_app_row_indices(const std::vector<OrderedRow>& rows,
                                          AppSortColumn column,
                                          AppSortDirection direction) {
  std::vector<size_t> apps;
  std::vector<size_t> system;
  apps.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    if (row_is_lcos_system(rows[i])) system.push_back(i);
    else apps.push_back(i);
  }
  const bool ascending = direction == AppSortDirection::Ascending;
  std::stable_sort(apps.begin(), apps.end(), [&](size_t ai, size_t bi) {
    const OrderedRow& a = rows[ai];
    const OrderedRow& b = rows[bi];
    if (column == AppSortColumn::Name) {
      const int cmp = compare_ci(a.name, b.name);
      if (cmp != 0) return ascending ? cmp < 0 : cmp > 0;
      return tie_before(a, ai, b, bi);
    }
    if (a.rss_known != b.rss_known) {
      // Unknown is below every known value: first when ascending, last
      // among applications when descending.
      if (ascending) return !a.rss_known && b.rss_known;
      return a.rss_known && !b.rss_known;
    }
    if (a.rss_known && a.rss_kb != b.rss_kb) {
      return ascending ? a.rss_kb < b.rss_kb : a.rss_kb > b.rss_kb;
    }
    return tie_before(a, ai, b, bi);
  });
  apps.insert(apps.end(), system.begin(), system.end());
  return apps;
}

namespace {

bool lists_without_normal(WindowKind kind) {
  return kind == WindowKind::DesktopOrDock || kind == WindowKind::SkipTaskbar ||
         kind == WindowKind::Utility;
}

std::string distinct_title(const GroupedApp& row) {
  if (!utf8_valid(row.tooltip) || row.tooltip.empty() || row.tooltip == row.name) return {};
  return row.tooltip;
}

void disambiguate_row_names(std::vector<GroupedApp>& rows) {
  std::unordered_map<std::string, int> name_count;
  for (const auto& row : rows) name_count[row.name]++;

  std::unordered_map<std::string, int> title_count;
  for (const auto& row : rows) {
    if (name_count[row.name] < 2) continue;
    title_count[row.name + "\n" + distinct_title(row)]++;
  }

  for (auto& row : rows) {
    if (name_count[row.name] < 2) continue;
    const std::string title = distinct_title(row);
    const bool title_unique = !title.empty() && title_count[row.name + "\n" + title] == 1;
    if (row.has_pid && row.pid > 1) row.distinguish = "pid " + std::to_string(row.pid);
    else row.distinguish = "window " + std::to_string(row.xid);
    // The menu and the confirm dialog use `name`. The row paints `distinguish`
    // in a separate column because an end ellipsis would hide a suffix.
    if (title_unique) {
      row.name += " \u2014 " + title;
      continue;
    }
    row.name += " (" + row.distinguish + ")";
  }
}

}  // namespace

std::string painted_row_name(const std::string& name, const std::string& distinguish) {
  if (distinguish.empty()) return name;
  const std::string suffix = " (" + distinguish + ")";
  if (name.size() >= suffix.size() &&
      name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return name.substr(0, name.size() - suffix.size());
  }
  return name;
}

namespace {

std::string ascii_fold(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (unsigned char c : text) out.push_back(static_cast<char>(std::tolower(c)));
  return out;
}

std::string trim_desktop_value(const std::string& text) {
  size_t begin = 0;
  while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
  size_t end = text.size();
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::string desktop_id_stem(const std::string& id) {
  std::string stem = id;
  const auto slash = stem.find_last_of('/');
  if (slash != std::string::npos) stem = stem.substr(slash + 1);
  const std::string suffix = ".desktop";
  if (stem.size() >= suffix.size() &&
      stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0) {
    stem.resize(stem.size() - suffix.size());
  }
  return stem;
}

bool desktop_id_matches(const std::string& id, const std::string& key) {
  if (id.empty() || key.empty()) return false;
  const std::string folded_key = ascii_fold(key);
  if (ascii_fold(id) == folded_key) return true;
  if (ascii_fold(id) == folded_key + ".desktop") return true;
  return ascii_fold(desktop_id_stem(id)) == folded_key;
}

int desktop_match_score(const DesktopAppRecord& app, const std::string& res_name,
                        const std::string& res_class, const std::string& comm) {
  const std::string wm = app.startup_wm_class;
  if (!wm.empty()) {
    if (!res_class.empty() && wm == res_class) return 80;
    if (!res_class.empty() && ascii_fold(wm) == ascii_fold(res_class)) return 70;
    if (!res_name.empty() && wm == res_name) return 60;
    if (!res_name.empty() && ascii_fold(wm) == ascii_fold(res_name)) return 50;
  }
  if (desktop_id_matches(app.id, res_name)) return 40;
  if (desktop_id_matches(app.id, res_class)) return 30;
  if (desktop_id_matches(app.id, comm)) return 20;
  return 0;
}

}  // namespace

DesktopAppRecord parse_desktop_entry(const std::string& text, const std::string& id) {
  DesktopAppRecord rec;
  rec.id = id;
  bool in_entry = false;
  bool seen_group = false;
  std::string localized;
  std::string line;
  auto take = [&]() {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') return;
    if (line[0] == '[') {
      const auto end = line.find(']');
      const std::string group = end == std::string::npos ? std::string() : line.substr(1, end - 1);
      in_entry = group == "Desktop Entry";
      seen_group = true;
      return;
    }
    if (seen_group && !in_entry) return;
    const auto eq = line.find('=');
    if (eq == std::string::npos) return;
    const std::string key = trim_desktop_value(line.substr(0, eq));
    const std::string value = trim_desktop_value(line.substr(eq + 1));
    if (key == "Name" && rec.name.empty()) rec.name = value;
    else if (localized.empty() && key.size() > 5 && key.compare(0, 5, "Name[") == 0) localized = value;
    else if (key == "StartupWMClass" && rec.startup_wm_class.empty()) rec.startup_wm_class = value;
    else if (key == "Icon" && rec.icon.empty()) rec.icon = value;
  };
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '\n') {
      take();
      line.clear();
    } else {
      line.push_back(text[i]);
    }
  }
  if (rec.name.empty()) rec.name = localized;
  return rec;
}

std::string prettify_class_name(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  bool cap = true;
  bool pending_space = false;
  for (unsigned char c : raw) {
    if (c == '-' || c == '_' || c == '.') {
      if (!out.empty()) pending_space = true;
      cap = true;
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    if (cap && c >= 'a' && c <= 'z') out.push_back(static_cast<char>(c - 'a' + 'A'));
    else out.push_back(static_cast<char>(c));
    cap = false;
  }
  return out;
}

std::string display_name_for_window(const WindowFact& window,
                                    const std::vector<DesktopAppRecord>& desktop_apps,
                                    std::string* icon_out) {
  if (icon_out) icon_out->clear();
  const std::string res_class =
      (utf8_valid(window.res_class) && !window.res_class.empty()) ? window.res_class : std::string();
  const std::string res_name =
      (utf8_valid(window.res_name) && !window.res_name.empty()) ? window.res_name : std::string();
  const std::string comm =
      (utf8_valid(window.comm) && !window.comm.empty()) ? window.comm : std::string();

  const DesktopAppRecord* best = nullptr;
  int best_score = 0;
  for (const auto& app : desktop_apps) {
    if (!utf8_valid(app.name) || app.name.empty()) continue;
    const int score = desktop_match_score(app, res_name, res_class, comm);
    if (score > best_score) {
      best = &app;
      best_score = score;
    }
  }
  if (best) {
    if (icon_out && utf8_valid(best->icon)) *icon_out = best->icon;
    return best->name;
  }
  if (!res_class.empty()) return prettify_class_name(res_class);
  if (!res_name.empty()) return prettify_class_name(res_name);
  if (!comm.empty()) return comm;
  if (utf8_valid(window.title) && !window.title.empty()) return window.title;
  if (window.has_pid && window.pid > 1) return "pid " + std::to_string(window.pid);
  return {};
}

std::vector<pid_t> row_tree_pids(
    const std::unordered_set<pid_t>& row_pids,
    const std::unordered_map<pid_t, std::vector<pid_t>>& children) {
  std::vector<pid_t> out;
  std::unordered_set<pid_t> seen;
  for (pid_t root : row_pids) {
    std::vector<pid_t> stack;
    stack.push_back(root);
    while (!stack.empty()) {
      const pid_t pid = stack.back();
      stack.pop_back();
      if (!seen.insert(pid).second) continue;
      out.push_back(pid);
      const auto kids = children.find(pid);
      if (kids == children.end()) continue;
      for (pid_t child : kids->second) {
        if (child != root && row_pids.count(child) != 0) continue;
        stack.push_back(child);
      }
    }
  }
  return out;
}

ProcChargeAction proc_charge_action(bool in_row_tree, bool cached_charge_still_valid) {
  if (in_row_tree && !cached_charge_still_valid) return ProcChargeAction::ReadRollup;
  return ProcChargeAction::SkipRollup;
}

RefreshPace refresh_pace(bool mapped, bool iconified, bool active) {
  if (!mapped || iconified) return RefreshPace::Stopped;
  if (!active) return RefreshPace::Slow;
  return RefreshPace::Live;
}

int refresh_interval_ms(RefreshPace pace) {
  if (pace == RefreshPace::Live) return 3000;
  if (pace == RefreshPace::Slow) return 10000;
  return 0;
}

std::vector<GroupedApp> group_windows(const std::vector<WindowFact>& windows,
                                      const std::vector<DesktopAppRecord>& desktop_apps) {
  struct Acc {
    std::vector<const WindowFact*> all;
    std::vector<const WindowFact*> eligible;
  };
  std::unordered_map<std::string, Acc> groups;
  std::vector<std::string> order;
  order.reserve(windows.size());

  for (const auto& w : windows) {
    const std::string key = (w.has_pid && w.pid > 1)
                                ? ("p" + std::to_string(w.pid))
                                : ("x" + std::to_string(w.xid));
    if (!groups.count(key)) order.push_back(key);
    Acc& acc = groups[key];
    acc.all.push_back(&w);
    if (w.kind == WindowKind::Normal) acc.eligible.push_back(&w);
  }

  std::vector<GroupedApp> out;
  for (const auto& key : order) {
    const Acc& acc = groups[key];
    std::vector<const WindowFact*> pool = acc.eligible;
    if (pool.empty()) {
      for (const WindowFact* w : acc.all) {
        if (lists_without_normal(w->kind)) pool.push_back(w);
      }
    }
    if (pool.empty()) continue;

    const WindowFact* best = nullptr;
    int best_score = -1;
    for (const WindowFact* w : pool) {
      const int score = representative_score(*w);
      if (!best || score > best_score) {
        best = w;
        best_score = score;
      }
    }
    if (!best) continue;

    GroupedApp g;
    g.xid = best->xid;
    g.has_pid = best->has_pid && best->pid > 1;
    g.pid = g.has_pid ? best->pid : 0;
    std::string desktop_icon;
    g.name = display_name_for_window(*best, desktop_apps, &desktop_icon);
    g.desktop_icon = std::move(desktop_icon);
    if (g.name.empty()) continue;
    if (utf8_valid(best->res_class) && !best->res_class.empty()) g.class_name = best->res_class;
    if (utf8_valid(best->title) && !best->title.empty()) g.tooltip = best->title;
    else g.tooltip = g.name;
    g.comm = best->comm;
    g.start_ticks = best->start_ticks;
    g.identity_ok = best->identity_ok;
    if (!g.identity_ok) {
      for (const WindowFact* w : acc.all) {
        if (!w->identity_ok) continue;
        g.comm = w->comm;
        g.start_ticks = w->start_ticks;
        g.identity_ok = true;
        break;
      }
    }

    for (const WindowFact* w : acc.all) {
      std::string why;
      if (window_marks_protected(*w, why)) {
        g.protected_app = true;
        if (g.protect_reason.empty()) g.protect_reason = why;
      }
    }
    if (!g.has_pid && g.protect_reason.empty()) g.protect_reason = "No process ID";
    out.push_back(std::move(g));
  }
  disambiguate_row_names(out);
  return out;
}

bool is_display_class(unsigned class_code) {
  return ((class_code >> 16) & 0xFFu) == 0x03u;
}

PciDb parse_pci_ids(std::istream& in) {
  PciDb db;
  std::string line;
  unsigned current_vendor = 0;
  bool have_vendor = false;
  while (std::getline(in, line)) {
    line = trim_cr(line);
    if (line.empty() || line[0] == '#') continue;
    if (line[0] == '\t') {
      if (!have_vendor) continue;
      if (line.size() > 1 && line[1] == '\t') continue;
      unsigned id = 0;
      size_t name_at = 0;
      if (!parse_pci_token(line, 1, id, name_at)) continue;
      db.devices[current_vendor][id] = line.substr(name_at);
      continue;
    }
    unsigned id = 0;
    size_t name_at = 0;
    if (!parse_pci_token(line, 0, id, name_at)) {
      have_vendor = false;
      continue;
    }
    current_vendor = id;
    have_vendor = true;
    db.vendors[id] = line.substr(name_at);
  }
  return db;
}

namespace {

std::string pci_card_key(const std::string& slot) {
  const auto dot = slot.rfind('.');
  if (dot == std::string::npos || dot + 1 >= slot.size()) return slot;
  for (size_t i = dot + 1; i < slot.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(slot[i]))) return slot;
  }
  return slot.substr(0, dot);
}

std::string gpu_device_piece(const GpuDevice& dev, const PciDb& db) {
  const auto vendor = db.vendors.find(dev.vendor);
  std::string device_name;
  const auto devs = db.devices.find(dev.vendor);
  if (devs != db.devices.end()) {
    const auto found = devs->second.find(dev.device);
    if (found != devs->second.end()) device_name = found->second;
  }
  if (vendor != db.vendors.end() && !device_name.empty()) {
    return vendor->second + " " + device_name;
  }
  char buf[80];
  std::snprintf(buf, sizeof(buf), "PCI 0x%04x:0x%04x", dev.vendor, dev.device);
  std::string piece = buf;
  if (!dev.driver.empty()) piece += " (" + dev.driver + ")";
  return piece;
}

bool piece_is_named(const std::string& piece) {
  return piece.compare(0, 4, "PCI ") != 0;
}

struct GpuCard {
  std::string key;
  std::string slot;
  bool have_vga = false;
  bool have_3d = false;
  bool have_other = false;
  size_t vga = 0;
  size_t three_d = 0;
  size_t other = 0;
};

int gpu_card_rank(const GpuCard& card) {
  // The panel's display controller before a discrete 3D controller.
  if (card.have_vga) return 0;
  if (card.have_3d) return 1;
  return 2;
}

}  // namespace

std::string gpu_label_from_devices(const std::vector<GpuDevice>& devices,
                                   const PciDb& db) {
  std::vector<GpuCard> cards;
  for (size_t i = 0; i < devices.size(); ++i) {
    if (!is_display_class(devices[i].class_code)) continue;
    const std::string key = pci_card_key(devices[i].slot);
    GpuCard* card = nullptr;
    for (GpuCard& existing : cards) {
      if (existing.key == key) {
        card = &existing;
        break;
      }
    }
    if (!card) {
      cards.push_back(GpuCard{});
      card = &cards.back();
      card->key = key;
      card->slot = devices[i].slot;
    } else if (devices[i].slot < card->slot) {
      card->slot = devices[i].slot;
    }
    if (is_vga_controller(devices[i].class_code)) {
      card->have_vga = true;
      card->vga = i;
    } else if (is_3d_controller(devices[i].class_code)) {
      card->have_3d = true;
      card->three_d = i;
    } else {
      card->have_other = true;
      card->other = i;
    }
  }
  if (cards.empty()) return {};

  std::sort(cards.begin(), cards.end(), [](const GpuCard& a, const GpuCard& b) {
    const int ra = gpu_card_rank(a);
    const int rb = gpu_card_rank(b);
    if (ra != rb) return ra < rb;
    return a.slot < b.slot;
  });

  std::string out;
  for (const GpuCard& card : cards) {
    size_t idx = card.other;
    if (card.have_vga && card.have_3d) {
      // One card, two functions: a single name. Prefer the 3D function when
      // pci.ids names it; otherwise keep the display controller's name.
      const std::string named_3d = gpu_device_piece(devices[card.three_d], db);
      const std::string named_vga = gpu_device_piece(devices[card.vga], db);
      idx = piece_is_named(named_3d) || !piece_is_named(named_vga) ? card.three_d : card.vga;
    } else if (card.have_vga) {
      idx = card.vga;
    } else if (card.have_3d) {
      idx = card.three_d;
    } else if (!card.have_other) {
      continue;
    }
    const std::string piece = gpu_device_piece(devices[idx], db);
    if (piece.empty()) continue;
    if (!out.empty()) out += "; ";
    out += piece;
  }
  return out;
}

bool credits_should_crawl(int view_height, int layout_pixel_height, int fit_slack) {
  if (view_height <= 1 || layout_pixel_height <= 0) return false;
  return layout_pixel_height > view_height + fit_slack;
}

CreditsTickResult credits_on_tick(bool names_overflow, bool pointer_over) {
  if (!names_overflow || pointer_over) return CreditsTickResult::StopAndClearId;
  return CreditsTickResult::Continue;
}

bool may_delete_row(bool in_next_snapshot, bool menu_posted, bool dialog_open) {
  if (in_next_snapshot) return false;
  if (dialog_open) return false;
  if (menu_posted) return false;
  return true;
}

double clamp_scroll_value(double value, double upper, double page_size) {
  const double max_value = std::max(0.0, upper - page_size);
  if (value < 0.0) return 0.0;
  if (value > max_value) return max_value;
  return value;
}

bool scroll_anchor_ready(double row_bottom, double upper) {
  if (upper <= 1.0) return false;
  return row_bottom <= upper + 1.0;
}

double anchored_scroll_value(double row_y, double delta, double upper, double page) {
  return clamp_scroll_value(row_y + delta, upper, page);
}

std::optional<double> restore_anchored_scroll(bool layout_ready, double row_y, double delta,
                                              double row_bottom, double upper, double page) {
  if (!layout_ready || !scroll_anchor_ready(row_bottom, upper)) return std::nullopt;
  return anchored_scroll_value(row_y, delta, upper, page);
}

bool utf8_valid(const std::string& text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text.data());
  const auto* end = p + text.size();
  while (p < end) {
    const unsigned char c = *p;
    if (c == 0) return false;
    if (c < 0x80) {
      ++p;
      continue;
    }
    unsigned long cp = 0;
    int need = 0;
    if ((c & 0xE0) == 0xC0) {
      need = 1;
      cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
      need = 2;
      cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
      need = 3;
      cp = c & 0x07u;
    } else {
      return false;
    }
    if (p + need >= end) return false;
    for (int i = 1; i <= need; ++i) {
      if ((p[i] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    if (need == 1 && cp < 0x80) return false;
    if (need == 2 && cp < 0x800) return false;
    if (need == 3 && cp < 0x10000) return false;
    if (cp > 0x10FFFFul) return false;
    if (cp >= 0xD800ul && cp <= 0xDFFFul) return false;
    p += static_cast<size_t>(need) + 1;
  }
  return true;
}

std::string escape_mnemonic(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if (c == '_') out.push_back('_');
    out.push_back(c);
  }
  return out;
}

std::string force_close_menu_label(const std::string& row_name, bool can_close,
                                   const std::string& protect_reason) {
  if (can_close) return "Force Close " + escape_mnemonic(row_name);
  std::string reason = protect_reason.empty() ? "Protected" : protect_reason;
  return escape_mnemonic(reason);
}

std::string row_tooltip_text(const std::string& title_or_name, bool can_close,
                             const std::string& protect_reason,
                             const std::string& distinguish) {
  if (!can_close) {
    const std::string reason = protect_reason.empty() ? std::string("Protected") : protect_reason;
    const auto nl = title_or_name.find('\n');
    if (nl != std::string::npos) return reason + title_or_name.substr(nl);
    return reason;
  }
  std::string body = title_or_name;
  if (!distinguish.empty() && body.find(distinguish) == std::string::npos) {
    if (!body.empty()) body += "\n";
    body += distinguish;
  }
  const char* hint = "Right-click or press the Menu key or Shift+F10 to Force Close";
  if (body.empty()) return hint;
  return body + "\n" + hint;
}

bool is_force_close_popup_key(unsigned keyval, unsigned state) {
  constexpr unsigned kMenu = 0xff67u;
  constexpr unsigned kMenuKB = 0x1008ff65u;
  constexpr unsigned kF10 = 0xffc7u;
  constexpr unsigned kShift = 1u;
  if (keyval == kMenu || keyval == kMenuKB) return true;
  if ((state & kShift) != 0 && keyval == kF10) return true;
  return false;
}

bool is_row_activate_key(unsigned keyval, unsigned state) {
  constexpr unsigned kControl = 4u;
  constexpr unsigned kMod1 = 8u;
  if ((state & (kControl | kMod1)) != 0) return false;
  constexpr unsigned kReturn = 0xff0du;
  constexpr unsigned kKPEnter = 0xff8du;
  constexpr unsigned kISOEnter = 0xfe34u;
  return keyval == kReturn || keyval == kKPEnter || keyval == kISOEnter;
}

PidfdOpenAction pidfd_open_action(int pidfd, int err) {
  if (pidfd >= 0) return PidfdOpenAction::SendOnPidfd;
  if (err == ESRCH) return PidfdOpenAction::AlreadyExited;
  return PidfdOpenAction::FallbackKill;
}

PidfdSignalFailure pidfd_signal_failure(int err) {
  if (err == ENOSYS) return PidfdSignalFailure::FallbackKill;
  if (err == ESRCH) return PidfdSignalFailure::AlreadyExited;
  return PidfdSignalFailure::ReportErrno;
}

std::string force_close_errno_message(pid_t pid, int err) {
  const char* text = std::strerror(err);
  if (!text || text[0] == '\0') text = "Unknown error";
  return "Could not force-close PID " + std::to_string(pid) + ": " + text;
}

ForceCloseOrder force_close_signal_order(pid_t root, const std::vector<ProcPin>& pins) {
  ForceCloseOrder order;
  order.root = root;
  for (const auto& pin : pins) {
    if (pin.pid == root) continue;
    order.helpers.push_back(pin.pid);
  }
  return order;
}

bool force_close_should_signal_helpers(bool root_signalled) { return root_signalled; }

void helper_close_note(HelperCloseReport& report, pid_t pid, bool ok, const std::string& why) {
  if (ok) {
    report.signalled.push_back(pid);
    return;
  }
  if (why.empty() || why == "Already exited.") return;
  if (why == "That PID belongs to another row and was not signalled.") return;
  report.failures.push_back("PID " + std::to_string(pid) + ": " + why);
}

std::string helper_close_message(const HelperCloseReport& report) {
  if (report.failures.empty()) return {};
  std::string msg = "Signalled";
  if (report.signalled.empty()) {
    msg += " nothing.";
  } else {
    for (size_t i = 0; i < report.signalled.size(); ++i) {
      if (i == 0) msg += " PID ";
      else msg += ", PID ";
      msg += std::to_string(report.signalled[i]);
    }
    msg += ".";
  }
  for (const auto& line : report.failures) {
    msg += "\n";
    msg += line;
  }
  return msg;
}

namespace {

std::string prompt_text(const std::string& text) {
  if (utf8_valid(text)) return text;
  std::string out;
  out.reserve(text.size());
  for (unsigned char c : text) {
    if (c >= 0x20 && c < 0x7f) out.push_back(static_cast<char>(c));
    else out.push_back('?');
  }
  return out;
}

bool same_name(const std::string& name, const std::string& comm) {
  if (name.size() != comm.size()) return false;
  for (size_t i = 0; i < name.size(); ++i) {
    const unsigned char a = static_cast<unsigned char>(name[i]);
    const unsigned char b = static_cast<unsigned char>(comm[i]);
    if (std::tolower(a) != std::tolower(b)) return false;
  }
  return true;
}

}  // namespace

ForceClosePrompt force_close_prompt(const std::string& row_name, const std::string& command,
                                    pid_t pid, const std::string& window_class,
                                    bool window_still_there) {
  const std::string shown = prompt_text(!row_name.empty() ? row_name : command);
  const std::string comm = prompt_text(command.empty() ? "unknown" : command);
  const std::string klass = prompt_text(window_class);
  ForceClosePrompt prompt;
  prompt.primary = "Force Close \"" + shown + "\"?";
  prompt.secondary = "This will quit " + shown + " immediately (command " + comm + ", PID " +
                     std::to_string(pid) +
                     "), including helper processes counted in this row. Unsaved work will be lost.";
  if (!klass.empty() && !same_name(klass, command)) {
    prompt.secondary += "\nWindow class \"" + klass + "\" does not match that command.";
  }
  if (!window_still_there) {
    prompt.secondary +=
        "\nThe listed window is already gone. The process is still closed when its command and "
        "start time match this refresh.";
  }
  return prompt;
}

SmapsRollup parse_smaps_rollup(const std::string& text) {
  SmapsRollup out;
  std::string line;
  auto take = [&]() {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto colon = line.find(':');
    if (colon == std::string::npos) return;
    const std::string key = line.substr(0, colon);
    const auto pos = line.find_first_of("0123456789", colon + 1);
    long value = 0;
    bool saw_number = false;
    if (pos != std::string::npos) {
      try {
        value = std::stol(line.substr(pos));
        saw_number = true;
      } catch (...) {
        saw_number = false;
      }
    }
    if (!saw_number) return;
    if (key == "Pss_Anon") {
      out.saw_pss_anon = true;
      out.pss_anon_kb = value;
    } else if (key == "Private_Dirty") {
      out.saw_private_dirty = true;
      out.private_dirty_kb = value;
    }
  };
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '\n') {
      take();
      line.clear();
    } else {
      line.push_back(text[i]);
    }
  }
  return out;
}

std::optional<long> anon_charge_kb(const SmapsRollup& rollup) {
  if (rollup.saw_pss_anon) return rollup.pss_anon_kb;
  if (rollup.saw_private_dirty) return rollup.private_dirty_kb;
  return std::nullopt;
}

long process_anon_charge_kb(bool have_rollup, const SmapsRollup& rollup, long rss_anon_kb) {
  if (have_rollup) {
    if (const auto charge = anon_charge_kb(rollup)) return *charge;
  }
  return rss_anon_kb;
}

std::vector<std::string> parse_supporter_entries(std::istream& in) {
  std::vector<std::string> entries;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && line[0] == '#') continue;
    bool blank = true;
    for (char c : line) {
      if (c != ' ' && c != '\t') {
        blank = false;
        break;
      }
    }
    if (blank) continue;
    entries.push_back(line);
  }
  return entries;
}

std::string join_supporter_entries(const std::vector<std::string>& entries) {
  std::string names;
  for (const std::string& entry : entries) {
    if (!names.empty()) names += ", ";
    names += entry;
  }
  return names;
}

std::vector<std::string> builtin_supporter_entries() {
  return {
      "\"Fuzzy\"",
      "Jon Darrow",
      "Steve Rockefeller",
      "Steven P.",
      "Chris Hammond",
      "Mike Beasley",
      "Jack Beckman",
      "Jesse Buschhaus",
      "Bob Lorna",
  };
}

}  // namespace lundukeabout
