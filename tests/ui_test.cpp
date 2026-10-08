// SPDX-License-Identifier: GPL-3.0-or-later
// Headless-display checks for the window: ellipsized OS line, Force Close
// keyboard popup, a scrollbar that is not an overlay, and LCOS System
// outside the scroller at the default size.
#include "app_row.hpp"
#include "main_window.hpp"

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <gtkmm/main.h>

#include <cstring>
#include <functional>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, int line) {
  if (cond) return;
  std::cerr << "FAIL " << line << ": " << expr << "\n";
  ++g_failures;
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void drain() {
  while (gtk_events_pending()) gtk_main_iteration();
}

void walk(Gtk::Widget& widget, const std::function<void(Gtk::Widget&)>& fn) {
  fn(widget);
  if (auto* container = dynamic_cast<Gtk::Container*>(&widget)) {
    for (Gtk::Widget* child : container->get_children()) {
      if (child) walk(*child, fn);
    }
  }
}

bool inside_scrolled(Gtk::Widget& widget) {
  for (Gtk::Widget* parent = widget.get_parent(); parent; parent = parent->get_parent()) {
    if (dynamic_cast<Gtk::ScrolledWindow*>(parent)) return true;
  }
  return false;
}

void test_info_label() {
  const std::string long_name =
      "OS Version:  Lunduke Computer Operating System 0.9 "
      "(Devuan GNU/Linux preview with an extremely long pretty name for layout)";
  Gtk::Window window;
  Gtk::Box box(Gtk::ORIENTATION_HORIZONTAL, 0);
  Gtk::Label ellipsized;
  Gtk::Label plain;
  lundukeabout::set_info_label(ellipsized, long_name);
  plain.set_text(long_name);
  box.pack_start(ellipsized, Gtk::PACK_SHRINK);
  box.pack_start(plain, Gtk::PACK_SHRINK);
  window.add(box);
  window.show_all();
  drain();
  CHECK(ellipsized.get_ellipsize() == Pango::ELLIPSIZE_END);
  CHECK(ellipsized.get_max_width_chars() == 36);
  CHECK(ellipsized.get_tooltip_text() == long_name);
  int min_w = 0;
  int nat_w = 0;
  int plain_min = 0;
  int plain_nat = 0;
  ellipsized.get_preferred_width(min_w, nat_w);
  plain.get_preferred_width(plain_min, plain_nat);
  CHECK(min_w > 0);
  CHECK(min_w < plain_min);
  CHECK(nat_w < plain_nat);
  window.hide();
  drain();
}

void test_menu_key() {
  lundukeabout::AppEntry entry;
  entry.name = "My_App";
  entry.tooltip = "Doc";
  entry.pid = 42;
  entry.comm = "testwin";
  entry.identity_ok = true;
  entry.rss_known = true;
  entry.rss_kb = 200;
  lundukeabout::AppEntry other_entry = entry;
  other_entry.name = "Keeper";
  other_entry.pid = 43;
  Gtk::Window window;
  Gtk::Box box(Gtk::ORIENTATION_VERTICAL, 0);
  auto* row = Gtk::manage(new lundukeabout::AppRow(entry));
  auto* other = Gtk::manage(new lundukeabout::AppRow(other_entry));
  box.pack_start(*row, Gtk::PACK_SHRINK);
  box.pack_start(*other, Gtk::PACK_SHRINK);
  window.add(box);
  window.show_all();
  drain();
  const std::string tip = row->get_tooltip_text();
  CHECK(tip.find("Right-click or press the Menu key to Force Close") != std::string::npos);
  CHECK(tip.find("Doc") != std::string::npos);
  CHECK(row->get_can_focus());
  row->grab_focus();
  drain();
  CHECK(row->is_focus());
  if (auto gdkwin = window.get_window()) gdkwin->focus(GDK_CURRENT_TIME);
  drain();
  CHECK(window.has_toplevel_focus());
  CHECK(row->has_focus());
  g_signal_emit_by_name(window.gobj(), "move-focus", GTK_DIR_TAB_FORWARD);
  drain();
  CHECK(other->is_focus());
  row->grab_focus();
  drain();

  GdkEventKey key;
  std::memset(&key, 0, sizeof(key));
  key.type = GDK_KEY_PRESS;
  key.window = row->get_window() ? row->get_window()->gobj() : nullptr;
  key.keyval = GDK_KEY_Menu;
  key.state = 0;
  key.send_event = 1;
  gtk_widget_event(GTK_WIDGET(row->gobj()), reinterpret_cast<GdkEvent*>(&key));
  drain();
  CHECK(row->menu_posted());

  window.hide();
  drain();
}

