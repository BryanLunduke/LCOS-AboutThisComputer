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
#include <vector>

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

void walk(Gtk::Widget& widget, const std::function<void(Gtk::Widget&)>& fn);

GdkDevice* seat_device(bool keyboard) {
  GdkDisplay* display = gdk_display_get_default();
  if (!display) return nullptr;
  GdkSeat* seat = gdk_display_get_default_seat(display);
  if (!seat) return nullptr;
  return keyboard ? gdk_seat_get_keyboard(seat) : gdk_seat_get_pointer(seat);
}

void send_button(Gtk::Widget& widget, guint button) {
  auto window = widget.get_window();
  if (!window) return;
  GdkEvent* event = gdk_event_new(GDK_BUTTON_PRESS);
  event->button.window = GDK_WINDOW(g_object_ref(window->gobj()));
  event->button.send_event = TRUE;
  event->button.button = button;
  event->button.time = GDK_CURRENT_TIME;
  event->button.x = 4;
  event->button.y = 4;
  if (GdkDevice* device = seat_device(false)) gdk_event_set_device(event, device);
  gtk_widget_event(widget.gobj(), event);
  gdk_event_free(event);
}

void send_key(Gtk::Widget& widget, guint keyval, guint state) {
  auto window = widget.get_window();
  if (!window) return;
  GdkEvent* event = gdk_event_new(GDK_KEY_PRESS);
  event->key.window = GDK_WINDOW(g_object_ref(window->gobj()));
  event->key.send_event = TRUE;
  event->key.keyval = keyval;
  event->key.state = static_cast<GdkModifierType>(state);
  event->key.time = GDK_CURRENT_TIME;
  if (GdkDevice* device = seat_device(true)) gdk_event_set_device(event, device);
  gtk_widget_event(widget.gobj(), event);
  gdk_event_free(event);
}

std::vector<Gtk::Label*> labels_of(Gtk::Widget& widget) {
  std::vector<Gtk::Label*> out;
  walk(widget, [&](Gtk::Widget& child) {
    if (auto* label = dynamic_cast<Gtk::Label*>(&child)) out.push_back(label);
  });
  return out;
}

Gtk::Label* find_label(Gtk::Widget& widget, const std::string& text) {
  for (Gtk::Label* label : labels_of(widget)) {
    if (label->get_text() == text) return label;
  }
  return nullptr;
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
  // Focus inside the window. A display with no window manager never reports
  // has_toplevel_focus(), and that must not fail the test.
  row->grab_focus();
  drain();
  CHECK(row->is_focus());
  CHECK(row->has_focus() == window.has_toplevel_focus());
  g_signal_emit_by_name(window.gobj(), "move-focus", GTK_DIR_TAB_FORWARD);
  drain();
  CHECK(other->is_focus());
  row->grab_focus();
  drain();
  CHECK(row->is_focus());

  send_key(*row, GDK_KEY_Menu, 0);
  drain();
  CHECK(row->menu_posted());
  CHECK(!other->menu_posted());

  window.hide();
  drain();
}

lundukeabout::AppEntry closable_entry(const std::string& name, pid_t pid) {
  lundukeabout::AppEntry entry;
  entry.name = name;
  entry.tooltip = name;
  entry.pid = pid;
  entry.comm = "testwin";
  entry.identity_ok = true;
  entry.rss_known = true;
  entry.rss_kb = 200;
  return entry;
}

