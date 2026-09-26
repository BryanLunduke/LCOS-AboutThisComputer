// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gtkmm.h>
#include <string>

namespace lundukeabout {

// Full-width Mac OS 9–style RAM bar: blue used fill, light free remainder,
// beveled frame, overlay labels "X RAM Used" / "X RAM Free".
class MemoryBar : public Gtk::DrawingArea {
public:
  MemoryBar();
  void set_memory(long used_kb, long total_kb);

protected:
  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;

private:
  long used_kb_ = 0;
  long total_kb_ = 0;
};

}  // namespace lundukeabout
