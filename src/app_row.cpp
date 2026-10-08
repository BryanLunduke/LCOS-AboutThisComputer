// SPDX-License-Identifier: GPL-3.0-or-later
#include "app_row.hpp"
#include "about_logic.hpp"
#include "system_info.hpp"

#include <glib.h>

#include <cstring>
#include <unistd.h>

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

  name_.set_halign(Gtk::ALIGN_START);
  name_.set_xalign(0.0f);
  name_.set_ellipsize(Pango::ELLIPSIZE_END);
  name_.set_hexpand(true);
  box_.pack_start(name_, Gtk::PACK_EXPAND_WIDGET);

  // Shared-name token. It does not ellipsize: the name column gives up
  // width first, so two long rows stay visually distinct.
  detail_.set_halign(Gtk::ALIGN_END);
  detail_.set_xalign(1.0f);
  detail_.set_ellipsize(Pango::ELLIPSIZE_NONE);
  detail_.set_hexpand(false);
  detail_.set_no_show_all(true);
  box_.pack_start(detail_, Gtk::PACK_SHRINK);

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

  apply_entry_text();
  show_all();
}

AppRow::~AppRow() {
  menu_posted_ = false;
  if (menu_attached_ && menu_.get_attach_widget()) menu_.detach();
  menu_attached_ = false;
}

void AppRow::update_entry(const AppEntry& entry) {
  const bool mem_changed =
      entry_.rss_kb != entry.rss_kb || entry_.rss_known != entry.rss_known;
  entry_ = entry;
  apply_entry_text();
  if (mem_changed) mem_label_.set_text(memory_caption(entry_));
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
  if (entry_.protected_app) return false;
  if (entry_.pid <= 1) return false;
  if (entry_.pid == ::getpid()) return false;
  return true;
}

bool AppRow::keyboard_targets_this_row() const {
  // The window's focus widget. has_focus() also demands a focused toplevel,
  // which a display without a window manager does not provide.
  return is_focus();
}

std::string AppRow::blocked_reason() const {
  if (!entry_.protect_reason.empty()) return entry_.protect_reason;
  if (entry_.pid == ::getpid()) return "This application";
  if (entry_.pid <= 1) return "No process ID";
  return "Protected";
}

void AppRow::apply_entry_text() {
  name_.set_text(utf8_text(painted_row_name(entry_.name, entry_.distinguish)));
  if (entry_.distinguish.empty()) {
    detail_.set_text("");
    detail_.hide();
  } else {
    detail_.set_text(utf8_text(entry_.distinguish));
    detail_.show();
  }
  const bool closable = can_force_close();
  const std::string title = entry_.tooltip.empty() ? entry_.name : entry_.tooltip;
  const std::string tip =
      row_tooltip_text(title, closable, closable ? std::string() : blocked_reason(),
                       closable ? entry_.distinguish : std::string());
  const Glib::ustring shown = utf8_text(tip);
  name_.set_tooltip_text(shown);
  detail_.set_tooltip_text(shown);
  set_tooltip_text(shown);
  name_.set_sensitive(!entry_.protected_app);
  detail_.set_sensitive(!entry_.protected_app);
  rebuild_menu_item();
}

void AppRow::set_force_close_handler(ForceCloseHandler handler) {
  handler_ = std::move(handler);
}

void AppRow::rebuild_menu_item() {
  const bool closable = can_force_close();
  item_.set_label(force_close_menu_label(entry_.name, closable,
                                         closable ? std::string() : blocked_reason()));
  item_.set_sensitive(closable);
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

bool AppRow::on_focus(Gtk::DirectionType /*direction*/) {
  // GtkContainer::focus uses has_focus(), which is false whenever the
  // toplevel is not the active window. Tab then grabs this row again and
  // never reaches the next one. is_focus() is the focus widget inside
  // this window, which is enough to move on.
  if (!get_can_focus()) return false;
  if (!is_focus()) {
    grab_focus();
    return true;
  }
  return false;
}

bool AppRow::on_button_press_event(GdkEventButton* event) {
  // Left click and right click both make this the current row before any
  // later Menu or Shift+F10. The key handlers act on the focused row only.
  if (event && event->type == GDK_BUTTON_PRESS &&
      (event->button == 1 || event->button == 3)) {
    grab_focus();
  }
  if (event && event->type == GDK_BUTTON_PRESS && event->button == 3) {
    popup_force_close_menu(reinterpret_cast<const GdkEvent*>(event));
    return true;
  }
  return Gtk::EventBox::on_button_press_event(event);
}

bool AppRow::on_key_press_event(GdkEventKey* event) {
  if (event && is_force_close_popup_key(event->keyval, event->state)) {
    // Swallow the key even when this row is not current, so a binding
    // cannot open Force Close for a row the user is not on.
    if (!keyboard_targets_this_row()) return true;
    popup_force_close_menu(reinterpret_cast<const GdkEvent*>(event));
    return true;
  }
  return Gtk::EventBox::on_key_press_event(event);
}

bool AppRow::on_popup_menu() {
  if (!keyboard_targets_this_row()) return false;
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
