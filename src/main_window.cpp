// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.hpp"
#include "app_row.hpp"
#include "about_logic.hpp"
#include "config.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <climits>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/syscall.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>

namespace lundukeabout {
namespace {

// Empty-list copy retained for reference; UI always shows LCOS System instead.
[[maybe_unused]] constexpr const char* kNoRunningSoftware = "No Running Software.";

// Theme-aware chrome: no hardcoded platinum greys for window/panel.
// The list stays white, so its labels are forced dark (a dark theme's
// foreground would otherwise be unreadable on that white).
const char* kAboutCss = R"CSS(
window.lunduke-about {
  background-color: @theme_bg_color;
  color: @theme_fg_color;
}
window.lunduke-about * {
  font-family: "Charcoal", "Geneva", "Helvetica", "DejaVu Sans", Sans;
}
.about-panel {
  background-color: @theme_bg_color;
}
.about-info {
  background-color: @theme_bg_color;
  color: @theme_fg_color;
  font-size: 13px;
  font-weight: bold;
}
.about-info label {
  color: @theme_fg_color;
  font-size: 13px;
  font-weight: bold;
}
.platinum-list-frame {
  background-color: #ffffff;
  border-style: solid;
  border-width: 2px;
  border-color: #404040 #ffffff #ffffff #404040;
  border-radius: 0;
}
.platinum-list {
  background-color: #ffffff;
  color: #1a1a1a;
}
.platinum-list label,
.platinum-list row label {
  color: #1a1a1a;
}
.platinum-list label:disabled,
.platinum-list row label:disabled {
  color: #5a5a5a;
}
.platinum-list row,
.platinum-list .app-row-bg,
.platinum-list .app-row {
  background-color: #ffffff;
  color: #1a1a1a;
}
.platinum-list .app-row:focus {
  background-color: #e4eaf6;
}
.about-header {
  background-color: @theme_bg_color;
}
.supporters-names {
  background-color: @theme_bg_color;
  color: @theme_fg_color;
  font-size: 13px;
  font-weight: bold;
}
/* The class lives on the scrolled window. Themes style the scrollbar GTK
   creates inside it, so the selector has to reach that child. The same
   class is also added to the scrollbar widget. Square 14px blue thumb,
   only for this list — nothing here restyles other windows. */
scrolledwindow.platinum-scroll scrollbar,
scrollbar.platinum-scroll {
  background-color: @theme_bg_color;
  background-image: none;
  border-radius: 0;
  box-shadow: none;
}
scrolledwindow.platinum-scroll scrollbar.vertical,
scrollbar.platinum-scroll.vertical {
  min-width: 14px;
}
scrolledwindow.platinum-scroll scrollbar trough,
scrollbar.platinum-scroll trough {
  background-color: @theme_bg_color;
  background-image: none;
  border-radius: 0;
  min-width: 14px;
  box-shadow: none;
}
scrolledwindow.platinum-scroll scrollbar slider,
scrollbar.platinum-scroll slider {
  background-color: #5a7ec8;
  background-image: none;
  border-radius: 0;
  min-width: 14px;
  min-height: 14px;
  border: 1px solid #2a4a8a;
  margin: 0;
  box-shadow: none;
}
scrolledwindow.platinum-scroll scrollbar button,
scrollbar.platinum-scroll button {
  background-color: @theme_bg_color;
  background-image: none;
  border-radius: 0;
}
)CSS";

// Movie-credits crawl. Slow enough to read; integer pixels so the names stay
// as sharp as the title. The gap is only used while looping an overflow list.
constexpr double kCreditsPixelsPerSecond = 16.0;
constexpr int kCreditsLoopGapPx = 22;
// 1px covers allocation rounding. Anything taller is real clipping.
constexpr int kFitSlackPx = 1;

std::string app_key(const AppEntry& entry) {
  if (entry.pid > 1) return "pid:" + std::to_string(entry.pid);
  return "xid:" + std::to_string(entry.xid);
}

int open_pidfd(pid_t pid) {
#if defined(SYS_pidfd_open)
  return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
#else
  errno = ENOSYS;
  return -1;
#endif
}

int signal_pidfd(int pidfd, int sig) {
#if defined(SYS_pidfd_send_signal)
  return static_cast<int>(::syscall(SYS_pidfd_send_signal, pidfd, sig, nullptr, 0));
#else
  (void)pidfd;
  (void)sig;
  errno = ENOSYS;
  return -1;
#endif
}

void show_notice(Gtk::Window& parent, const std::string& text, const std::string& secondary) {
  Gtk::MessageDialog err(parent, text, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
  if (!secondary.empty()) err.set_secondary_text(secondary);
  err.run();
  err.hide();
}

int positive_height(Gtk::Widget& widget, int fallback) {
  const int allocated = widget.get_allocated_height();
  if (allocated > 1) return allocated;
  int minimum = 0;
  int natural = 0;
  widget.get_preferred_height(minimum, natural);
  if (natural > 1) return natural;
  return fallback;
}

}  // namespace

void set_info_label(Gtk::Label& label, const std::string& text) {
  label.set_text(text);
  label.set_halign(Gtk::ALIGN_START);
  label.set_ellipsize(Pango::ELLIPSIZE_END);
  label.set_max_width_chars(36);
  label.set_tooltip_text(text);
}

namespace {

std::string pieces_on_own_lines(const std::string& text) {
  std::string shown;
  shown.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    if (text.compare(i, 2, "; ") == 0) {
      shown.push_back('\n');
      i += 2;
      continue;
    }
    shown.push_back(text[i]);
    ++i;
  }
  return shown;
}

}  // namespace

