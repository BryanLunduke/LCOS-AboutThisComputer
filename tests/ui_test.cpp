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
#include <gdk/gdk.h>

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
  CHECK(tip.find("Right-click or press the Menu key or Shift+F10 to Force Close") !=
        std::string::npos);
  CHECK(tip.find("Shift+F10") != std::string::npos);
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
  CHECK(tip_a.find("Right-click or press the Menu key or Shift+F10 to Force Close") !=
        std::string::npos);
  CHECK(tip_a.find("Shift+F10") != std::string::npos);
  CHECK(tip_a != tip_b);
  window.hide();
  drain();
}

struct FocusSample {
  int focus = 0;
  int white = 0;
  int dark = 0;
  int total = 0;
  int r = -1;
  int g = -1;
  int b = -1;
  bool ok = false;
};

FocusSample sample_row_background(Gtk::Widget& widget) {
  FocusSample out;
  auto window = widget.get_window();
  if (!window) return out;
  const int width = widget.get_allocated_width();
  const int height = widget.get_allocated_height();
  if (width < 16 || height < 8) return out;
  Glib::RefPtr<Gdk::Pixbuf> pix;
  try {
    pix = Gdk::Pixbuf::create(window, 0, 0, width, height);
  } catch (const Glib::Error&) {
    return out;
  }
  if (!pix || pix->get_n_channels() < 3) return out;
  const int n = pix->get_n_channels();
  const int stride = pix->get_rowstride();
  const guint8* pixels = pix->get_pixels();
  for (int y = 0; y < pix->get_height(); ++y) {
    for (int x = 0; x < pix->get_width(); ++x) {
      const guint8* p = pixels + y * stride + x * n;
      if (p[0] < 80 && p[1] < 80 && p[2] < 80) ++out.dark;
    }
  }
  // Middle of the row, clear of the short name on the left and the RAM
  // caption on the right.
  const int x0 = width / 2 - 4;
  const int x1 = width / 2 + 4;
  for (int y = 0; y < pix->get_height(); ++y) {
    for (int x = x0; x < x1 && x < pix->get_width(); ++x) {
      if (x < 0) continue;
      const guint8* p = pixels + y * stride + x * n;
      ++out.total;
      if (out.r < 0) {
        out.r = p[0];
        out.g = p[1];
        out.b = p[2];
      }
      const int dr = static_cast<int>(p[0]) - 0xE4;
      const int dg = static_cast<int>(p[1]) - 0xEA;
      const int db = static_cast<int>(p[2]) - 0xF6;
      if (dr >= -4 && dr <= 4 && dg >= -4 && dg <= 4 && db >= -4 && db <= 4) ++out.focus;
      if (p[0] >= 250 && p[1] >= 250 && p[2] >= 250) ++out.white;
    }
  }
  out.ok = out.total > 0;
  return out;
}

void settle_draw(Gtk::Widget& widget) {
  widget.queue_draw();
  drain();
  if (auto window = widget.get_window()) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    gdk_window_process_updates(window->gobj(), TRUE);
#pragma GCC diagnostic pop
  }
  if (GdkDisplay* display = gdk_display_get_default()) gdk_display_flush(display);
  drain();
}

