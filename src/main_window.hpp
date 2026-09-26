// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "system_info.hpp"
#include "memory_bar.hpp"
#include "window_enum.hpp"

#include <gtkmm.h>
#include <vector>

namespace lundukeabout {

class MainWindow : public Gtk::ApplicationWindow {
public:
  MainWindow();

private:
  void apply_platinum_css();
  void load_logo();
  void load_supporters();
  void refresh_app_list();
  void update_ram_bar();
  Glib::RefPtr<Gdk::Pixbuf> load_lcos_system_icon() const;
  void on_force_close(const AppEntry& entry);
  bool on_refresh_tick();
  std::string find_data_file(const std::string& relative) const;

  SystemInfo info_;
  Gtk::Box root_{Gtk::ORIENTATION_VERTICAL, 0};
  Gtk::Image logo_;
  // Top-right Supporters block (static for now). Keep as a dedicated box so a
  // future upward movie-credits scroll can replace the labels with a viewport.
  Gtk::Box supporters_box_{Gtk::ORIENTATION_VERTICAL, 0};
  Gtk::Label supporters_title_;
  Gtk::Label supporters_blank_;
  Gtk::Label supporters_names_;
  Gtk::Label os_label_;
  Gtk::Label mem_label_;
  Gtk::Label cpu_label_;
  Gtk::Label gpu_label_;
  MemoryBar ram_bar_;
  Gtk::ScrolledWindow list_scroll_;
  Gtk::Box list_box_{Gtk::ORIENTATION_VERTICAL, 0};
  Glib::RefPtr<Gtk::CssProvider> css_;
  sigc::connection refresh_conn_;
};

}  // namespace lundukeabout
