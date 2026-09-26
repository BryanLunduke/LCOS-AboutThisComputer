// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gtkmm.h>
#include <string>

namespace lundukeabout {

class Marquee : public Gtk::DrawingArea {
public:
  Marquee();
  void set_text(const std::string& text);

protected:
  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;
  void on_size_allocate(Gtk::Allocation& allocation) override;

private:
  bool on_tick();
  std::string text_;
  double offset_ = 0.0;
  double text_width_ = 0.0;
  sigc::connection tick_;
};

}  // namespace lundukeabout
