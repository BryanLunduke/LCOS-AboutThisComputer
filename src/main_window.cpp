// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.hpp"
#include "app_row.hpp"
#include "config.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
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

}  // namespace

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
  //    supporters_box_ is the hook point for a future upward credits scroll.
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
  supporters_box_.set_halign(Gtk::ALIGN_END);
  supporters_box_.set_valign(Gtk::ALIGN_CENTER);
  supporters_box_.set_hexpand(true);
  supporters_box_.set_spacing(0);

  supporters_title_.set_text("Supporters of LCOS");
  supporters_title_.set_halign(Gtk::ALIGN_END);
  supporters_title_.set_justify(Gtk::JUSTIFY_RIGHT);
  // Blank line between title and name (nbsp so the row does not collapse).
  supporters_blank_.set_text(u8"\u00a0");
  supporters_blank_.set_halign(Gtk::ALIGN_END);
  supporters_names_.set_halign(Gtk::ALIGN_END);
  supporters_names_.set_justify(Gtk::JUSTIFY_RIGHT);

  supporters_box_.pack_start(supporters_title_, Gtk::PACK_SHRINK);
  supporters_box_.pack_start(supporters_blank_, Gtk::PACK_SHRINK);
  supporters_box_.pack_start(supporters_names_, Gtk::PACK_SHRINK);

  header->pack_start(logo_, Gtk::PACK_SHRINK);
  header->pack_start(supporters_box_, Gtk::PACK_EXPAND_WIDGET);
  root_.pack_start(*header, Gtk::PACK_SHRINK);

  load_supporters();

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

void MainWindow::load_supporters() {
  // Preserve blank lines from supporters.txt so the right block shows
  // title / blank / names with intentional gaps (e.g. Fuzzy, blank, placeholder).
  std::string path = find_data_file("supporters.txt");
  std::ifstream in(path);
  std::string names;
  if (in) {
    std::string line;
    bool started = false;
    while (std::getline(in, line)) {
      if (!line.empty() && line[0] == '#') continue;
      if (!started) {
        if (line.empty()) continue;  // skip leading blanks after comments
        started = true;
      } else {
        names += '\n';
      }
      names += line;
    }
  }
  if (names.empty()) {
    names = "[Your Name Here]";
  }
  supporters_names_.set_text(names);
}

void MainWindow::update_ram_bar() {
  refresh_memory_usage(info_);
  mem_label_.set_text("Built-in Memory:  " + info_.total_memory);
  ram_bar_.set_memory(info_.used_memory_kb, info_.total_memory_kb);
}

void MainWindow::refresh_app_list() {
  update_ram_bar();

  // Clear existing rows (managed widgets destroyed on remove)
  auto children = list_box_.get_children();
  for (auto* child : children) {
    list_box_.remove(*child);
  }

  auto apps = enumerate_graphical_apps(getpid());

  long apps_rss = 0;
  for (const auto& a : apps) apps_rss += a.rss_kb;

  // System remainder: total used RAM minus RSS attributed to listed GUI apps.
  long system_kb = info_.used_memory_kb - apps_rss;
  if (system_kb < 0) system_kb = 0;

  // GUI apps first …
  for (const auto& a : apps) {
    auto* row = Gtk::manage(new AppRow(a));
    row->set_force_close_handler(
        [this](const AppEntry& e) { on_force_close(e); });
    auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::ORIENTATION_HORIZONTAL);
    list_box_.pack_start(*row, Gtk::PACK_SHRINK);
    list_box_.pack_start(*sep, Gtk::PACK_SHRINK);
  }

  // … then LCOS System always at the bottom (protected / non-closable).
  AppEntry system_entry;
  system_entry.name = "LCOS System";
  system_entry.pid = 0;
  system_entry.rss_kb = system_kb;
  system_entry.icon = load_lcos_system_icon();
  system_entry.protected_app = true;
  system_entry.protect_reason = "system";

  auto* sys_row = Gtk::manage(new AppRow(system_entry));
  // No force-close handler — protected / non-closable.
  list_box_.pack_start(*sys_row, Gtk::PACK_SHRINK);

  list_box_.show_all();
}

void MainWindow::on_force_close(const AppEntry& entry) {
  if (entry.protected_app || entry.pid <= 1 || entry.pid == getpid()) return;

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

  if (dlg.run() == Gtk::RESPONSE_ACCEPT) {
    if (kill(entry.pid, SIGKILL) != 0) {
      Gtk::MessageDialog err(*this, "Could not force-close process.", false,
                             Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
      err.set_secondary_text(std::strerror(errno));
      err.run();
    }
    // Brief delay then refresh
    Glib::signal_timeout().connect_seconds(
        [this]() {
          refresh_app_list();
          return false;
        },
        1);
  }
}

bool MainWindow::on_refresh_tick() {
  refresh_app_list();
  return true;
}

}  // namespace lundukeabout