void test_click_and_keys_agree() {
  lundukeabout::AppEntry first = closable_entry("Keeper", 43);
  lundukeabout::AppEntry second = closable_entry("GhostWin", 44);
  lundukeabout::AppEntry blocked = closable_entry("xfce4-panel", 45);
  blocked.protected_app = true;
  blocked.protect_reason = "Desktop panel";
  blocked.comm = "xfce4-panel";

  Gtk::Window window;
  Gtk::Box box(Gtk::ORIENTATION_VERTICAL, 0);
  auto* row_a = Gtk::manage(new lundukeabout::AppRow(first));
  auto* row_b = Gtk::manage(new lundukeabout::AppRow(second));
  auto* row_c = Gtk::manage(new lundukeabout::AppRow(blocked));
  box.pack_start(*row_a, Gtk::PACK_SHRINK);
  box.pack_start(*row_b, Gtk::PACK_SHRINK);
  box.pack_start(*row_c, Gtk::PACK_SHRINK);
  window.add(box);
  window.show_all();
  drain();
  CHECK(row_a->get_window());
  CHECK(row_b->get_window());

  row_a->grab_focus();
  drain();
  CHECK(row_a->is_focus());

  send_button(*row_b, 1);
  drain();
  CHECK(row_b->is_focus());
  CHECK(!row_a->is_focus());
  CHECK(!row_b->menu_posted());

  send_key(*row_a, GDK_KEY_Menu, 0);
  drain();
  CHECK(!row_a->menu_posted());
  CHECK(row_b->is_focus());

  send_key(*row_b, GDK_KEY_Menu, 0);
  drain();
  CHECK(row_b->menu_posted());
  CHECK(!row_a->menu_posted());

  window.hide();
  drain();

  Gtk::Window shift_window;
  Gtk::Box shift_box(Gtk::ORIENTATION_VERTICAL, 0);
  auto* shift_a = Gtk::manage(new lundukeabout::AppRow(first));
  auto* shift_b = Gtk::manage(new lundukeabout::AppRow(second));
  shift_box.pack_start(*shift_a, Gtk::PACK_SHRINK);
  shift_box.pack_start(*shift_b, Gtk::PACK_SHRINK);
  shift_window.add(shift_box);
  shift_window.show_all();
  drain();
  shift_a->grab_focus();
  drain();
  send_button(*shift_b, 1);
  drain();
  CHECK(shift_b->is_focus());
  send_key(*shift_b, GDK_KEY_F10, GDK_SHIFT_MASK);
  drain();
  CHECK(shift_b->menu_posted());
  CHECK(!shift_a->menu_posted());
  send_key(*shift_b, GDK_KEY_F10, 0);
  drain();
  CHECK(!shift_a->menu_posted());
  shift_window.hide();
  drain();

  Gtk::Window click_window;
  Gtk::Box click_box(Gtk::ORIENTATION_VERTICAL, 0);
  auto* click_a = Gtk::manage(new lundukeabout::AppRow(first));
  auto* click_b = Gtk::manage(new lundukeabout::AppRow(second));
  click_box.pack_start(*click_a, Gtk::PACK_SHRINK);
  click_box.pack_start(*click_b, Gtk::PACK_SHRINK);
  click_window.add(click_box);
  click_window.show_all();
  drain();
  click_a->grab_focus();
  drain();
  send_button(*click_b, 3);
  drain();
  CHECK(click_b->is_focus());
  CHECK(click_b->menu_posted());
  CHECK(!click_a->menu_posted());
  click_window.hide();
  drain();

  Gtk::Window blocked_window;
  Gtk::Box blocked_box(Gtk::ORIENTATION_VERTICAL, 0);
  auto* open_row = Gtk::manage(new lundukeabout::AppRow(first));
  auto* shut_row = Gtk::manage(new lundukeabout::AppRow(blocked));
  blocked_box.pack_start(*open_row, Gtk::PACK_SHRINK);
  blocked_box.pack_start(*shut_row, Gtk::PACK_SHRINK);
  blocked_window.add(blocked_box);
  blocked_window.show_all();
  drain();
  open_row->grab_focus();
  drain();
  send_button(*shut_row, 1);
  drain();
  CHECK(shut_row->is_focus());
  send_key(*shut_row, GDK_KEY_Menu, 0);
  drain();
  CHECK(shut_row->menu_posted());
  CHECK(!open_row->menu_posted());
  CHECK(shut_row->get_tooltip_text().find("Force Close") == std::string::npos);
  CHECK(shut_row->get_tooltip_text() == "Desktop panel");
  blocked_window.hide();
  drain();
}

