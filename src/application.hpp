// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gtkmm.h>

namespace lundukeabout {

class Application : public Gtk::Application {
public:
  static Glib::RefPtr<Application> create();

protected:
  Application();
  void on_activate() override;
};

}  // namespace lundukeabout
