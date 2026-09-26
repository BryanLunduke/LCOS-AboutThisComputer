// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gtkmm.h>

namespace lundukeabout {

// Classic Mac OS 9 style memory usage bar: outlined frame = allocated,
// blue fill = used. Drawn with Cairo.
class MemoryBar : public Gtk::DrawingArea {
public:
  MemoryBar();
  void set_usage(double used_fraction);  // 0..1 within the bar
  void set_relative_width(double relative);  // 0..1 of max row width

protected:
  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;

private:
  double used_ = 0.0;
  double relative_ = 1.0;
};

}  // namespace lundukeabout
