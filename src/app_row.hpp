// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "window_enum.hpp"
#include "memory_bar.hpp"

#include <gtkmm.h>
#include <functional>

namespace lundukeabout {

class AppRow : public Gtk::EventBox {
public:
  AppRow(const AppEntry& entry, long max_rss_kb, long total_ram_kb);

  pid_t pid() const { return entry_.pid; }
  const AppEntry& entry() const { return entry_; }

  using ForceCloseHandler = std::function<void(const AppEntry&)>;
  void set_force_close_handler(ForceCloseHandler handler);

protected:
  bool on_button_press_event(GdkEventButton* event) override;

private:
  void on_force_close();
  AppEntry entry_;
  ForceCloseHandler handler_;
  Gtk::Box box_{Gtk::ORIENTATION_HORIZONTAL, 8};
  Gtk::Image icon_;
  Gtk::Label name_;
  MemoryBar bar_;
  Gtk::Label mem_label_;
  Glib::RefPtr<Gtk::CssProvider> row_css_;
};

}  // namespace lundukeabout
