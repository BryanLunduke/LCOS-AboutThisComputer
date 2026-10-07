// SPDX-License-Identifier: GPL-3.0-or-later
#include "application.hpp"

#include <gtk/gtk.h>

#include <iostream>

int main(int argc, char* argv[]) {
  g_set_prgname("lunduke-about");
  g_set_application_name("About This Computer");
  // Leave GDK_BACKEND alone. Forcing x11 makes a missing display fail inside
  // GTK before the in-window "Needs X11" row can be shown.
  if (!gtk_init_check(&argc, &argv)) {
    std::cerr << "Needs X11\nThis view needs an X11 display.\n";
    return 1;
  }

  auto app = lundukeabout::Application::create();
  return app->run(argc, argv);
}
