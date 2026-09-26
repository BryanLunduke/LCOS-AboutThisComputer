// SPDX-License-Identifier: GPL-3.0-or-later
#include "application.hpp"

#include <glib.h>

int main(int argc, char* argv[]) {
  g_set_prgname("lunduke-about");
  g_set_application_name("About This Computer");
  if (g_getenv("GDK_BACKEND") == nullptr) {
    g_setenv("GDK_BACKEND", "x11", FALSE);
  }

  auto app = lundukeabout::Application::create();
  return app->run(argc, argv);
}
