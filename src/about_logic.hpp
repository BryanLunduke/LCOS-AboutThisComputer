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

// Each pid contributes its charged anonymous memory once, even if two rows list it.
long long sum_rss_once(const std::vector<ProcPin>& pins);

// smaps_rollup fields used for per-process anonymous memory. Pss_Anon is the
// proportional share of anonymous pages (shared pages count once when the
// shares are added). Private_Dirty is the fallback when the kernel has no
// Pss_Anon line: those pages are private, so they are not repeated.
struct SmapsRollup {
  bool saw_pss_anon = false;
  bool saw_private_dirty = false;
  long pss_anon_kb = 0;
  long private_dirty_kb = 0;
};

SmapsRollup parse_smaps_rollup(const std::string& text);
// nullopt when neither field was present.
std::optional<long> anon_charge_kb(const SmapsRollup& rollup);
// have_rollup false, or a rollup with neither field, uses rss_anon_kb.
long process_anon_charge_kb(bool have_rollup, const SmapsRollup& rollup, long rss_anon_kb);

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
  // Valid UTF-8 WM_CLASS class, before any disambiguating suffix. Empty when
  // the class was missing or not UTF-8.
  std::string class_name;
  // Short token ("pid 32", "window 9") when another row shares this name.
  // Empty when the name is unique. The row shows it in a column that does
  // not ellipsize; `name` may also carry it for the menu and the dialog.
  std::string distinguish;
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
// gets a row so its anonymous memory is subtracted from LCOS System.
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
// One name per PCI card. A card that exposes both a VGA function and a 3D
// function is named once. Distinct cards are all named, display controller
// first, then 3D controllers, joined with "; ".
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

// True when a row whose bottom is row_bottom has been given a place inside
// the current adjustment upper. A short upper left over from the previous
// list is not ready: clamping the saved pixel onto it would stick.
bool scroll_anchor_ready(double row_bottom, double upper);

// Viewport value that puts row_y + delta at the top, clamped to the range.
double anchored_scroll_value(double row_y, double delta, double upper, double page);

// nullopt until the new layout can hold the anchored row. Callers must not
// fall back to clamping against a short upper.
std::optional<double> restore_anchored_scroll(bool layout_ready, double row_y, double delta,
                                              double row_bottom, double upper, double page);

// Byte string is well-formed UTF-8 with no embedded NUL and no surrogate.
bool utf8_valid(const std::string& text);

// Menu labels parse '_' as a mnemonic. A doubled underscore is shown as one.
std::string escape_mnemonic(const std::string& text);
std::string force_close_menu_label(const std::string& row_name, bool can_close,
                                   const std::string& protect_reason);

// Name painted in the ellipsizing column. A trailing " (pid N)" or
// " (window N)" that repeats `distinguish` is removed so that token can
// sit in its own column. The stored name, the menu, and the dialog keep it.
std::string painted_row_name(const std::string& name, const std::string& distinguish);

// Closable rows: the title, the shared-name token when one was assigned,
// and the Force Close hint. Rows that cannot be closed use protect_reason
// (or "Protected") and do not invite Force Close. A newline already in
// title_or_name, such as the unclamped remainder, is kept after the reason.
std::string row_tooltip_text(const std::string& title_or_name, bool can_close,
                             const std::string& protect_reason,
                             const std::string& distinguish);

// GDK_KEY_Menu, the XF86 menu key beside the keypad (GDK_KEY_MenuKB), and
// Shift+F10. Plain F10 is not a Force Close key. Return is not one of
// these; it is the row's default action (is_row_activate_key).
bool is_force_close_popup_key(unsigned keyval, unsigned state);

// Return, keypad Enter, and ISO Enter run the focused row's default action
// (the Force Close menu). Control and Alt chords are not that action.
bool is_row_activate_key(unsigned keyval, unsigned state);

// What to do with the result of pidfd_open. ESRCH means the process is gone.
// ENOSYS and every other errno (EMFILE, ENOMEM, EPERM, EINVAL) use kill()
// after the start-time check, the same as a kernel without pidfd.
enum class PidfdOpenAction {
  SendOnPidfd,
  FallbackKill,
  AlreadyExited,
};

PidfdOpenAction pidfd_open_action(int pidfd, int err);

// pidfd_send_signal failed. ENOSYS falls back to kill(). ESRCH is exit.
// Any other errno is reported; kill() would drop the pin.
enum class PidfdSignalFailure {
  FallbackKill,
  AlreadyExited,
  ReportErrno,
};

PidfdSignalFailure pidfd_signal_failure(int err);
std::string force_close_errno_message(pid_t pid, int err);

struct ForceCloseOrder {
  pid_t root = 0;
  std::vector<pid_t> helpers;
};

// Root first, then other pins in their existing order. Helpers are not
// signalled when the root signal fails.
ForceCloseOrder force_close_signal_order(pid_t root, const std::vector<ProcPin>& pins);
bool force_close_should_signal_helpers(bool root_signalled);

struct HelperCloseReport {
  std::vector<pid_t> signalled;
  std::vector<std::string> failures;
};

void helper_close_note(HelperCloseReport& report, pid_t pid, bool ok, const std::string& why);
std::string helper_close_message(const HelperCloseReport& report);

struct ForceClosePrompt {
  std::string primary;
  std::string secondary;
};

// Title uses the row the user clicked. The body names the command and PID
// in ordinary language. window_class is the raw class, not a disambiguated
// label; the mismatch line is kept when that class and the command differ.
ForceClosePrompt force_close_prompt(const std::string& row_name, const std::string& command,
                                    pid_t pid, const std::string& window_class,
                                    bool window_still_there);

}  // namespace lundukeabout