void test_window_layout() {
  lundukeabout::MainWindow window;
  drain();

  int min_w = 0;
  int min_h = 0;
  window.get_size_request(min_w, min_h);
  CHECK(min_w == 520);
  CHECK(min_h == 360);
  int pref_min_w = 0;
  int pref_nat_w = 0;
  int pref_min_h = 0;
  int pref_nat_h = 0;
  window.get_preferred_width(pref_min_w, pref_nat_w);
  window.get_preferred_height(pref_min_h, pref_nat_h);
  CHECK(pref_min_w <= 520);
  CHECK(pref_min_h <= 360);
  CHECK(pref_nat_w >= pref_min_w);
  CHECK(pref_nat_h >= pref_min_h);
  CHECK(window.get_allocated_width() <= 560);

  bool saw_scroll = false;
  bool overlay = true;
  walk(window, [&](Gtk::Widget& widget) {
    if (auto* scroll = dynamic_cast<Gtk::ScrolledWindow*>(&widget)) {
      saw_scroll = true;
      overlay = scroll->get_overlay_scrolling();
    }
  });
  CHECK(saw_scroll);
  CHECK(overlay == false);

  bool saw_os = false;
  walk(window, [&](Gtk::Widget& widget) {
    auto* label = dynamic_cast<Gtk::Label*>(&widget);
    if (!label) return;
    const std::string text = label->get_text();
    if (text.rfind("OS Version:", 0) != 0) return;
    saw_os = true;
    CHECK(label->get_ellipsize() == Pango::ELLIPSIZE_END);
    CHECK(label->get_max_width_chars() == 36);
    CHECK(label->get_tooltip_text() == text);
  });
  CHECK(saw_os);

  Gtk::Label* system = nullptr;
  for (int i = 0; i < 50 && !(system && system->get_allocated_height() > 1); ++i) {
    system = nullptr;
    walk(window, [&](Gtk::Widget& widget) {
      auto* label = dynamic_cast<Gtk::Label*>(&widget);
      if (!label) return;
      if (label->get_text() == "LCOS System") system = label;
    });
    if (system && system->get_allocated_height() > 1) break;
    g_usleep(50000);
    drain();
  }
  CHECK(system != nullptr);
  if (system) {
    CHECK(inside_scrolled(*system) == false);
    CHECK(system->get_mapped());
    int window_h = window.get_allocated_height();
    int x = 0;
    int y = 0;
    const bool translated = system->translate_coordinates(window, 0, 0, x, y);
    CHECK(translated);
    const int bottom = y + system->get_allocated_height();
    CHECK(system->get_allocated_height() > 1);
    CHECK(y >= 0);
    CHECK(bottom <= window_h);
  }

  window.hide();
  drain();
}

}  // namespace

int main(int argc, char** argv) {
  if (!gtk_init_check(&argc, &argv)) {
    std::cerr << "Needs X11\n";
    return 1;
  }
  Gtk::Main::init_gtkmm_internals();
  test_info_label();
  test_menu_key();
  test_window_layout();
  if (g_failures != 0) {
    std::cerr << g_failures << " failure(s)\n";
    return 1;
  }
  std::cout << "ok\n";
  return 0;
}
