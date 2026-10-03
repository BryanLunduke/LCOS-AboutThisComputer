// SPDX-License-Identifier: GPL-3.0-or-later
#include "application.hpp"
#include "main_window.hpp"
#include "config.h"

namespace lundukeabout {

Glib::RefPtr<Application> Application::create() {
  return Glib::RefPtr<Application>(new Application());
}

Application::Application()
    : Gtk::Application(APP_ID, Gio::APPLICATION_FLAGS_NONE) {
  Gtk::Window::set_default_icon_name(APP_ID);
}

void Application::on_activate() {
  // A second launch (desktop file, menu) activates this process again.
  // Present the window we already have instead of stacking another one.
  const auto windows = get_windows();
  if (!windows.empty()) {
    windows.front()->present();
    return;
  }
  auto* window = new MainWindow();
  add_window(*window);
  window->present();
}

}  // namespace lundukeabout