void set_wrapping_info_label(Gtk::Label& label, const std::string& text) {
  // Each "; "-joined piece (a second CPU group, a second GPU) starts on
  // its own line, then wraps inside the column. Nothing is ellipsized.
  label.set_text(pieces_on_own_lines(text));
  label.set_halign(Gtk::ALIGN_START);
  label.set_valign(Gtk::ALIGN_START);
  label.set_xalign(0.0f);
  label.set_line_wrap(true);
  label.set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
  label.set_ellipsize(Pango::ELLIPSIZE_NONE);
  label.set_justify(Gtk::JUSTIFY_LEFT);
  // width_chars fixes both the column and the height-for-width probe.
  // Without it a wrapping label measures its minimum height at one
  // character and the window becomes a tall strip. 32 characters stays
  // inside the right-hand column of the 520px window.
  label.set_width_chars(32);
  label.set_max_width_chars(36);
  label.set_tooltip_text(text);
}

SupportersNamesView::SupportersNamesView() {
  set_halign(Gtk::ALIGN_FILL);
  set_hexpand(true);
  set_vexpand(false);
  set_valign(Gtk::ALIGN_START);
  get_style_context()->add_class("supporters-names");
  get_style_context()->add_class("about-info");
  add_events(Gdk::ENTER_NOTIFY_MASK | Gdk::LEAVE_NOTIFY_MASK);
}

SupportersNamesView::~SupportersNamesView() {
  stop_tick();
}

void SupportersNamesView::set_text(const Glib::ustring& text) {
  if (text_ == text) return;
  text_ = text;
  scroll_offset_ = 0.0;
  invalidate_layout();
  queue_resize();
}

void SupportersNamesView::set_max_height(int height) {
  if (height < 1) height = 1;
  if (height == max_height_) return;
  max_height_ = height;
  queue_resize();
}

Gtk::SizeRequestMode SupportersNamesView::get_request_mode_vfunc() const {
  return Gtk::SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

void SupportersNamesView::get_preferred_width_vfunc(int& minimum_width,
                                                   int& natural_width) const {
  // The header row already has a width (window minus the logo). Reporting a
  // tiny natural width lets that row assign the leftover space instead of
  // the unwrapped names pushing the window wider or into the logo.
  minimum_width = 1;
  natural_width = 1;
}

void SupportersNamesView::get_preferred_height_vfunc(int& minimum_height,
                                                    int& natural_height) const {
  int width = get_allocated_width();
  if (width < 40) width = 320;
  get_preferred_height_for_width_vfunc(width, minimum_height, natural_height);
}

void SupportersNamesView::get_preferred_height_for_width_vfunc(
    int width, int& minimum_height, int& natural_height) const {
  // A width this small is the minimum-size probe, not the header allocation.
  // Report one line so that probe cannot turn the window into a tall column.
  if (width < 40) {
    ensure_layout(320);
    int line = single_line_height_ > 0 ? single_line_height_ : layout_pixel_height_;
    if (line < 1) line = 1;
    minimum_height = line;
    natural_height = line;
    return;
  }

  ensure_layout(width);
  int shown = layout_pixel_height_;
  if (shown < 1) shown = 1;
  if (max_height_ > 0 && shown > max_height_) shown = max_height_;
  minimum_height = shown;
  natural_height = shown;
}

void SupportersNamesView::on_size_allocate(Gtk::Allocation& allocation) {
  Gtk::DrawingArea::on_size_allocate(allocation);
  if (allocation.get_width() > 1) ensure_layout(allocation.get_width());
  sync_scroll_policy();
}

void SupportersNamesView::on_style_updated() {
  Gtk::DrawingArea::on_style_updated();
  invalidate_layout();
  queue_resize();
}

void SupportersNamesView::invalidate_layout() {
  layout_.reset();
  layout_width_ = -1;
  layout_pixel_height_ = 0;
  single_line_height_ = 0;
}

void SupportersNamesView::ensure_layout(int width) const {
  if (width < 1) width = 1;
  if (layout_ && layout_width_ == width) return;

  auto layout = const_cast<SupportersNamesView*>(this)->create_pango_layout(text_);
  layout->set_wrap(Pango::WRAP_WORD_CHAR);
  layout->set_ellipsize(Pango::ELLIPSIZE_NONE);
  layout->set_alignment(Pango::ALIGN_RIGHT);
  layout->set_width(width * PANGO_SCALE);

  int pixel_width = 0;
  int pixel_height = 0;
  layout->get_pixel_size(pixel_width, pixel_height);
  (void)pixel_width;

  auto single = const_cast<SupportersNamesView*>(this)->create_pango_layout("Ay");
  single->set_width(-1);
  int single_width = 0;
  int single_height = 0;
  single->get_pixel_size(single_width, single_height);
  (void)single_width;

  layout_ = layout;
  layout_width_ = width;
  layout_pixel_height_ = pixel_height;
  single_line_height_ = single_height > 0 ? single_height : 1;
}

bool SupportersNamesView::names_overflow(int view_height) const {
  // view_height <= 1 is the only "not laid out yet" guard. A cap shorter
  // than one line still crawls; the header is also grown to one line.
  return credits_should_crawl(view_height, layout_pixel_height_, kFitSlackPx);
}

int SupportersNamesView::line_height() const {
  if (single_line_height_ > 1) return single_line_height_;
  auto layout = const_cast<SupportersNamesView*>(this)->create_pango_layout("Ay");
  layout->set_width(-1);
  int width = 0;
  int height = 0;
  layout->get_pixel_size(width, height);
  if (height < 1) height = 1;
  single_line_height_ = height;
  return single_line_height_;
}

void SupportersNamesView::sync_scroll_policy() {
  // Visible fit: keep every name on screen and do not arm a frame callback.
  if (!names_overflow(get_allocated_height())) {
    if (scroll_offset_ != 0.0) {
      scroll_offset_ = 0.0;
      queue_draw();
    }
    last_frame_us_ = 0;
    stop_tick();
    return;
  }
  // A pointer resting on the names pauses the crawl until it leaves.
  if (pointer_over_) {
    last_frame_us_ = 0;
    stop_tick();
    return;
  }
  start_tick();
}

void SupportersNamesView::start_tick() {
  if (tick_id_ != 0) return;
  last_frame_us_ = 0;
  tick_id_ = add_tick_callback(sigc::mem_fun(*this, &SupportersNamesView::on_tick));
}

void SupportersNamesView::stop_tick() {
  if (tick_id_ == 0) return;
  const guint id = tick_id_;
  tick_id_ = 0;
  remove_tick_callback(id);
}

bool SupportersNamesView::on_enter_notify_event(GdkEventCrossing* /*crossing_event*/) {
  pointer_over_ = true;
  last_frame_us_ = 0;
  stop_tick();
  return false;
}

bool SupportersNamesView::on_leave_notify_event(GdkEventCrossing* /*crossing_event*/) {
  pointer_over_ = false;
  last_frame_us_ = 0;
  sync_scroll_policy();
  return false;
}

bool SupportersNamesView::on_tick(const Glib::RefPtr<Gdk::FrameClock>& clock) {
  const int view_h = get_allocated_height();
  // StopAndClearId returns false and clears tick_id_ without
  // remove_tick_callback. GTK drops the callback when the handler returns false.
  if (credits_on_tick(names_overflow(view_h), pointer_over_) == CreditsTickResult::StopAndClearId) {
    if (!pointer_over_) {
      scroll_offset_ = 0.0;
      queue_draw();
    }
    last_frame_us_ = 0;
    tick_id_ = 0;
    return false;
  }

  const gint64 now = clock->get_frame_time();
  if (last_frame_us_ == 0) last_frame_us_ = now;
  double dt = static_cast<double>(now - last_frame_us_) / 1000000.0;
  last_frame_us_ = now;
  if (dt < 0.0) dt = 0.0;
  if (dt > 0.05) dt = 0.05;

  const int origin_before = static_cast<int>(scroll_offset_);
  scroll_offset_ += kCreditsPixelsPerSecond * dt;
  const double cycle =
      static_cast<double>(layout_pixel_height_ + kCreditsLoopGapPx);
  if (cycle > 1.0) {
    while (scroll_offset_ >= cycle) scroll_offset_ -= cycle;
  }
  const int origin_after = static_cast<int>(scroll_offset_);
  // 16 px/s only moves the integer origin on some frames. Skip the rest.
  if (origin_after != origin_before) queue_draw();
  return true;
}

bool SupportersNamesView::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
  const int width = get_allocated_width();
  const int height = get_allocated_height();
  if (width <= 0 || height <= 0) return true;

  ensure_layout(width);

  auto style = get_style_context();
  style->render_background(cr, 0, 0, width, height);

  cr->save();
  cr->rectangle(0, 0, width, height);
  cr->clip();

  const auto color = style->get_color(get_state_flags());
  cr->set_source_rgba(color.get_red(), color.get_green(), color.get_blue(),
                      color.get_alpha());

  const bool overflow = names_overflow(height);
  const int origin = overflow ? -static_cast<int>(scroll_offset_) : 0;

  auto paint_at = [&](int y) {
    cr->save();
    cr->translate(0, y);
    layout_->show_in_cairo_context(cr);
    cr->restore();
  };

  paint_at(origin);
  // Second copy is what makes the crawl loop without a jump. It stays
  // outside the clip until the first copy has moved up, and it is not
  // drawn at all when the names already fit.
  if (overflow) paint_at(origin + layout_pixel_height_ + kCreditsLoopGapPx);

  cr->restore();
  return true;
}

