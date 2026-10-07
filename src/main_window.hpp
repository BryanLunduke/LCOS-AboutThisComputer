// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "system_info.hpp"
#include "memory_bar.hpp"
#include "window_enum.hpp"

#include <gtkmm.h>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace lundukeabout {
class AppRow;
}

namespace lundukeabout {

// Right-aligned, wrapping supporter names. Height is capped to the header
// space beside the logo. A frame-clock crawl runs only while the wrapped
// text is taller than that cap; when the names fit, the view is static.
class SupportersNamesView : public Gtk::DrawingArea {
public:
  SupportersNamesView();
  ~SupportersNamesView() override;

  void set_text(const Glib::ustring& text);
  void set_max_height(int height);
  int line_height() const;

protected:
  Gtk::SizeRequestMode get_request_mode_vfunc() const override;
  void get_preferred_width_vfunc(int& minimum_width, int& natural_width) const override;
  void get_preferred_height_vfunc(int& minimum_height, int& natural_height) const override;
  void get_preferred_height_for_width_vfunc(int width, int& minimum_height,
                                            int& natural_height) const override;
  void on_size_allocate(Gtk::Allocation& allocation) override;
  void on_style_updated() override;
  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;
  bool on_enter_notify_event(GdkEventCrossing* crossing_event) override;
  bool on_leave_notify_event(GdkEventCrossing* crossing_event) override;

private:
  void ensure_layout(int width) const;
  void invalidate_layout();
  void sync_scroll_policy();
  void start_tick();
  void stop_tick();
  bool on_tick(const Glib::RefPtr<Gdk::FrameClock>& clock);
  bool names_overflow(int view_height) const;

  Glib::ustring text_;
  int max_height_ = 0;
  double scroll_offset_ = 0.0;
  gint64 last_frame_us_ = 0;
  guint tick_id_ = 0;

  mutable Glib::RefPtr<Pango::Layout> layout_;
  mutable int layout_width_ = -1;
  mutable int layout_pixel_height_ = 0;
  mutable int single_line_height_ = 0;
  bool pointer_over_ = false;
};

class MainWindow : public Gtk::ApplicationWindow {
public:
  MainWindow();
  ~MainWindow() override;

private:
  void apply_platinum_css();
  void load_logo();
  void load_supporters();
  void update_supporters_cap();
  void on_supporters_header_allocate(Gtk::Allocation& allocation);
  void update_ram_bar();
  Glib::RefPtr<Gdk::Pixbuf> load_lcos_system_icon() const;
  void on_force_close(const AppEntry& entry);
  void arm_force_close_refresh();
  bool signal_pinned_pid(pid_t pid, unsigned long long expected_start, bool other_row,
                         bool require_comm, const std::string& expected_comm, std::string& why);
  bool on_refresh_tick();
  void on_mapped();
  void on_unmapped();
  bool on_window_state(GdkEventWindowState* event);
  void start_refresh_timer();
  void stop_refresh_work();
  void schedule_refresh();
  bool on_probe_idle();
  void apply_app_snapshot(std::vector<AppEntry> apps, bool x11);
  void on_list_scroll_allocate(Gtk::Allocation& allocation);
  void arm_scroll_restore(double value);
  void apply_pending_scroll();
  std::string find_data_file(const std::string& relative) const;

  struct AppListItem {
    std::string key;
    AppRow* row = nullptr;
    Gtk::Separator* sep = nullptr;
  };
  AppEntry make_system_entry(long system_kb, const std::string& tooltip);
  void sync_app_rows(const std::vector<AppEntry>& apps, bool allow_structure);

  SystemInfo info_;
  Gtk::Box root_{Gtk::ORIENTATION_VERTICAL, 0};
  Gtk::Image logo_;
  // Title stays fixed. supporters_names_view_ scrolls only when the wrapped
  // names are taller than the header space beside the logo.
  Gtk::Box supporters_box_{Gtk::ORIENTATION_VERTICAL, 0};
  Gtk::Label supporters_title_;
  Gtk::Label supporters_blank_;
  SupportersNamesView supporters_names_view_;
  bool supporters_cap_update_queued_ = false;
  bool alive_ = true;
  Gtk::Label os_label_;
  Gtk::Label mem_label_;
  Gtk::Label cpu_label_;
  Gtk::Label gpu_label_;
  MemoryBar ram_bar_;
  Gtk::ScrolledWindow list_scroll_;
  Gtk::Box list_box_{Gtk::ORIENTATION_VERTICAL, 0};
  std::vector<AppListItem> app_rows_;
  AppRow* system_row_ = nullptr;
  Glib::RefPtr<Gdk::Pixbuf> system_icon_;
  // Nested dialog: do not destroy rows while a Force Close menu callback
  // is still on the stack.
  int force_close_depth_ = 0;
  Glib::RefPtr<Gtk::CssProvider> css_;
  sigc::connection refresh_conn_;
  sigc::connection probe_conn_;
  sigc::connection scroll_restore_conn_;
  sigc::connection scroll_idle_conn_;
  sigc::connection supporters_idle_conn_;
  sigc::connection force_close_refresh_conn_;
  std::unique_ptr<AppListRefresh> probe_;
  bool iconified_ = false;
  bool refresh_running_ = false;
  // Set only by an explicit Force Close refresh, never by the 3s timer.
  bool refresh_followup_ = false;
  std::chrono::steady_clock::time_point last_snapshot_{};
  bool pending_scroll_restore_ = false;
  double pending_scroll_ = 0.0;
};

}  // namespace lundukeabout
