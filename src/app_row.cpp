// SPDX-License-Identifier: GPL-3.0-or-later
#include "app_row.hpp"
#include "system_info.hpp"

#include <algorithm>
#include <iostream>

namespace lundukeabout {

AppRow::AppRow(const AppEntry& entry, long max_rss_kb, long /*total_ram_kb*/)
    : entry_(entry) {
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
  name_.set_ellipsize(Pango::ELLIPSIZE_END);
  name_.set_max_width_chars(28);
  name_.set_size_request(160, -1);
  if (entry.protected_app) {
    name_.set_sensitive(false);
  }
  box_.pack_start(name_, Gtk::PACK_SHRINK);

  // Bar length ∝ RSS vs largest in list (Mac partition width analogue).
  // Fill uses RSS vs a capped "allocated" (VmSize is usually huge on Linux
  // because of shared mappings, so we clamp allocated to <= 3x RSS).
  double rel = 0.25;
  if (max_rss_kb > 0)
    rel = std::max(0.12, static_cast<double>(entry.rss_kb) / static_cast<double>(max_rss_kb));
  long alloc_kb = entry.vsize_kb;
  if (alloc_kb < entry.rss_kb) alloc_kb = entry.rss_kb;
  if (entry.rss_kb > 0 && alloc_kb > entry.rss_kb * 3)
    alloc_kb = entry.rss_kb * 3;
  // Prefer a visible used portion: treat allocated as at least 1.15x RSS
  if (entry.rss_kb > 0 && alloc_kb < static_cast<long>(entry.rss_kb * 1.15))
    alloc_kb = static_cast<long>(entry.rss_kb * 1.15);
  double used_frac = 1.0;
  if (alloc_kb > 0)
    used_frac = std::clamp(static_cast<double>(entry.rss_kb) /
                               static_cast<double>(alloc_kb),
                           0.15, 1.0);
  bar_.set_relative_width(rel);
  bar_.set_usage(used_frac);
  bar_.set_size_request(160, 14);
  box_.pack_start(bar_, Gtk::PACK_EXPAND_WIDGET);

  mem_label_.set_text(format_memory_mb(entry.rss_kb));
  mem_label_.set_halign(Gtk::ALIGN_END);
  mem_label_.set_width_chars(8);
  box_.pack_start(mem_label_, Gtk::PACK_SHRINK);

  add_events(Gdk::BUTTON_PRESS_MASK);
  show_all();
}

void AppRow::set_force_close_handler(ForceCloseHandler handler) {
  handler_ = std::move(handler);
}

bool AppRow::on_button_press_event(GdkEventButton* event) {
  if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
    auto menu = Gtk::make_managed<Gtk::Menu>();
    std::string label = "Force Close " + entry_.name;
    auto item = Gtk::make_managed<Gtk::MenuItem>(label);
    if (entry_.protected_app) {
      item->set_sensitive(false);
      item->set_label(label + " (" + entry_.protect_reason + ")");
    } else {
      item->signal_activate().connect(sigc::mem_fun(*this, &AppRow::on_force_close));
    }
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