MainWindow::MainWindow() {
  set_title("About This Computer");
  // Reinforce default icon for WMs that ignore gtk_window_set_default_icon_name.
  set_icon_name(APP_ID);
  // v0.2.6: shorter default/min so ~2½ app rows show (not a tall empty list).
  set_default_size(520, 360);
  set_size_request(520, 360);
  set_border_width(0);
  set_resizable(true);
  get_style_context()->add_class("lunduke-about");

  apply_platinum_css();

  int x_fd = -1;
  std::string x_name;
  if (GdkDisplay* display = gdk_display_get_default()) {
    if (GDK_IS_X11_DISPLAY(display)) {
      if (Display* dpy = gdk_x11_display_get_xdisplay(display)) {
        x_fd = XConnectionNumber(dpy);
      }
      if (const char* name = gdk_display_get_name(display)) x_name = name;
    }
  }
  info_ = gather_system_info(x_fd, x_name);

  add(root_);
  root_.get_style_context()->add_class("about-panel");
  root_.set_margin_top(10);
  root_.set_margin_bottom(10);
  root_.set_margin_start(12);
  root_.set_margin_end(12);
  root_.set_spacing(8);

  // 1. Header: LCOS logo left + Supporters block right (right-justified).
  //    The title stays put. Names wrap in the space beside the logo and crawl
  //    upward only when those wrapped lines are taller than this header row.
  auto* header = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 16);
  header->get_style_context()->add_class("about-header");
  header->set_halign(Gtk::ALIGN_FILL);
  header->set_hexpand(true);
  header->set_margin_top(2);
  header->set_margin_bottom(2);

  load_logo();
  logo_.set_halign(Gtk::ALIGN_START);
  logo_.set_valign(Gtk::ALIGN_START);
  logo_.set_margin_top(4);
  logo_.set_margin_bottom(2);

  supporters_box_.get_style_context()->add_class("about-info");
  // FILL so the names wrap across the leftover header width, not a narrow
  // natural width. The title and the name lines stay right-aligned inside it.
  supporters_box_.set_halign(Gtk::ALIGN_FILL);
  supporters_box_.set_valign(Gtk::ALIGN_CENTER);
  supporters_box_.set_hexpand(true);
  supporters_box_.set_spacing(0);

  supporters_title_.set_text("Supporters of LCOS");
  supporters_title_.set_halign(Gtk::ALIGN_END);
  supporters_title_.set_justify(Gtk::JUSTIFY_RIGHT);
  supporters_title_.set_xalign(1.0f);
  // Blank line between title and name (nbsp so the row does not collapse).
  supporters_blank_.set_text(u8"\u00a0");
  supporters_blank_.set_halign(Gtk::ALIGN_END);

  supporters_box_.pack_start(supporters_title_, Gtk::PACK_SHRINK);
  supporters_box_.pack_start(supporters_blank_, Gtk::PACK_SHRINK);
  supporters_box_.pack_start(supporters_names_view_, Gtk::PACK_SHRINK);

  header->pack_start(logo_, Gtk::PACK_SHRINK);
  header->pack_start(supporters_box_, Gtk::PACK_EXPAND_WIDGET);
  root_.pack_start(*header, Gtk::PACK_SHRINK);

  load_supporters();
  update_supporters_cap();
  // queue_resize during size-allocate is dropped, so refine the cap on idle
  // once the logo and title have real heights.
  header->signal_size_allocate().connect(
      sigc::mem_fun(*this, &MainWindow::on_supporters_header_allocate));

  // 2. System info — two columns (left: version + memory; right: CPU + GPU)
  auto* info_cols = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 24);
  info_cols->get_style_context()->add_class("about-info");
  info_cols->set_margin_top(4);
  info_cols->set_margin_bottom(4);
  info_cols->set_halign(Gtk::ALIGN_FILL);
  info_cols->set_hexpand(true);

  auto* left_col = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 2);
  left_col->set_halign(Gtk::ALIGN_START);
  left_col->set_hexpand(true);
  auto* right_col = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 2);
  right_col->set_halign(Gtk::ALIGN_START);
  right_col->set_hexpand(true);

  set_info_label(os_label_, "OS Version:  " + info_.os_pretty);
  set_info_label(mem_label_, "Built-in Memory:  " + info_.total_memory);
  set_wrapping_info_label(cpu_label_, "CPU:  " + info_.cpu_model);
  set_wrapping_info_label(gpu_label_, "GPU:  " + info_.gpu);

  left_col->pack_start(os_label_, Gtk::PACK_SHRINK);
  left_col->pack_start(mem_label_, Gtk::PACK_SHRINK);
  right_col->pack_start(cpu_label_, Gtk::PACK_SHRINK);
  right_col->pack_start(gpu_label_, Gtk::PACK_SHRINK);
  info_cols->pack_start(*left_col, Gtk::PACK_EXPAND_WIDGET);
  info_cols->pack_start(*right_col, Gtk::PACK_EXPAND_WIDGET);
  root_.pack_start(*info_cols, Gtk::PACK_SHRINK);

  // 3. Full-width RAM Used / Free bar (between stats and app list)
  ram_bar_.set_margin_top(2);
  ram_bar_.set_margin_bottom(2);
  root_.pack_start(ram_bar_, Gtk::PACK_SHRINK);
  update_ram_bar();

  // LCOS System is pinned under the bar so the default 520×360 window
  // still shows it. The scroller below is only the other rows.
  system_slot_.set_hexpand(true);
  root_.pack_start(system_slot_, Gtk::PACK_SHRINK);

  // 4. Scrollable app list in beveled frame
  auto* frame = Gtk::make_managed<Gtk::Frame>();
  frame->set_shadow_type(Gtk::SHADOW_IN);
  frame->get_style_context()->add_class("platinum-list-frame");
  frame->set_hexpand(true);
  frame->set_vexpand(true);

  list_scroll_.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
  // Overlay scrollbars stay invisible until hover. This list should show
  // the bar whenever the rows do not fit.
  list_scroll_.set_overlay_scrolling(false);
  list_scroll_.get_style_context()->add_class("platinum-scroll");
  auto tag_bar = [](Gtk::Scrollbar* bar) {
    if (bar) bar->get_style_context()->add_class("platinum-scroll");
  };
  tag_bar(list_scroll_.get_vscrollbar());
  tag_bar(list_scroll_.get_hscrollbar());
  list_scroll_.signal_realize().connect([this, tag_bar]() {
    tag_bar(list_scroll_.get_vscrollbar());
    tag_bar(list_scroll_.get_hscrollbar());
  });
  // Shorter than the old 100px so the pinned LCOS System row still fits
  // inside the 360px window.
  list_scroll_.set_min_content_height(64);

  list_box_.get_style_context()->add_class("platinum-list");
  list_box_.set_homogeneous(false);

  list_scroll_.add(list_box_);
  frame->add(list_scroll_);
  root_.pack_start(*frame, Gtk::PACK_EXPAND_WIDGET);

  // The box allocation is the one that places each row. The scrolled
  // window can allocate before those y positions exist.
  list_box_.signal_size_allocate().connect(
      sigc::mem_fun(*this, &MainWindow::on_list_scroll_allocate));
  signal_map().connect(sigc::mem_fun(*this, &MainWindow::on_mapped));
  signal_unmap().connect(sigc::mem_fun(*this, &MainWindow::on_unmapped));
  signal_window_state_event().connect(
      sigc::mem_fun(*this, &MainWindow::on_window_state));

  show_all();
}