void test_unclosable_tooltip() {
  lundukeabout::AppEntry panel;
  panel.name = "xfce4-panel";
  panel.tooltip = "xfce4-panel";
  panel.pid = 80;
  panel.protected_app = true;
  panel.protect_reason = "Desktop panel";
  panel.comm = "xfce4-panel";
  panel.identity_ok = true;

  lundukeabout::AppEntry ghost;
  ghost.name = "GhostWin";
  ghost.tooltip = "GhostWin";
  ghost.pid = 0;
  ghost.protect_reason = "No process ID";

  lundukeabout::AppEntry system;
  system.name = "LCOS System";
  system.tooltip = "LCOS System\nUnclamped remainder: -5 kB";
  system.pid = 0;
  system.protected_app = true;
  system.protect_reason = "LCOS System";

  Gtk::Window window;
  Gtk::Box box(Gtk::ORIENTATION_VERTICAL, 0);
  auto* panel_row = Gtk::manage(new lundukeabout::AppRow(panel));
  auto* ghost_row = Gtk::manage(new lundukeabout::AppRow(ghost));
  auto* system_row = Gtk::manage(new lundukeabout::AppRow(system));
  box.pack_start(*panel_row, Gtk::PACK_SHRINK);
  box.pack_start(*ghost_row, Gtk::PACK_SHRINK);
  box.pack_start(*system_row, Gtk::PACK_SHRINK);
  window.add(box);
  window.show_all();
  drain();

  CHECK(panel_row->get_tooltip_text() == "Desktop panel");
  CHECK(ghost_row->get_tooltip_text() == "No process ID");
  CHECK(system_row->get_tooltip_text() == "LCOS System\nUnclamped remainder: -5 kB");
  CHECK(panel_row->get_tooltip_text().find("Force Close") == std::string::npos);
  CHECK(ghost_row->get_tooltip_text().find("Menu key") == std::string::npos);
  CHECK(system_row->get_tooltip_text().find("Force Close") == std::string::npos);
  window.hide();
  drain();
}

void test_distinguish_column() {
  const std::string long_name =
      "An Extremely Long Application Class Name That Will Not Fit In The Row";
  lundukeabout::AppEntry a = closable_entry(long_name + " (pid 6804)", 6804);
  a.distinguish = "pid 6804";
  a.tooltip = long_name;
  lundukeabout::AppEntry b = closable_entry(long_name + " (pid 6808)", 6808);
  b.distinguish = "pid 6808";
  b.tooltip = long_name;

  Gtk::Window window;
  window.set_default_size(520, 160);
  Gtk::Box box(Gtk::ORIENTATION_VERTICAL, 0);
  auto* row_a = Gtk::manage(new lundukeabout::AppRow(a));
  auto* row_b = Gtk::manage(new lundukeabout::AppRow(b));
  box.pack_start(*row_a, Gtk::PACK_SHRINK);
  box.pack_start(*row_b, Gtk::PACK_SHRINK);
  window.add(box);
  window.show_all();
  drain();

  Gtk::Label* name_a = find_label(*row_a, long_name);
  Gtk::Label* pid_a = find_label(*row_a, "pid 6804");
  Gtk::Label* name_b = find_label(*row_b, long_name);
  Gtk::Label* pid_b = find_label(*row_b, "pid 6808");
  CHECK(name_a != nullptr);
  CHECK(pid_a != nullptr);
  CHECK(name_b != nullptr);
  CHECK(pid_b != nullptr);
  if (name_a && name_b) {
    CHECK(name_a->get_text() == name_b->get_text());
    CHECK(name_a->get_ellipsize() == Pango::ELLIPSIZE_END);
  }
  if (pid_a && pid_b) {
    CHECK(pid_a->get_ellipsize() == Pango::ELLIPSIZE_NONE);
    CHECK(pid_b->get_ellipsize() == Pango::ELLIPSIZE_NONE);
    CHECK(pid_a->get_visible());
    CHECK(pid_b->get_visible());
    CHECK(pid_a->get_text() != pid_b->get_text());
  }
  if (pid_a && name_a && row_a->get_allocated_height() > 1) {
    const int height = std::max(36, row_a->get_allocated_height());
    row_a->size_allocate(Gtk::Allocation(0, 0, 420, height));
    int pid_min = 0;
    int pid_nat = 0;
    pid_a->get_preferred_width(pid_min, pid_nat);
    CHECK(pid_a->get_allocated_width() + 1 >= pid_nat);
    int name_min = 0;
    int name_nat = 0;
    name_a->get_preferred_width(name_min, name_nat);
    CHECK(name_nat > name_a->get_allocated_width());
  }
  const std::string tip_a = row_a->get_tooltip_text();
  const std::string tip_b = row_b->get_tooltip_text();
  CHECK(tip_a.find("pid 6804") != std::string::npos);
  CHECK(tip_b.find("pid 6808") != std::string::npos);
  CHECK(tip_a.find(long_name) != std::string::npos);
  CHECK(tip_a.find("Right-click or press the Menu key to Force Close") != std::string::npos);
  CHECK(tip_a != tip_b);
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
  test_click_and_keys_agree();
  test_unclosable_tooltip();
  test_distinguish_column();
  test_window_layout();
  if (g_failures != 0) {
    std::cerr << g_failures << " failure(s)\n";
    return 1;
  }
  std::cout << "ok\n";
  return 0;
}
