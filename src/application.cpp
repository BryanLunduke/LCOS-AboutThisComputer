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
  auto* window = new MainWindow();
  add_window(*window);
  window->present();
}

}  // namespace lundukeabout
