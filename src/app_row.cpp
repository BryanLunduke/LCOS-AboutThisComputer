// SPDX-License-Identifier: GPL-3.0-or-later
#include "app_row.hpp"
#include "system_info.hpp"

namespace lundukeabout {

AppRow::AppRow(const AppEntry& entry) : entry_(entry) {
  set_visible_window(false);
  add(box_);
  box_.set_margin_start(6);
  box_.set_margin_end(6);
  box_.set_margin_top(3);
  box_.set_margin_bottom(3);
  box_.set_valign(Gtk::ALIGN_CENTER);

  if (entry.icon) {
    icon_.set(entry.icon);
  } else {
    icon_.set_from_icon_name("application-x-executable", Gtk::ICON_SIZE_DND);
    icon_.set_pixel_size(32);
  }
  icon_.set_valign(Gtk::ALIGN_CENTER);
  box_.pack_start(icon_, Gtk::PACK_SHRINK);

  name_.set_text(entry.name);
  name_.set_halign(Gtk::ALIGN_START);
  name_.set_xalign(0.0f);
  name_.set_ellipsize(Pango::ELLIPSIZE_END);
  name_.set_hexpand(true);
  if (entry.protected_app) {
    name_.set_sensitive(false);
  }
  box_.pack_start(name_, Gtk::PACK_EXPAND_WIDGET);

  mem_label_.set_text(format_memory_mb(entry.rss_kb) + " RAM Used");
  mem_label_.set_halign(Gtk::ALIGN_END);
  mem_label_.set_xalign(1.0f);
  box_.pack_start(mem_label_, Gtk::PACK_SHRINK);

  add_events(Gdk::BUTTON_PRESS_MASK);
  show_all();
}

void AppRow::set_force_close_handler(ForceCloseHandler handler) {
  handler_ = std::move(handler);
}

bool AppRow::on_button_press_event(GdkEventButton* event) {
  if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
    // Protected rows (e.g. LCOS System): no Force Close action / menu.
    if (entry_.protected_app) {
      return true;
    }
    auto menu = Gtk::make_managed<Gtk::Menu>();
    std::string label = "Force Close " + entry_.name;
    auto item = Gtk::make_managed<Gtk::MenuItem>(label);
    item->signal_activate().connect(sigc::mem_fun(*this, &AppRow::on_force_close));
    menu->append(*item);
    menu->show_all();
    menu->popup_at_pointer(reinterpret_cast<GdkEvent*>(event));
    return true;
  }
  return Gtk::EventBox::on_button_press_event(event);
}

void AppRow::on_force_close() {
  if (handler_) handler_(entry_);
}

}  // namespace lundukeabout
