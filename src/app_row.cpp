// SPDX-License-Identifier: GPL-3.0-or-later
#include "app_row.hpp"
#include "system_info.hpp"

#include <cstring>

namespace lundukeabout {

bool AppRow::same_pixbuf(const Glib::RefPtr<Gdk::Pixbuf>& a,
                         const Glib::RefPtr<Gdk::Pixbuf>& b) {
  if (a == b) return true;
  if (!a || !b) return false;
  if (a->get_width() != b->get_width() || a->get_height() != b->get_height() ||
      a->get_rowstride() != b->get_rowstride() ||
      a->get_n_channels() != b->get_n_channels() ||
      a->get_bits_per_sample() != b->get_bits_per_sample()) {
    return false;
  }
  const int nbytes = a->get_rowstride() * a->get_height();
  if (nbytes <= 0) return true;
  return std::memcmp(a->get_pixels(), b->get_pixels(),
                     static_cast<size_t>(nbytes)) == 0;
}

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
  if (entry.icon) shown_icon_ = entry.icon;

  show_all();
}

void AppRow::update_entry(const AppEntry& entry) {
  const bool name_changed = entry_.name != entry.name;
  const bool mem_changed = entry_.rss_kb != entry.rss_kb;
  const bool prot_changed = entry_.protected_app != entry.protected_app;
  entry_ = entry;
  if (name_changed) name_.set_text(entry_.name);
  if (mem_changed) {
    mem_label_.set_text(format_memory_mb(entry_.rss_kb) + " RAM Used");
  }
  if (prot_changed) name_.set_sensitive(!entry_.protected_app);
  if (entry_.icon && !same_pixbuf(shown_icon_, entry_.icon)) {
    icon_.set(entry_.icon);
    shown_icon_ = entry_.icon;
  }
}

bool AppRow::can_force_close() const {
  return !entry_.protected_app && entry_.pid > 1;
}

void AppRow::set_force_close_handler(ForceCloseHandler handler) {
  handler_ = std::move(handler);
}

bool AppRow::on_button_press_event(GdkEventButton* event) {
  if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
    // Protected rows (LCOS System, panel, window manager, …) and windows
    // with no real PID have no Force Close action.
    if (!can_force_close()) return true;
    auto menu = Gtk::make_managed<Gtk::Menu>();
    auto item = Gtk::make_managed<Gtk::MenuItem>("Force Close " + entry_.name);
    item->signal_activate().connect(sigc::mem_fun(*this, &AppRow::on_force_close));
    menu->append(*item);
    menu->show_all();
    menu->popup_at_pointer(reinterpret_cast<GdkEvent*>(event));
    return true;
  }
  return Gtk::EventBox::on_button_press_event(event);
}

void AppRow::on_force_close() {
  if (!handler_ || !can_force_close()) return;
  // Copy before the handler runs a dialog. Refresh must not free this row
  // out from under the reference the handler would otherwise keep.
  const AppEntry snapshot = entry_;
  ForceCloseHandler handler = handler_;
  handler(snapshot);
}

}  // namespace lundukeabout
