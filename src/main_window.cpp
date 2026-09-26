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

const char* kPlatinumCss = R"CSS(
window.lunduke-about {
  background-color: #c0c0c0;
  color: #000000;
}
window.lunduke-about * {
  font-family: "Charcoal", "Geneva", "Helvetica", "DejaVu Sans", Sans;
}
.platinum-panel {
  background-color: #c0c0c0;
}
.platinum-info {
  background-color: #c0c0c0;
  color: #000000;
  font-size: 12px;
}
.platinum-info label {
  color: #000000;
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
.platinum-header {
  background-color: #c0c0c0;
}
scrollbar.platinum-scroll {
  background-color: #c0c0c0;
}
scrollbar.platinum-scroll slider {
  background-color: #5a7ec8;
  border-radius: 0;
  min-width: 14px;
  border: 1px solid #2a4a8a;
}
scrollbar.platinum-scroll button {
  background-color: #a8a8a8;
  border-radius: 0;
}
)CSS";

}  // namespace

MainWindow::MainWindow() {
  set_title("About This Computer");
  set_default_size(520, 480);
  set_border_width(0);
  set_resizable(true);
  get_style_context()->add_class("lunduke-about");

  apply_platinum_css();

  info_ = gather_system_info();

  add(root_);
  root_.get_style_context()->add_class("platinum-panel");
  root_.set_margin_top(10);
  root_.set_margin_bottom(10);
  root_.set_margin_start(12);
  root_.set_margin_end(12);
  root_.set_spacing(8);

  // 1. Logo centered
  load_logo();
  logo_.set_halign(Gtk::ALIGN_CENTER);
  logo_.set_margin_top(4);
  logo_.set_margin_bottom(2);
  root_.pack_start(logo_, Gtk::PACK_SHRINK);

  // 2. Marquee
  load_supporters();
  marquee_.set_margin_top(2);
  marquee_.set_margin_bottom(2);
  root_.pack_start(marquee_, Gtk::PACK_SHRINK);

  // 3. System info
  auto* info_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 2);
  info_box->get_style_context()->add_class("platinum-info");
  info_box->set_margin_top(4);
  info_box->set_margin_bottom(4);
  info_box->set_halign(Gtk::ALIGN_START);

  os_label_.set_text("LCOS version:  " + info_.os_pretty);
  os_label_.set_halign(Gtk::ALIGN_START);
  mem_label_.set_text("Built-in Memory:  " + info_.total_memory);
  mem_label_.set_halign(Gtk::ALIGN_START);
  cpu_label_.set_text("System CPU:  " + info_.cpu_model);
  cpu_label_.set_halign(Gtk::ALIGN_START);
  cpu_label_.set_ellipsize(Pango::ELLIPSIZE_END);
  cpu_label_.set_max_width_chars(56);
  gpu_label_.set_text("GPU:  " + info_.gpu);
  gpu_label_.set_halign(Gtk::ALIGN_START);
  gpu_label_.set_ellipsize(Pango::ELLIPSIZE_END);
  gpu_label_.set_max_width_chars(56);

  info_box->pack_start(os_label_, Gtk::PACK_SHRINK);
  info_box->pack_start(mem_label_, Gtk::PACK_SHRINK);
  info_box->pack_start(cpu_label_, Gtk::PACK_SHRINK);
  info_box->pack_start(gpu_label_, Gtk::PACK_SHRINK);
  root_.pack_start(*info_box, Gtk::PACK_SHRINK);

  // 4. Scrollable app list in beveled frame
  auto* frame = Gtk::make_managed<Gtk::Frame>();
  frame->set_shadow_type(Gtk::SHADOW_IN);
  frame->get_style_context()->add_class("platinum-list-frame");
  frame->set_hexpand(true);
  frame->set_vexpand(true);

  list_scroll_.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
  list_scroll_.get_style_context()->add_class("platinum-scroll");
  list_scroll_.set_min_content_height(200);

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
    css_->load_from_data(kPlatinumCss);
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
  // Prefer tree/shipped black logo; fallback to system pixmap only as last resort
  // and only if it is the black variant (we intentionally skip old color bitmap).
  const char* paths[] = {
      "pixmaps/lcos-logo-black-128.png",
      "pixmaps/lcos-logo-black-256.png",
      "pixmaps/lcos-logo-black-preview.png",
      "pixmaps/lcos-logo-black.svg",
  };

  Glib::RefPtr<Gdk::Pixbuf> pb;
  for (const char* rel : paths) {
    std::string path = find_data_file(rel);
    try {
      // GdkPixbuf loads PNG reliably; SVG needs librsvg loader which may be absent.
      if (std::string(rel).size() >= 4 &&
          std::string(rel).substr(std::string(rel).size() - 4) == ".svg") {
        continue;  // skip SVG without librsvg; we ship baked PNGs
      }
      pb = Gdk::Pixbuf::create_from_file(path, 96, 96, true);
      if (pb) break;
    } catch (...) {
      // try next
    }
  }

  if (!pb) {
    // Last-ditch: try absolute known branding preview
    try {
      pb = Gdk::Pixbuf::create_from_file(
          "/workspace/lcos-branding/lcos-logo-black-preview.png", 96, 96, true);
    } catch (...) {
    }
  }

  if (pb) {
    logo_.set(pb);
  } else {
    logo_.set_from_icon_name("computer", Gtk::ICON_SIZE_DIALOG);
  }
}

void MainWindow::load_supporters() {
  std::string path = find_data_file("supporters.txt");
  std::ifstream in(path);
  std::string names;
  if (in) {
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      if (!names.empty()) names += ", ";
      // Allow either comma-separated one-liners or one-per-line
      if (line.find(',') != std::string::npos) {
        names += line;
      } else {
        names += line;
      }
    }
  }
  if (names.empty()) {
    names = "Alice, Bob, Carol";  // obvious placeholders
  }
  marquee_.set_text("Supporters of LCOS:  " + names + "    ");
}

void MainWindow::refresh_app_list() {
  // Clear existing rows (managed widgets destroyed on remove)
  auto children = list_box_.get_children();
  for (auto* child : children) {
    list_box_.remove(*child);
  }

  auto apps = enumerate_graphical_apps(getpid());
  long max_rss = 1;
  for (const auto& a : apps) max_rss = std::max(max_rss, a.rss_kb);

  if (apps.empty()) {
    auto* empty = Gtk::make_managed<Gtk::Label>("No graphical applications found.");
    empty->set_margin_top(12);
    empty->set_margin_bottom(12);
    list_box_.pack_start(*empty, Gtk::PACK_SHRINK);
  } else {
    for (const auto& a : apps) {
      auto* row = Gtk::manage(new AppRow(a, max_rss, info_.total_memory_kb));
      row->set_force_close_handler(
          [this](const AppEntry& e) { on_force_close(e); });
      // Alternating subtle separator
      auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::ORIENTATION_HORIZONTAL);
      list_box_.pack_start(*row, Gtk::PACK_SHRINK);
      list_box_.pack_start(*sep, Gtk::PACK_SHRINK);
    }
  }
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
