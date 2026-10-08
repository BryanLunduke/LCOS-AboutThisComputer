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

std::string application_name(const WindowFact& w) {
  // A Latin-1 class or title is not a name. Fall through to the command,
  // then a valid title, then the pid. Pango must not be handed those bytes.
  if (utf8_valid(w.res_class) && !w.res_class.empty()) return w.res_class;
  if (utf8_valid(w.res_name) && !w.res_name.empty()) return w.res_name;
  if (utf8_valid(w.comm) && !w.comm.empty()) return w.comm;
  if (utf8_valid(w.title) && !w.title.empty()) return w.title;
  if (w.has_pid && w.pid > 1) return "pid " + std::to_string(w.pid);
  return {};
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
  std::string tok;
  for (int field = 3; field <= 22; ++field) {
    if (!(iss >> tok)) return id;
  }
  try {
    id.start_ticks = std::stoull(tok);
  } catch (...) {
    return id;
  }
  id.ok = true;
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

std::vector<GroupedApp> group_windows(const std::vector<WindowFact>& windows) {
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
    g.name = application_name(*best);
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

std::string gpu_label_from_devices(const std::vector<GpuDevice>& devices,
                                   const PciDb& db) {
  std::vector<size_t> display;
  bool any_3d = false;
  bool any_vga = false;
  for (size_t i = 0; i < devices.size(); ++i) {
    if (!is_display_class(devices[i].class_code)) continue;
    display.push_back(i);
    if (is_3d_controller(devices[i].class_code)) any_3d = true;
    if (is_vga_controller(devices[i].class_code)) any_vga = true;
  }
  if (display.empty()) return {};

  std::vector<size_t> chosen;
  if (any_3d && any_vga) {
    for (size_t i : display) {
      if (is_3d_controller(devices[i].class_code)) chosen.push_back(i);
    }
  } else {
    chosen = display;
  }

  std::string out;
  for (size_t i : chosen) {
    const GpuDevice& dev = devices[i];
    std::string piece;
    const auto vendor = db.vendors.find(dev.vendor);
    std::string device_name;
    const auto devs = db.devices.find(dev.vendor);
    if (devs != db.devices.end()) {
      const auto found = devs->second.find(dev.device);
      if (found != devs->second.end()) device_name = found->second;
    }
    if (vendor != db.vendors.end() && !device_name.empty()) {
      piece = vendor->second + " " + device_name;
    } else {
      char buf[80];
      std::snprintf(buf, sizeof(buf), "PCI 0x%04x:0x%04x", dev.vendor, dev.device);
      piece = buf;
      if (!dev.driver.empty()) piece += " (" + dev.driver + ")";
    }
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
  const char* hint = "Right-click or press the Menu key to Force Close";
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

}  // namespace lundukeabout
