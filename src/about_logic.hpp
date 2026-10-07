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
// Frames larger than this are skipped (their pixels are not decoded).
// The decoder also refuses an edge of 512 or more.
constexpr unsigned long kIconSelectMaxEdge = 256;

struct IconChoice {
  unsigned long width = 0;
  unsigned long height = 0;
  unsigned long pixel_offset = 0;
};

// Read one CARD32 of a (possibly truncated) _NET_WM_ICON. Return false when
// that index is not in the current chunk. total_items is the full property.
using IconWordReader = bool (*)(void* ctx, unsigned long index, unsigned long& word);

// Best ~32px frame, or nullopt. A frame whose header is readable and whose
// declared size fits in total_items is skipped when its pixels are outside
// the chunk or either edge is greater than kIconSelectMaxEdge. A zero
// dimension, an overflowing size, or a frame that does not fit in total_items
// stops the walk. The function does not loop.
std::optional<IconChoice> choose_net_wm_icon_chunked(unsigned long total_items,
                                                    IconWordReader read_word,
                                                    void* ctx);

// Whole-buffer walk: total_items is nitems, and every index below nitems is readable.
std::optional<IconChoice> choose_net_wm_icon(const unsigned long* icons,
                                            unsigned long nitems);

// After a page of CARD32 window ids has been consumed: true means another
// page should be read at next_offset. Stops at the end and at max_ids.
// An empty page (nitems == 0) does not advance, even when bytes_after > 0.
// The caller treats that as a missing or empty client list and walks the tree.
bool client_list_advance(long offset, unsigned long nitems,
                         unsigned long bytes_after, long max_ids,
                         long& next_offset);

// Format 32 values are stored as long. Format 8 or 16 must not be indexed
// as Atom / Window / CARD32. nbytes is the buffer size in bytes.
// {format=8, nitems=128, nbytes=129} is not indexable.
bool x_property_indexable_as_longs(int format, bool type_matches,
                                   unsigned long nitems, unsigned long nbytes);

// Bytes of a UTF8_STRING that may be copied. 0 unless format is 8, the type
// matches, and both counts are non-zero. Never more than nbytes.
unsigned long x_property_utf8_copy_bytes(int format, bool type_matches,
                                         unsigned long nitems, unsigned long nbytes);

// rss of root plus descendants. A descendant that is itself in row_pids is
// omitted, along with its subtree, so that process is counted on its own row.
// A parent/child cycle terminates.
long rollup_rss_anon(pid_t root,
                     const std::unordered_map<pid_t, long>& rss_kb,
                     const std::unordered_map<pid_t, std::vector<pid_t>>& children,
                     const std::unordered_set<pid_t>& row_pids);

struct ProcSnapshot {
  bool ok = false;
  std::string comm;
  unsigned long long start_ticks = 0;
};

// /proc/<pid>/stat line. comm is between the first '(' and the last ')'.
// starttime is field 22. ok is false when the line is short or not a number.
ProcSnapshot parse_proc_stat_line(const std::string& stat_line);

// Refresh-time comm + starttime must both still match. A recycled pid has a
// different starttime and does not match, even when comm is unchanged.
bool proc_identity_matches(const ProcSnapshot& pinned, const ProcSnapshot& now);

// One process Force Close may signal. Descendants are included only when the
// starttime re-read still matches the refresh snapshot. Other rows, pid <= 1,
// and this process are never signalled.
bool may_signal_pinned_pid(pid_t pid, pid_t self_pid,
                           unsigned long long expected_start,
                           bool reread_ok, unsigned long long reread_start,
                           bool belongs_to_other_row);

struct ProcPin {
  pid_t pid = 0;
  unsigned long long start_ticks = 0;
  long rss_kb = 0;
};

// Root plus descendants that are not another row. Pids without a starttime
// are omitted (they cannot be re-checked). A cycle terminates.
std::vector<ProcPin> collect_kill_pins(
    pid_t root,
    const std::unordered_map<pid_t, long>& rss_kb,
    const std::unordered_map<pid_t, unsigned long long>& start_ticks,
    const std::unordered_map<pid_t, std::vector<pid_t>>& children,
    const std::unordered_set<pid_t>& row_pids);

// Each pid contributes its RssAnon once, even if two rows list it.
long long sum_rss_once(const std::vector<ProcPin>& pins);

struct SystemRemainder {
  long shown_kb = 0;
  long long raw_kb = 0;
  bool clamped = false;
};

// used − apps. A negative raw remainder is shown as 0 (clamped) so shared
// anonymous pages cannot paint LCOS System as owing memory.
SystemRemainder system_remainder_kb(long used_kb, long long apps_rss);

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
  unsigned long long start_ticks = 0;
  bool identity_ok = false;
};

// True only when /proc comm matches a protected binary. A WM_CLASS hit whose
// comm is a different program does not protect that pid.
bool window_marks_protected(const WindowFact& window, std::string& reason);

struct GroupedApp {
  unsigned long xid = 0;
  pid_t pid = 0;
  bool has_pid = false;
  std::string name;
  std::string tooltip;
  bool protected_app = false;
  std::string protect_reason;
  std::string comm;
  unsigned long long start_ticks = 0;
  bool identity_ok = false;
};

// One row per PID (or per XID when the window has no PID). The representative
// prefers an active Normal window, then a Normal window with a title and an
// icon. Splash, menu, tooltip, and notification windows are not rows.
// A pid whose windows are only dock, desktop, utility, or skip-taskbar still
// gets a row so its RssAnon is subtracted from LCOS System.
// Protection comes from /proc comm, not from a foreign WM_CLASS.
std::vector<GroupedApp> group_windows(const std::vector<WindowFact>& windows);

// A refresh may destroy this row only when it left the snapshot, its menu
// is not posted, and no Force Close dialog is nested.
bool may_delete_row(bool in_next_snapshot, bool menu_posted, bool dialog_open);

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

// The frame callback returns false and clears tick_id_ without
// remove_tick_callback. GTK drops the callback when the handler returns false.
enum class CreditsTickResult {
  Continue,
  StopAndClearId,
};

CreditsTickResult credits_on_tick(bool names_overflow, bool pointer_over);

double clamp_scroll_value(double value, double upper, double page_size);

}  // namespace lundukeabout
