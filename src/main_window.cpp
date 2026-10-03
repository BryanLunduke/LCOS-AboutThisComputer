// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.hpp"
#include "app_row.hpp"
#include "config.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <map>
#include <set>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <cerrno>
#include <cstring>

namespace lundukeabout {
namespace {

// Empty-list copy retained for reference; UI always shows LCOS System instead.
[[maybe_unused]] constexpr const char* kNoRunningSoftware = "No Running Software.";

// Theme-aware chrome: no hardcoded platinum greys for window/panel.
// List frame stays white/readable. Labels inherit dark readable theme fg.
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
}
.platinum-list row,
.platinum-list .app-row-bg {
  background-color: #ffffff;
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
scrollbar.platinum-scroll {
  background-color: @theme_bg_color;
}
scrollbar.platinum-scroll slider {
  background-color: #5a7ec8;
  border-radius: 0;
  min-width: 14px;
  border: 1px solid #2a4a8a;
}
scrollbar.platinum-scroll button {
  background-color: @theme_bg_color;
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

struct ProcIdentity {
  bool ok = false;
  std::string comm;
  unsigned long long start_ticks = 0;
};

// /proc/<pid>/stat: pid (comm) state ... starttime is field 22.
// comm is everything between the first '(' and the last ')'.
ProcIdentity read_proc_identity(pid_t pid) {
  ProcIdentity id;
  if (pid <= 1) return id;
  std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
  std::string stat;
  if (!std::getline(in, stat)) return id;
  const auto lparen = stat.find('(');
  const auto rparen = stat.rfind(')');
  if (lparen == std::string::npos || rparen == std::string::npos || rparen <= lparen) {
    return id;
  }
  id.comm = stat.substr(lparen + 1, rparen - lparen - 1);
  std::istringstream iss(stat.substr(rparen + 1));
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

SupportersNamesView::SupportersNamesView() {
  set_halign(Gtk::ALIGN_FILL);
  set_hexpand(true);
  set_vexpand(false);
  set_valign(Gtk::ALIGN_START);
  get_style_context()->add_class("supporters-names");
  get_style_context()->add_class("about-info");
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

  auto single = const_cast<SupportersNamesView*>(this)->create_pango_layout(text_);
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
  // A strip shorter than one line is an unfinished header measure (the logo
  // size is not known yet). The names are not actually too tall to read, so
  // do not arm the crawl.
  if (view_height <= 1 || layout_pixel_height_ <= 0) return false;
  if (single_line_height_ > 1 &&
      view_height + kFitSlackPx < single_line_height_) {
    return false;
  }
  return layout_pixel_height_ > view_height + kFitSlackPx;
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

bool SupportersNamesView::on_tick(const Glib::RefPtr<Gdk::FrameClock>& clock) {
  const int view_h = get_allocated_height();
  if (!names_overflow(view_h)) {
    scroll_offset_ = 0.0;
    last_frame_us_ = 0;
    tick_id_ = 0;
    queue_draw();
    return false;
  }

  const gint64 now = clock->get_frame_time();
  if (last_frame_us_ == 0) last_frame_us_ = now;
  double dt = static_cast<double>(now - last_frame_us_) / 1000000.0;
  last_frame_us_ = now;
  if (dt < 0.0) dt = 0.0;
  if (dt > 0.05) dt = 0.05;

  scroll_offset_ += kCreditsPixelsPerSecond * dt;
  const double cycle =
      static_cast<double>(layout_pixel_height_ + kCreditsLoopGapPx);
  if (cycle > 1.0) {
    while (scroll_offset_ >= cycle) scroll_offset_ -= cycle;
  }

  queue_draw();
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

  info_ = gather_system_info();

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

  os_label_.set_text("OS Version:  " + info_.os_pretty);
  os_label_.set_halign(Gtk::ALIGN_START);
  mem_label_.set_text("Built-in Memory:  " + info_.total_memory);
  mem_label_.set_halign(Gtk::ALIGN_START);
  cpu_label_.set_text("CPU:  " + info_.cpu_model);
  cpu_label_.set_halign(Gtk::ALIGN_START);
  cpu_label_.set_ellipsize(Pango::ELLIPSIZE_END);
  cpu_label_.set_max_width_chars(36);
  gpu_label_.set_text("GPU:  " + info_.gpu);
  gpu_label_.set_halign(Gtk::ALIGN_START);
  gpu_label_.set_ellipsize(Pango::ELLIPSIZE_END);
  gpu_label_.set_max_width_chars(36);

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

  // 4. Scrollable app list in beveled frame
  auto* frame = Gtk::make_managed<Gtk::Frame>();
  frame->set_shadow_type(Gtk::SHADOW_IN);
  frame->get_style_context()->add_class("platinum-list-frame");
  frame->set_hexpand(true);
  frame->set_vexpand(true);

  list_scroll_.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
  list_scroll_.get_style_context()->add_class("platinum-scroll");
  list_scroll_.set_min_content_height(100);

  list_box_.get_style_context()->add_class("platinum-list");
  list_box_.set_homogeneous(false);

  list_scroll_.add(list_box_);
  frame->add(list_scroll_);
  root_.pack_start(*frame, Gtk::PACK_EXPAND_WIDGET);

  show_all();

  // First enumerate often races window mapping; refresh once idle, then periodically.
  Glib::signal_idle().connect_once([this]() { refresh_app_list(); });
  refresh_conn_ = Glib::signal_timeout().connect(
      sigc::mem_fun(*this, &MainWindow::on_refresh_tick), 3000);
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
  // Prefer installed DATADIR, then SOURCE_DATADIR (build/run from tree),
  // then relative to cwd.
  const char* candidates[] = {DATADIR, SOURCE_DATADIR, "data", nullptr};
  for (int i = 0; candidates[i]; ++i) {
    std::string path = std::string(candidates[i]) + "/" + relative;
    std::ifstream test(path);
    if (test.good()) return path;
  }
  return relative;
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
    std::string path = find_data_file(rel);
    try {
      if (std::string(rel).size() >= 4 &&
          std::string(rel).substr(std::string(rel).size() - 4) == ".svg") {
        continue;
      }
      // Slightly larger than v0.1 so rings/arcs stay readable
      pb = Gdk::Pixbuf::create_from_file(path, 120, 120, true);
      if (pb) break;
    } catch (...) {
      // try next
    }
  }

  if (!pb) {
    try {
      pb = Gdk::Pixbuf::create_from_file(
          "/workspace/artifacts/lcos-logo-black-preview.png", 120, 120, true);
    } catch (...) {
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
    std::string path = find_data_file(rel);
    try {
      auto pb = Gdk::Pixbuf::create_from_file(path, 32, 32, true);
      if (pb) return pb;
    } catch (...) {
    }
  }
  // Live recipe / system pixmaps fallback
  const char* sys_paths[] = {
      "/usr/share/pixmaps/lcos32.png",
      "/workspace/lcos-live-07/config/includes.chroot/usr/share/pixmaps/lcos32.png",
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
  if (max_names < 1) max_names = 1;
  supporters_names_view_.set_max_height(max_names);
}

void MainWindow::on_supporters_header_allocate(Gtk::Allocation& /*allocation*/) {
  if (supporters_cap_update_queued_) return;
  supporters_cap_update_queued_ = true;
  Glib::signal_idle().connect_once([this]() {
    supporters_cap_update_queued_ = false;
    update_supporters_cap();
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
  mem_label_.set_text("Built-in Memory:  " + info_.total_memory);
  ram_bar_.set_memory(info_.used_memory_kb, info_.total_memory_kb);
}

AppEntry MainWindow::make_system_entry(long system_kb) {
  AppEntry system_entry;
  system_entry.name = "LCOS System";
  system_entry.pid = 0;
  system_entry.rss_kb = system_kb;
  if (!system_icon_) system_icon_ = load_lcos_system_icon();
  system_entry.icon = system_icon_;
  system_entry.protected_app = true;
  system_entry.protect_reason = "system";
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

  double saved_scroll = 0.0;
  bool restore_scroll = false;
  if (!same_set) {
    if (auto adj = list_scroll_.get_vadjustment()) {
      saved_scroll = adj->get_value();
      restore_scroll = true;
    }
  }

  if (!same_set) {
    for (size_t i = 0; i < app_rows_.size();) {
      if (new_keys.count(app_rows_[i].key)) {
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
    std::vector<AppListItem> ordered;
    ordered.reserve(new_order.size());
    int pos = 0;
    for (const auto& key : new_order) {
      auto it = pool.find(key);
      if (it == pool.end() || !it->second.row) continue;
      list_box_.reorder_child(*it->second.row, pos++);
      if (it->second.sep) list_box_.reorder_child(*it->second.sep, pos++);
      ordered.push_back(it->second);
    }
    if (system_row_) list_box_.reorder_child(*system_row_, pos);
    app_rows_ = std::move(ordered);
  }

  if (restore_scroll) {
    if (auto adj = list_scroll_.get_vadjustment()) {
      const double max = std::max(0.0, adj->get_upper() - adj->get_page_size());
      adj->set_value(std::min(saved_scroll, max));
    }
  }
}

void MainWindow::refresh_app_list() {
  update_ram_bar();

  auto apps = enumerate_graphical_apps(getpid());

  // Per-app rows report RssAnon only (private heap). Sum those, then residual
  // physical used (MemTotal - MemAvailable) minus that sum is LCOS System —
  // so RssFile / shared library pages and RssShmem are counted once in System,
  // not double-counted on every GTK app. Clamp if residual would go negative.
  long apps_rss = 0;
  for (const auto& a : apps) apps_rss += a.rss_kb;

  long system_kb = info_.used_memory_kb - apps_rss;
  if (system_kb < 0) system_kb = 0;

  const AppEntry system_entry = make_system_entry(system_kb);
  if (!system_row_) {
    system_row_ = Gtk::manage(new AppRow(system_entry));
    list_box_.pack_start(*system_row_, Gtk::PACK_SHRINK);
    system_row_->show_all();
  } else {
    system_row_->update_entry(system_entry);
  }

  sync_app_rows(apps, force_close_depth_ == 0);
}

bool MainWindow::entry_still_listed(const AppEntry& entry) const {
  if (entry.pid <= 1) return false;
  const std::string key = app_key(entry);
  const auto apps = enumerate_graphical_apps(getpid());
  for (const auto& app : apps) {
    if (app_key(app) == key) return true;
  }
  return false;
}

void MainWindow::on_force_close(const AppEntry& entry_ref) {
  // Snapshot before the dialog. dlg.run() nests the main loop, so the 3s
  // refresh can destroy the AppRow that owns entry_ref. Cancel and confirm
  // both use this copy only.
  const AppEntry entry = entry_ref;
  if (entry.protected_app || entry.pid <= 1 || entry.pid == getpid()) return;

  const ProcIdentity before = read_proc_identity(entry.pid);
  if (!before.ok) return;

  Gtk::MessageDialog dlg(*this,
                         "Force Close \"" + entry.name + "\"?",
                         false,
                         Gtk::MESSAGE_WARNING,
                         Gtk::BUTTONS_NONE,
                         true);
  dlg.set_secondary_text(
      "This will send SIGKILL to PID " + std::to_string(entry.pid) +
      ".\nUnsaved work in that application may be lost.");
  dlg.add_button("Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("Force Close", Gtk::RESPONSE_ACCEPT);
  dlg.set_default_response(Gtk::RESPONSE_CANCEL);

  ++force_close_depth_;
  const int response = dlg.run();
  dlg.hide();
  --force_close_depth_;
  if (response != Gtk::RESPONSE_ACCEPT) return;

  // pid 0 is LCOS System and kill(0) signals the whole process group.
  if (entry.pid <= 1 || entry.pid == getpid()) return;
  if (!entry_still_listed(entry)) return;

  const ProcIdentity after = read_proc_identity(entry.pid);
  if (!after.ok || after.comm != before.comm || after.start_ticks != before.start_ticks) {
    return;
  }

  if (kill(entry.pid, SIGKILL) != 0) {
    const int kill_errno = errno;
    Gtk::MessageDialog err(*this, "Could not force-close process.", false,
                           Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
    err.set_secondary_text(std::strerror(kill_errno));
    err.run();
    err.hide();
  }
  // Brief delay then refresh
  Glib::signal_timeout().connect_seconds(
      [this]() {
        refresh_app_list();
        return false;
      },
      1);
}

bool MainWindow::on_refresh_tick() {
  refresh_app_list();
  return true;
}

}  // namespace lundukeabout