void test_row_focus_visible() {
  lundukeabout::AppEntry first = closable_entry("Keeper", 43);
  lundukeabout::AppEntry second = closable_entry("GhostWin", 44);
  Gtk::Window window;
  window.set_default_size(520, 160);
  Gtk::Box box(Gtk::ORIENTATION_VERTICAL, 0);
  box.set_hexpand(true);
  auto* row_a = Gtk::manage(new lundukeabout::AppRow(first));
  auto* row_b = Gtk::manage(new lundukeabout::AppRow(second));
  row_a->set_hexpand(true);
  row_b->set_hexpand(true);
  row_a->set_size_request(480, 36);
  row_b->set_size_request(480, 36);
  box.pack_start(*row_a, Gtk::PACK_SHRINK);
  box.pack_start(*row_b, Gtk::PACK_SHRINK);
  window.add(box);
  window.show_all();
  drain();

  row_a->grab_focus();
  settle_draw(*row_a);
  settle_draw(*row_b);
  CHECK(row_a->is_focus());
  CHECK(!row_b->is_focus());
  const FocusSample focused = sample_row_background(*row_a);
  const FocusSample idle = sample_row_background(*row_b);
  if (!focused.ok || focused.focus * 2 < focused.total) {
    std::cerr << "focused row pixels r=" << focused.r << " g=" << focused.g
              << " b=" << focused.b << " focus=" << focused.focus
              << " white=" << focused.white << " total=" << focused.total << "\n";
  }
  CHECK(focused.ok);
  CHECK(focused.total > 0);
  CHECK(focused.focus * 2 >= focused.total);
  CHECK(focused.dark > 0);
  CHECK(idle.ok);
  CHECK(idle.white * 2 >= idle.total);
  CHECK(idle.focus * 4 < idle.total + 4);

  g_signal_emit_by_name(window.gobj(), "move-focus", GTK_DIR_TAB_FORWARD);
  drain();
  CHECK(row_b->is_focus());
  CHECK(!row_a->is_focus());
  settle_draw(*row_a);
  settle_draw(*row_b);
  const FocusSample now_b = sample_row_background(*row_b);
  const FocusSample now_a = sample_row_background(*row_a);
  if (!now_b.ok || now_b.focus * 2 < now_b.total) {
    std::cerr << "tabbed row pixels r=" << now_b.r << " g=" << now_b.g << " b=" << now_b.b
              << " focus=" << now_b.focus << " white=" << now_b.white
              << " total=" << now_b.total << "\n";
  }
  CHECK(now_b.ok);
  CHECK(now_b.focus * 2 >= now_b.total);
  CHECK(now_a.ok);
  CHECK(now_a.white * 2 >= now_a.total);
  CHECK(now_a.focus * 4 < now_a.total + 4);

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

bool label_shows_all(Gtk::Label& label) {
  if (label.get_ellipsize() != Pango::ELLIPSIZE_NONE) return false;
  if (!label.get_line_wrap()) return false;
  auto layout = label.get_layout();
  if (!layout) return false;
  if (layout->is_ellipsized()) return false;
  int text_w = 0;
  int text_h = 0;
  layout->get_pixel_size(text_w, text_h);
  if (label.get_allocated_width() < 8 || label.get_allocated_height() < 8) return false;
  if (text_w > label.get_allocated_width() + 1) return false;
  if (text_h > label.get_allocated_height() + 1) return false;
  return true;
}

void test_cpu_gpu_text_visible() {
  const std::string cpu =
      "CPU:  Intel(R) Xeon(R) Processor (4 cores, 4 threads)";
  const std::string gpu =
      "GPU:  Intel Corporation TigerLake-LP GT2 [Iris Xe Graphics]; "
      "Advanced Micro Devices, Inc. [AMD/ATI] Ellesmere "
      "[Radeon RX 470/480/570/570X/580/580X/590]";
  Gtk::Window window;
  window.set_default_size(520, 360);
  window.set_size_request(520, 360);
  Gtk::Box cols(Gtk::ORIENTATION_HORIZONTAL, 24);
  cols.set_margin_start(12);
  cols.set_margin_end(12);
  cols.set_margin_top(10);
  Gtk::Label os;
  lundukeabout::set_info_label(os, "OS Version:  LCOS 0.9");
  Gtk::Box right(Gtk::ORIENTATION_VERTICAL, 2);
  right.set_halign(Gtk::ALIGN_START);
  Gtk::Label cpu_label;
  Gtk::Label gpu_label;
  lundukeabout::set_wrapping_info_label(cpu_label, cpu);
  lundukeabout::set_wrapping_info_label(gpu_label, gpu);
  right.pack_start(cpu_label, Gtk::PACK_SHRINK);
  right.pack_start(gpu_label, Gtk::PACK_SHRINK);
  cols.pack_start(os, Gtk::PACK_EXPAND_WIDGET);
  cols.pack_start(right, Gtk::PACK_EXPAND_WIDGET);
  window.add(cols);
  window.show_all();
  drain();
  window.resize(520, 360);
  drain();

  CHECK(cpu_label.get_text().find("(4 cores, 4 threads)") != std::string::npos);
  CHECK(gpu_label.get_text().find("Ellesmere") != std::string::npos);
  CHECK(gpu_label.get_text().find("Iris Xe") != std::string::npos);
  CHECK(cpu_label.get_text().find('\n') == std::string::npos);
  CHECK(gpu_label.get_text().find('\n') != std::string::npos);
  CHECK(label_shows_all(cpu_label));
  CHECK(label_shows_all(gpu_label));
  CHECK(cpu_label.get_tooltip_text() == cpu);
  CHECK(gpu_label.get_tooltip_text() == gpu);
  int min_w = 0;
  int nat_w = 0;
  window.get_preferred_width(min_w, nat_w);
  CHECK(min_w <= 520);
  CHECK(window.get_allocated_width() <= 560);

  lundukeabout::MainWindow about;
  drain();
  bool saw_cpu = false;
  walk(about, [&](Gtk::Widget& widget) {
    auto* label = dynamic_cast<Gtk::Label*>(&widget);
    if (!label) return;
    const std::string text = label->get_text();
    if (text.rfind("CPU:", 0) != 0) return;
    saw_cpu = true;
    CHECK(label->get_ellipsize() == Pango::ELLIPSIZE_NONE);
    CHECK(label->get_line_wrap());
    CHECK(text.find("cores") != std::string::npos);
    CHECK(text.find("threads") != std::string::npos);
    CHECK(label_shows_all(*label));
  });
  CHECK(saw_cpu);
  int about_min_w = 0;
  int about_nat_w = 0;
  int about_min_h = 0;
  int about_nat_h = 0;
  about.get_preferred_width(about_min_w, about_nat_w);
  about.get_preferred_height(about_min_h, about_nat_h);
  CHECK(about_min_w <= 520);
  CHECK(about_min_h <= 360);
  CHECK(about.get_allocated_width() <= 560);

  window.hide();
  about.hide();
  drain();
}

void test_return_activates_row() {
  lundukeabout::AppEntry first = closable_entry("Keeper", 43);
  lundukeabout::AppEntry second = closable_entry("GhostWin", 44);
  Gtk::Window window;
  Gtk::Box box(Gtk::ORIENTATION_VERTICAL, 0);
  auto* row_a = Gtk::manage(new lundukeabout::AppRow(first));
  auto* row_b = Gtk::manage(new lundukeabout::AppRow(second));
  box.pack_start(*row_a, Gtk::PACK_SHRINK);
  box.pack_start(*row_b, Gtk::PACK_SHRINK);
  window.add(box);
  window.show_all();
  drain();
  row_a->grab_focus();
  drain();
  CHECK(row_a->is_focus());
  CHECK(row_a->get_tooltip_text().find("Shift+F10") != std::string::npos);
  CHECK(row_a->get_tooltip_text().find("Menu key") != std::string::npos);

  send_key(*row_b, GDK_KEY_Return, 0);
  drain();
  CHECK(!row_b->menu_posted());
  CHECK(!row_a->menu_posted());

  send_key(*row_a, GDK_KEY_Return, 0);
  drain();
  CHECK(row_a->menu_posted());
  CHECK(!row_b->menu_posted());
  window.hide();
  drain();

  Gtk::Window pad_window;
  Gtk::Box pad_box(Gtk::ORIENTATION_VERTICAL, 0);
  auto* pad = Gtk::manage(new lundukeabout::AppRow(first));
  pad_box.pack_start(*pad, Gtk::PACK_SHRINK);
  pad_window.add(pad_box);
  pad_window.show_all();
  drain();
  pad->grab_focus();
  drain();
  send_key(*pad, GDK_KEY_KP_Enter, 0);
  drain();
  CHECK(pad->menu_posted());
  pad_window.hide();
  drain();
}

void test_scrollbar_thumb() {
  lundukeabout::MainWindow styled;
  drain();
  Gtk::Window window;
  window.set_default_size(220, 90);
  Gtk::ScrolledWindow scroll;
  scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_ALWAYS);
  scroll.set_overlay_scrolling(false);
  scroll.get_style_context()->add_class("platinum-scroll");
  scroll.set_hexpand(true);
  scroll.set_vexpand(true);
  Gtk::Box box(Gtk::ORIENTATION_VERTICAL, 0);
  for (int i = 0; i < 30; ++i) {
    auto* label = Gtk::manage(new Gtk::Label("Application row " + std::to_string(i)));
    label->set_size_request(-1, 22);
    box.pack_start(*label, Gtk::PACK_SHRINK);
  }
  scroll.add(box);
  window.add(scroll);
  window.show_all();
  drain();
  window.resize(220, 90);
  drain();

  Gtk::Scrollbar* bar = scroll.get_vscrollbar();
  if (!bar) std::cerr << "vscrollbar is null\n";
  CHECK(bar != nullptr);
  if (bar) bar->get_style_context()->add_class("platinum-scroll");
  drain();
  CHECK(scroll.get_style_context()->has_class("platinum-scroll"));
  if (!bar) {
    window.hide();
    styled.hide();
    drain();
    return;
  }
  CHECK(bar->get_style_context()->has_class("platinum-scroll"));
  for (int i = 0; i < 10 && bar->get_allocated_width() < 14; ++i) {
    drain();
    g_usleep(20000);
  }
  const int width = bar->get_allocated_width();
  const int height = bar->get_allocated_height();
  if (width < 14) std::cerr << "scrollbar width " << width << "\n";
  CHECK(width >= 14);
  CHECK(height > 20);

  int origin_x = 0;
  int origin_y = 0;
  const bool placed = bar->translate_coordinates(window, 0, 0, origin_x, origin_y);
  CHECK(placed);
  int blue = 0;
  int sampled = 0;
  if (placed && window.get_window() && width > 0 && height > 0) {
    settle_draw(*bar);
    Glib::RefPtr<Gdk::Pixbuf> pix;
    try {
      pix = Gdk::Pixbuf::create(window.get_window(), origin_x, origin_y, width, height);
    } catch (const Glib::Error& err) {
      std::cerr << "scrollbar capture: " << err.what() << "\n";
    }
    if (pix && pix->get_n_channels() >= 3) {
      const int n = pix->get_n_channels();
      const int stride = pix->get_rowstride();
      const guint8* pixels = pix->get_pixels();
      for (int y = 0; y < pix->get_height(); ++y) {
        for (int x = 0; x < pix->get_width(); ++x) {
          const guint8* p = pixels + y * stride + x * n;
          ++sampled;
          const int r = p[0];
          const int g = p[1];
          const int b = p[2];
          if (b > 150 && b > r + 30 && b > g + 20 && r < 160) ++blue;
        }
      }
    }
  }
  if (blue < 20) {
    std::cerr << "scrollbar blue pixels " << blue << " of " << sampled << " width " << width
              << "\n";
  }
  CHECK(blue >= 20);

  window.hide();
  styled.hide();
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
  test_row_focus_visible();
  test_window_layout();
  test_cpu_gpu_text_visible();
  test_return_activates_row();
  test_scrollbar_thumb();
  if (g_failures != 0) {
    std::cerr << g_failures << " failure(s)\n";
    return 1;
  }
  std::cout << "ok\n";
  return 0;
}
