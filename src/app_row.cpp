// SPDX-License-Identifier: GPL-3.0-or-later
#include "app_row.hpp"
#include "about_logic.hpp"
#include "system_info.hpp"

#include <glib.h>

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

std::string AppRow::memory_caption(const AppEntry& entry) {
  if (!entry.rss_known) return std::string(u8"\u2014 RAM Used");
  return format_memory_human(entry.rss_kb) + " RAM Used";
}

Glib::ustring AppRow::utf8_text(const std::string& text) {
  if (text.empty()) return {};
  if (g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) return text;
  gchar* fixed = g_utf8_make_valid(text.data(), static_cast<gssize>(text.size()));
  Glib::ustring out(fixed ? fixed : "");
  g_free(fixed);
  return out;
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

  name_.set_text(utf8_text(entry.name));
  name_.set_halign(Gtk::ALIGN_START);
  name_.set_xalign(0.0f);
  name_.set_ellipsize(Pango::ELLIPSIZE_END);
  name_.set_hexpand(true);
  const std::string tip = row_tooltip_text(entry.tooltip.empty() ? entry.name : entry.tooltip);
  name_.set_tooltip_text(utf8_text(tip));
  set_tooltip_text(utf8_text(tip));
  if (entry.protected_app) {
    name_.set_sensitive(false);
  }
  box_.pack_start(name_, Gtk::PACK_EXPAND_WIDGET);

  mem_label_.set_text(memory_caption(entry));
  mem_label_.set_halign(Gtk::ALIGN_END);
  mem_label_.set_xalign(1.0f);
  box_.pack_start(mem_label_, Gtk::PACK_SHRINK);

  add_events(Gdk::BUTTON_PRESS_MASK | Gdk::KEY_PRESS_MASK);
  set_can_focus(true);
  get_style_context()->add_class("app-row");

  menu_.attach_to_widget(*this);
  menu_attached_ = true;
  item_.signal_activate().connect(sigc::mem_fun(*this, &AppRow::on_force_close));
  signal_popup_menu().connect(sigc::mem_fun(*this, &AppRow::on_popup_menu));
  menu_.signal_deactivate().connect([this]() { menu_posted_ = false; });
  menu_.signal_selection_done().connect([this]() { menu_posted_ = false; });
  menu_.append(item_);

  if (entry.icon) shown_icon_ = entry.icon;

  show_all();
}

AppRow::~AppRow() {
  menu_posted_ = false;
  if (menu_attached_ && menu_.get_attach_widget()) menu_.detach();
  menu_attached_ = false;
}

void AppRow::update_entry(const AppEntry& entry) {
  const bool name_changed = entry_.name != entry.name;
  const bool mem_changed =
      entry_.rss_kb != entry.rss_kb || entry_.rss_known != entry.rss_known;
  const bool prot_changed = entry_.protected_app != entry.protected_app;
  const bool tip_changed = entry_.tooltip != entry.tooltip || name_changed;
  entry_ = entry;
  if (name_changed) name_.set_text(utf8_text(entry_.name));
  if (tip_changed) {
    const std::string tip = row_tooltip_text(entry_.tooltip.empty() ? entry_.name : entry_.tooltip);
    name_.set_tooltip_text(utf8_text(tip));
    set_tooltip_text(utf8_text(tip));
  }
  if (mem_changed) mem_label_.set_text(memory_caption(entry_));
  if (prot_changed) name_.set_sensitive(!entry_.protected_app);
  if (entry_.icon) {
    if (!same_pixbuf(shown_icon_, entry_.icon)) {
      icon_.set(entry_.icon);
      shown_icon_ = entry_.icon;
    }
  } else if (shown_icon_) {
    icon_.set_from_icon_name("application-x-executable", Gtk::ICON_SIZE_DND);
    icon_.set_pixel_size(32);
    shown_icon_.reset();
  }
}

bool AppRow::can_force_close() const {
  return !entry_.protected_app && entry_.pid > 1;
}

void AppRow::set_force_close_handler(ForceCloseHandler handler) {
  handler_ = std::move(handler);
}

void AppRow::rebuild_menu_item() {
  item_.set_label(force_close_menu_label(entry_.name, can_force_close(), entry_.protect_reason));
  item_.set_sensitive(can_force_close());
}

void AppRow::popup_force_close_menu(const GdkEvent* event) {
  if (!menu_attached_) {
    menu_.attach_to_widget(*this);
    menu_attached_ = true;
  }
  rebuild_menu_item();
  menu_.show_all();
  menu_posted_ = true;
  if (event && event->type == GDK_BUTTON_PRESS) {
    menu_.popup_at_pointer(event);
  } else {
    menu_.popup_at_widget(this, Gdk::GRAVITY_SOUTH_WEST, Gdk::GRAVITY_NORTH_WEST, event);
  }
}

bool AppRow::on_button_press_event(GdkEventButton* event) {
  if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
    grab_focus();
    popup_force_close_menu(reinterpret_cast<const GdkEvent*>(event));
    return true;
  }
  return Gtk::EventBox::on_button_press_event(event);
}

bool AppRow::on_key_press_event(GdkEventKey* event) {
  if (event && is_force_close_popup_key(event->keyval, event->state)) {
    popup_force_close_menu(reinterpret_cast<const GdkEvent*>(event));
    return true;
  }
  return Gtk::EventBox::on_key_press_event(event);
}

bool AppRow::on_popup_menu() {
  popup_force_close_menu(nullptr);
  return true;
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
