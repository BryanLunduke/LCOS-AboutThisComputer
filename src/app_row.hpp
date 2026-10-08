// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "window_enum.hpp"

#include <gtkmm.h>
#include <functional>

namespace lundukeabout {

class AppRow : public Gtk::EventBox {
public:
  AppRow(const AppEntry& entry);
  ~AppRow() override;

  pid_t pid() const { return entry_.pid; }
  const AppEntry& entry() const { return entry_; }
  bool menu_posted() const { return menu_posted_; }
  void update_entry(const AppEntry& entry);

  using ForceCloseHandler = std::function<void(const AppEntry&)>;
  void set_force_close_handler(ForceCloseHandler handler);

protected:
  bool on_button_press_event(GdkEventButton* event) override;
  bool on_key_press_event(GdkEventKey* event) override;
  bool on_focus(Gtk::DirectionType direction) override;

private:
  void on_force_close();
  bool on_popup_menu();
  bool can_force_close() const;
  bool keyboard_targets_this_row() const;
  std::string blocked_reason() const;
  void apply_entry_text();
  void rebuild_menu_item();
  void popup_force_close_menu(const GdkEvent* event);
  static bool same_pixbuf(const Glib::RefPtr<Gdk::Pixbuf>& a,
                          const Glib::RefPtr<Gdk::Pixbuf>& b);
  static std::string memory_caption(const AppEntry& entry);
  static Glib::ustring utf8_text(const std::string& text);

  AppEntry entry_;
  ForceCloseHandler handler_;
  Glib::RefPtr<Gdk::Pixbuf> shown_icon_;
  Gtk::Box box_{Gtk::ORIENTATION_HORIZONTAL, 8};
  Gtk::Image icon_;
  Gtk::Label name_;
  Gtk::Label detail_;
  Gtk::Label mem_label_;
  Gtk::Menu menu_;
  Gtk::MenuItem item_;
  bool menu_attached_ = false;
  bool menu_posted_ = false;
};

}  // namespace lundukeabout