MainWindow::~MainWindow() {
  alive_ = false;
  supporters_idle_conn_.disconnect();
  force_close_refresh_conn_.disconnect();
  probe_conn_.disconnect();
  refresh_conn_.disconnect();
  scroll_restore_conn_.disconnect();
  scroll_idle_conn_.disconnect();
  probe_.reset();
}

void MainWindow::apply_platinum_css() {
  css_ = Gtk::CssProvider::create();
  try {
    css_->load_from_data(kAboutCss);
  } catch (const Glib::Error& e) {
    std::cerr << "CSS error: " << e.what() << std::endl;
  }
  auto screen = Gdk::Screen::get_default();
  if (screen) {
    Gtk::StyleContext::add_provider_for_screen(
        screen, css_, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
}

std::string MainWindow::find_data_file(const std::string& relative) const {
  // Installed data only. A missing file is an empty path so callers keep
  // the generic icon or the built-in supporter list.
  if (relative.empty() || relative.front() == '/' || relative.find("..") != std::string::npos) {
    return {};
  }
  const std::string path = std::string(DATADIR) + "/" + relative;
  std::ifstream test(path);
  if (test.good()) return path;
  return {};
}

void MainWindow::load_logo() {
  // Prefer baked full-mark PNGs (rings + banner). Skip SVG without librsvg.
  // Prefer larger sources first so downscale keeps ring detail.
  const char* paths[] = {
      "pixmaps/lcos-logo-black-preview.png",
      "pixmaps/lcos-logo-black-256.png",
      "pixmaps/lcos-logo-black-128.png",
      "pixmaps/lcos-logo-black.svg",
  };

  Glib::RefPtr<Gdk::Pixbuf> pb;
  for (const char* rel : paths) {
    if (std::string(rel).size() >= 4 &&
        std::string(rel).substr(std::string(rel).size() - 4) == ".svg") {
      continue;
    }
    const std::string path = find_data_file(rel);
    if (path.empty()) continue;
    try {
      // Slightly larger than v0.1 so rings/arcs stay readable
      pb = Gdk::Pixbuf::create_from_file(path, 120, 120, true);
      if (pb) break;
    } catch (...) {
      // try next
    }
  }

  if (pb) {
    logo_.set(pb);
  } else {
    logo_.set_from_icon_name("computer", Gtk::ICON_SIZE_DIALOG);
  }
}

Glib::RefPtr<Gdk::Pixbuf> MainWindow::load_lcos_system_icon() const {
  const char* paths[] = {
      "icons/hicolor/32x32/apps/org.lunduke.AboutThisComputer.png",
      "pixmaps/lcos-outline-32.png",
      "pixmaps/lcos-outline-256.png",
  };
  for (const char* rel : paths) {
    const std::string path = find_data_file(rel);
    if (path.empty()) continue;
    try {
      auto pb = Gdk::Pixbuf::create_from_file(path, 32, 32, true);
      if (pb) return pb;
    } catch (...) {
    }
  }
  // Installed system pixmaps. A missing file leaves the generic icon.
  const char* sys_paths[] = {
      "/usr/share/pixmaps/lcos32.png",
      "/usr/share/pixmaps/lcos-logo.png",
  };
  for (const char* path : sys_paths) {
    try {
      auto pb = Gdk::Pixbuf::create_from_file(path, 32, 32, true);
      if (pb) return pb;
    } catch (...) {
    }
  }
  return {};
}

void MainWindow::update_supporters_cap() {
  // Names may use the header row beside the logo, under the fixed title
  // and the blank line. They must not make that row taller than the logo.
  // load_logo() scales the mark to 120px; use that until the image is allocated.
  const int logo_px = positive_height(logo_, 120);
  const int logo_span = logo_px + logo_.get_margin_top() + logo_.get_margin_bottom();
  const int title_h = positive_height(supporters_title_, 16);
  const int blank_h = positive_height(supporters_blank_, 16);

  int max_names = logo_span - title_h - blank_h;
  const int line = supporters_names_view_.line_height();
  // A cap shorter than one line clips the names. Grow the header instead.
  if (line > 1 && max_names < line) max_names = line;
  if (max_names < 1) max_names = 1;
  supporters_names_view_.set_max_height(max_names);
}

void MainWindow::on_supporters_header_allocate(Gtk::Allocation& /*allocation*/) {
  if (!alive_ || supporters_cap_update_queued_) return;
  supporters_cap_update_queued_ = true;
  supporters_idle_conn_.disconnect();
  supporters_idle_conn_ = Glib::signal_idle().connect([this]() {
    supporters_cap_update_queued_ = false;
    if (!alive_) return false;
    update_supporters_cap();
    return false;
  });
}

void MainWindow::load_supporters() {
  // One entry per non-empty line. Join with comma-space so the header reads
  // "Fuzzy", Steven P., Chris Hammond — including whatever punctuation the
  // line already has (Fuzzy stays quoted).
  std::string path = find_data_file("supporters.txt");
  std::ifstream in(path);
  std::string names;
  if (in) {
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
      if (!names.empty()) names += ", ";
      names += line;
    }
  }
  if (names.empty()) {
    names = "\"Fuzzy\", Steven P., Chris Hammond, Mike Beasley";
  }
  supporters_names_view_.set_text(names);
}

void MainWindow::update_ram_bar() {
  refresh_memory_usage(info_);
  if (!info_.memory_known) {
    info_.total_memory = "Unknown";
    info_.total_memory_kb = 0;
    info_.used_memory_kb = 0;
    info_.available_memory_kb = 0;
    set_info_label(mem_label_, "Built-in Memory:  Unknown");
    ram_bar_.set_unknown();
    return;
  }
  const MemoryReadout readout =
      format_memory_readout(info_.used_memory_kb, info_.total_memory_kb);
  info_.total_memory = readout.total;
  set_info_label(mem_label_, "Built-in Memory:  " + readout.total);
  ram_bar_.set_memory(info_.used_memory_kb, info_.total_memory_kb, readout.used,
                      readout.free);
}

void MainWindow::ensure_system_row(const AppEntry& entry) {
  if (!system_row_) {
    system_row_ = Gtk::manage(new AppRow(entry));
    system_slot_.pack_start(*system_row_, Gtk::PACK_SHRINK);
    system_row_->show_all();
  } else {
    system_row_->update_entry(entry);
  }
}

AppEntry MainWindow::make_system_entry(long system_kb, const std::string& tooltip) {
  AppEntry system_entry;
  system_entry.name = "LCOS System";
  system_entry.pid = 0;
  system_entry.rss_kb = system_kb;
  system_entry.rss_known = true;
  system_entry.tooltip = tooltip;
  if (!system_icon_) system_icon_ = load_lcos_system_icon();
  system_entry.icon = system_icon_;
  system_entry.protected_app = true;
  system_entry.protect_reason = "LCOS System";
  return system_entry;
}

void MainWindow::sync_app_rows(const std::vector<AppEntry>& apps, bool allow_structure) {
  std::map<std::string, const AppEntry*> by_key;
  std::vector<std::string> new_order;
  new_order.reserve(apps.size());
  for (const auto& app : apps) {
    const std::string key = app_key(app);
    if (!by_key.emplace(key, &app).second) continue;
    new_order.push_back(key);
  }
  std::set<std::string> new_keys(new_order.begin(), new_order.end());

  std::set<std::string> old_keys;
  for (const auto& item : app_rows_) old_keys.insert(item.key);
  const bool same_set = old_keys == new_keys;

  auto update_matching = [&]() {
    for (auto& item : app_rows_) {
      const auto it = by_key.find(item.key);
      if (it == by_key.end() || !item.row) continue;
      item.row->update_entry(*it->second);
    }
  };

  // A Force Close confirm dialog runs a nested loop. Destroying the row
  // here would free the menu item whose activate handler is still running.
  if (!allow_structure) {
    update_matching();
    return;
  }

  bool restore_scroll = false;
  if (!same_set) {
    capture_scroll_anchor();
    restore_scroll = true;
  }

  if (!same_set) {
    for (size_t i = 0; i < app_rows_.size();) {
      const bool in_next = new_keys.count(app_rows_[i].key) != 0;
      const bool posted = app_rows_[i].row && app_rows_[i].row->menu_posted();
      // A posted Force Close menu keeps its row until deactivate, even when
      // the pid has left the snapshot. The dialog freeze is the early return
      // above; menu_posted is per row.
      if (!may_delete_row(in_next, posted, false)) {
        ++i;
        continue;
      }
      if (app_rows_[i].sep) list_box_.remove(*app_rows_[i].sep);
      if (app_rows_[i].row) list_box_.remove(*app_rows_[i].row);
      app_rows_.erase(app_rows_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    std::set<std::string> have;
    for (const auto& item : app_rows_) have.insert(item.key);
    for (const auto& key : new_order) {
      if (have.count(key)) continue;
      const AppEntry& app = *by_key[key];
      AppListItem item;
      item.key = key;
      item.row = Gtk::manage(new AppRow(app));
      item.row->set_force_close_handler(
          [this](const AppEntry& e) { on_force_close(e); });
      item.sep = Gtk::make_managed<Gtk::Separator>(Gtk::ORIENTATION_HORIZONTAL);
      list_box_.pack_start(*item.row, Gtk::PACK_SHRINK);
      list_box_.pack_start(*item.sep, Gtk::PACK_SHRINK);
      item.row->show_all();
      item.sep->show();
      app_rows_.push_back(item);
      have.insert(key);
    }
  }

  update_matching();

  // Same apps: leave the rows where the user is looking, even if RSS rank
  // changed. A real membership change may be re-sorted by RSS.
  if (!same_set) {
    std::map<std::string, AppListItem> pool;
    for (auto& item : app_rows_) pool.emplace(item.key, item);
    std::vector<std::string> desired = new_order;
    for (const auto& item : app_rows_) {
      if (!new_keys.count(item.key)) desired.push_back(item.key);
    }
    std::vector<AppListItem> ordered;
    ordered.reserve(desired.size());
    int pos = 0;
    for (const auto& key : desired) {
      auto it = pool.find(key);
      if (it == pool.end() || !it->second.row) continue;
      list_box_.reorder_child(*it->second.row, pos++);
      if (it->second.sep) list_box_.reorder_child(*it->second.sep, pos++);
      ordered.push_back(it->second);
    }
    app_rows_ = std::move(ordered);
  }

  if (restore_scroll) arm_scroll_restore();
}

void MainWindow::on_list_scroll_allocate(Gtk::Allocation& /*allocation*/) {
  if (!pending_scroll_restore_) return;
  scroll_layout_since_arm_ = true;
  if (apply_pending_scroll()) finish_scroll_restore();
}

void MainWindow::capture_scroll_anchor() {
  anchor_key_.clear();
  anchor_delta_ = 0.0;
  anchor_fallback_ = 0.0;
  auto adj = list_scroll_.get_vadjustment();
  if (!adj) return;
  anchor_fallback_ = adj->get_value();
  const double value = anchor_fallback_;
  for (const auto& item : app_rows_) {
    if (!item.row) continue;
    const int y = item.row->get_allocation().get_y();
    const int h = item.row->get_allocation().get_height();
    if (h <= 1) continue;
    if (static_cast<double>(y) + h > value + 0.5) {
      anchor_key_ = item.key;
      anchor_delta_ = value - static_cast<double>(y);
      return;
    }
  }
}

void MainWindow::finish_scroll_restore() {
  pending_scroll_restore_ = false;
  scroll_layout_since_arm_ = false;
  scroll_restore_conn_.disconnect();
  scroll_idle_conn_.disconnect();
}

void MainWindow::arm_scroll_restore() {
  pending_scroll_restore_ = true;
  scroll_layout_since_arm_ = false;
  scroll_restore_tries_ = 0;
  // reorder_child queues a resize. Ask again so a same-sized list still
  // allocates and the anchor can be read from the new row positions.
  list_box_.queue_resize();
  scroll_restore_conn_.disconnect();
  if (auto adj = list_scroll_.get_vadjustment()) {
    scroll_restore_conn_ = adj->signal_changed().connect([this]() {
      if (apply_pending_scroll()) finish_scroll_restore();
    });
  }
  scroll_idle_conn_.disconnect();
  scroll_idle_conn_ = Glib::signal_idle().connect([this]() {
    if (!pending_scroll_restore_) return false;
    if (apply_pending_scroll()) {
      finish_scroll_restore();
      return false;
    }
    // Do not clamp onto a short upper and then forget the anchor.
    if (++scroll_restore_tries_ > 30) {
      finish_scroll_restore();
      return false;
    }
    return true;
  });
}

bool MainWindow::apply_pending_scroll() {
  if (!pending_scroll_restore_ || !scroll_layout_since_arm_) return false;
  auto adj = list_scroll_.get_vadjustment();
  if (!adj) return false;
  const double upper = adj->get_upper();
  const double page = adj->get_page_size();
  Gtk::Widget* target = nullptr;
  for (const auto& item : app_rows_) {
    if (item.key == anchor_key_ && item.row) target = item.row;
  }
  if (target) {
    const int y = target->get_allocation().get_y();
    const int h = target->get_allocation().get_height();
    if (h <= 1) return false;
    const auto restored = restore_anchored_scroll(
        true, static_cast<double>(y), anchor_delta_, static_cast<double>(y) + h, upper, page);
    if (!restored) return false;
    adj->set_value(*restored);
    return true;
  }
  // The anchored row left the list. The post-layout upper is the real one.
  if (upper <= 1.0) return false;
  adj->set_value(clamp_scroll_value(anchor_fallback_, upper, page));
  return true;
}

void MainWindow::apply_app_snapshot(std::vector<AppEntry> apps, bool x11) {
  update_ram_bar();

  if (!x11) {
    AppEntry needs;
    needs.name = "Needs X11";
    needs.tooltip = "This view needs an X11 display.";
    needs.protected_app = true;
    needs.protect_reason = "This view needs X11";
    needs.rss_known = false;
    needs.pid = 0;
    apps.insert(apps.begin(), std::move(needs));
  }

  // Each pid's anonymous charge is subtracted once. Pss_Anon already splits
  // shared anonymous pages. A negative remainder is kept in the tooltip and
  // the row shows 0. Missing MemTotal is not a measurement: the row is an
  // em dash and the bar stays empty.
  AppEntry system_entry;
  if (!info_.memory_known) {
    system_entry = make_system_entry(0, "LCOS System");
    system_entry.rss_known = false;
    system_entry.rss_kb = 0;
  } else {
    std::vector<ProcPin> pins;
    for (const auto& app : apps) {
      if (!app.rss_known) continue;
      pins.insert(pins.end(), app.kill_pins.begin(), app.kill_pins.end());
    }
    const long long apps_rss = sum_rss_once(pins);
    const SystemRemainder remainder = system_remainder_kb(info_.used_memory_kb, apps_rss);
    std::string system_tip = "LCOS System";
    if (remainder.clamped) {
      system_tip += "\nUnclamped remainder: " + std::to_string(remainder.raw_kb) + " kB";
    }
    system_entry = make_system_entry(remainder.shown_kb, system_tip);
  }
  ensure_system_row(system_entry);

  sync_app_rows(apps, force_close_depth_ == 0);
  last_snapshot_ = std::chrono::steady_clock::now();
}

void MainWindow::arm_force_close_refresh() {
  force_close_refresh_conn_.disconnect();
  force_close_refresh_conn_ = Glib::signal_timeout().connect_seconds(
      [this]() {
        if (!alive_) return false;
        schedule_refresh();
        return false;
      },
      1);
}

bool MainWindow::signal_pinned_pid(pid_t pid, unsigned long long expected_start, bool other_row,
                                   bool require_comm, const std::string& expected_comm,
                                   std::string& why) {
  why.clear();
  if (pid <= 1 || pid == getpid()) {
    why = "That PID cannot be signalled.";
    return false;
  }
  if (other_row) {
    why = "That PID belongs to another row and was not signalled.";
    return false;
  }
  const int dirfd = open_proc_pid_dir(pid);
  if (dirfd < 0) {
    why = "Already exited.";
    return false;
  }
  const ProcSnapshot now = read_proc_snapshot_at(dirfd);
  if (!may_signal_pinned_pid(pid, getpid(), expected_start, now.ok, now.start_ticks, false)) {
    ::close(dirfd);
    why = now.ok ? "That PID is a different process now and was not signalled."
                 : "Already exited.";
    return false;
  }
  if (require_comm) {
    ProcSnapshot pinned;
    pinned.ok = true;
    pinned.comm = expected_comm;
    pinned.start_ticks = expected_start;
    if (!proc_identity_matches(pinned, now)) {
      ::close(dirfd);
      why = "That PID is a different process now and was not signalled.";
      return false;
    }
  }
  const int pidfd = open_pidfd(pid);
  const int pidfd_err = errno;
  const ProcSnapshot again = read_proc_snapshot_at(dirfd);
  ::close(dirfd);
  if (!may_signal_pinned_pid(pid, getpid(), expected_start, again.ok, again.start_ticks, false) ||
      (require_comm && again.ok && again.comm != expected_comm)) {
    if (pidfd >= 0) ::close(pidfd);
    why = again.ok ? "That PID changed before it could be closed and was not signalled."
                   : "Already exited.";
    return false;
  }
  const PidfdOpenAction opened = pidfd_open_action(pidfd, pidfd_err);
  if (opened == PidfdOpenAction::AlreadyExited) {
    why = "Already exited.";
    return false;
  }
  if (opened == PidfdOpenAction::SendOnPidfd) {
    if (signal_pidfd(pidfd, SIGKILL) == 0) {
      ::close(pidfd);
      return true;
    }
    const int err = errno;
    ::close(pidfd);
    const PidfdSignalFailure failure = pidfd_signal_failure(err);
    if (failure == PidfdSignalFailure::AlreadyExited) {
      why = "Already exited.";
      return false;
    }
    if (failure == PidfdSignalFailure::ReportErrno) {
      why = force_close_errno_message(pid, err);
      return false;
    }
    // ENOSYS: the pin could not be signalled. kill() is the same fallback
    // as a kernel without pidfd, after the start-time check above.
  }
  if (::kill(pid, SIGKILL) != 0) {
    if (errno == ESRCH) {
      why = "Already exited.";
      return false;
    }
    why = force_close_errno_message(pid, errno);
    return false;
  }
  return true;
}

void MainWindow::on_force_close(const AppEntry& entry_ref) {
  // Snapshot before the dialog. dlg.run() nests the main loop, so a refresh
  // must not destroy the AppRow that owns entry_ref.
  const AppEntry entry = entry_ref;
  if (entry.protected_app || entry.pid <= 1 || entry.pid == getpid()) return;

  ProcSnapshot pinned;
  pinned.ok = entry.identity_ok;
  pinned.comm = entry.comm;
  pinned.start_ticks = entry.start_ticks;
  if (!entry.identity_ok) {
    show_notice(*this, "Already exited.",
                "This row has no command and start time from the last refresh.");
    arm_force_close_refresh();
    return;
  }
  const ProcSnapshot before = read_proc_snapshot(entry.pid);
  if (!before.ok) {
    show_notice(*this, "Already exited.",
                "PID " + std::to_string(entry.pid) + " is no longer running.");
    arm_force_close_refresh();
    return;
  }
  if (!proc_identity_matches(pinned, before)) {
    show_notice(*this, "Process changed.",
                "PID " + std::to_string(entry.pid) +
                    " is a different process than the one listed. It was not closed.");
    arm_force_close_refresh();
    return;
  }

  const bool window_still_there = window_xid_matches_pid(entry.xid, entry.pid);
  const ForceClosePrompt prompt = force_close_prompt(
      entry.name, entry.comm, entry.pid, entry.class_name, window_still_there);
  Gtk::MessageDialog dlg(*this, prompt.primary, false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_NONE,
                         true);
  dlg.set_secondary_text(prompt.secondary);
  dlg.add_button("Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("Force Close", Gtk::RESPONSE_ACCEPT);
  dlg.set_default_response(Gtk::RESPONSE_CANCEL);

  ++force_close_depth_;
  const int response = dlg.run();
  dlg.hide();
  --force_close_depth_;
  if (response != Gtk::RESPONSE_ACCEPT) return;

  if (entry.pid <= 1 || entry.pid == getpid()) {
    show_notice(*this, "Could not force-close process.", "That PID cannot be signalled.");
    return;
  }

  std::set<pid_t> other_rows;
  for (const auto& item : app_rows_) {
    if (!item.row) continue;
    const pid_t pid = item.row->pid();
    if (pid > 1 && pid != entry.pid) other_rows.insert(pid);
  }

  const ForceCloseOrder order = force_close_signal_order(entry.pid, entry.kill_pins);
  std::string why;
  const bool root_ok =
      signal_pinned_pid(order.root, entry.start_ticks, false, true, entry.comm, why);
  if (!force_close_should_signal_helpers(root_ok)) {
    if (why == "Already exited.") {
      show_notice(*this, "Already exited.",
                  "PID " + std::to_string(entry.pid) + " is no longer running.");
    } else {
      show_notice(*this, "Could not force-close process.", why);
    }
    arm_force_close_refresh();
    return;
  }

  HelperCloseReport report;
  helper_close_note(report, order.root, true, "");
  for (pid_t helper : order.helpers) {
    const auto pin = std::find_if(entry.kill_pins.begin(), entry.kill_pins.end(),
                                  [helper](const ProcPin& item) { return item.pid == helper; });
    if (pin == entry.kill_pins.end()) continue;
    const bool other = other_rows.count(helper) != 0;
    std::string helper_why;
    const bool ok =
        signal_pinned_pid(helper, pin->start_ticks, other, false, "", helper_why);
    helper_close_note(report, helper, ok, helper_why);
  }
  const std::string extra = helper_close_message(report);
  if (!extra.empty()) {
    show_notice(*this, "Could not force-close every helper.", extra);
  }
  arm_force_close_refresh();
}

void MainWindow::start_refresh_timer() {
  if (refresh_conn_.connected()) return;
  refresh_conn_ = Glib::signal_timeout().connect(
      sigc::mem_fun(*this, &MainWindow::on_refresh_tick), 3000);
}

void MainWindow::stop_refresh_work() {
  refresh_conn_.disconnect();
  probe_conn_.disconnect();
  probe_.reset();
  refresh_running_ = false;
  refresh_followup_ = false;
}

void MainWindow::on_mapped() {
  auto window = get_window();
  if (window && (window->get_state() & Gdk::WINDOW_STATE_ICONIFIED)) {
    iconified_ = true;
    return;
  }
  iconified_ = false;
  start_refresh_timer();
  schedule_refresh();
}

void MainWindow::on_unmapped() { stop_refresh_work(); }

bool MainWindow::on_window_state(GdkEventWindowState* event) {
  if (!event) return false;
  const bool now_iconified =
      (event->new_window_state & GDK_WINDOW_STATE_ICONIFIED) != 0;
  if (now_iconified) {
    iconified_ = true;
    stop_refresh_work();
  } else if ((event->changed_mask & GDK_WINDOW_STATE_ICONIFIED) != 0) {
    iconified_ = false;
    if (get_mapped()) {
      start_refresh_timer();
      schedule_refresh();
    }
  }
  return false;
}

void MainWindow::schedule_refresh() {
  if (!alive_ || iconified_ || !get_mapped()) return;
  if (refresh_running_) {
    // Explicit callers (map, Force Close) may run once more when the probe
    // finishes. The 3s timer never reaches this branch while a probe runs.
    refresh_followup_ = true;
    return;
  }
  refresh_running_ = true;
  refresh_followup_ = false;
  probe_ = std::make_unique<AppListRefresh>(getpid());
  probe_conn_ = Glib::signal_idle().connect(sigc::mem_fun(*this, &MainWindow::on_probe_idle));
}

bool MainWindow::on_probe_idle() {
  if (!alive_ || !probe_) {
    refresh_running_ = false;
    return false;
  }
  if (probe_->step()) return true;

  std::vector<AppEntry> entries = probe_->entries();
  const bool x11 = probe_->on_x11();
  probe_.reset();
  apply_app_snapshot(std::move(entries), x11);

  if (refresh_followup_ && alive_ && get_mapped() && !iconified_) {
    refresh_followup_ = false;
    probe_ = std::make_unique<AppListRefresh>(getpid());
    return true;
  }
  refresh_followup_ = false;
  refresh_running_ = false;
  return false;
}

bool MainWindow::on_refresh_tick() {
  if (!alive_ || iconified_ || !get_mapped()) return true;
  if (refresh_running_) return true;
  if (last_snapshot_.time_since_epoch().count() != 0 &&
      std::chrono::steady_clock::now() - last_snapshot_ < std::chrono::seconds(3)) {
    return true;
  }
  schedule_refresh();
  return true;
}

}  // namespace lundukeabout
