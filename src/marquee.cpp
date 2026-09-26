// SPDX-License-Identifier: GPL-3.0-or-later
#include "marquee.hpp"

namespace lundukeabout {

Marquee::Marquee() {
  set_size_request(-1, 22);
  set_hexpand(true);
  tick_ = Glib::signal_timeout().connect(
      sigc::mem_fun(*this, &Marquee::on_tick), 30);
}

void Marquee::set_text(const std::string& text) {
  text_ = text;
  offset_ = 0.0;
  queue_draw();
}

void Marquee::on_size_allocate(Gtk::Allocation& allocation) {
  Gtk::DrawingArea::on_size_allocate(allocation);
}

bool Marquee::on_tick() {
  offset_ += 0.6;  // slow scroll left
  if (text_width_ > 0.0 && offset_ > text_width_ + 40.0) {
    offset_ = -static_cast<double>(get_allocated_width());
  }
  queue_draw();
  return true;
}

bool Marquee::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
  const int w = get_allocated_width();
  const int h = get_allocated_height();

  // Sunken platinum trough
  cr->set_source_rgb(0.85, 0.85, 0.85);
  cr->rectangle(0, 0, w, h);
  cr->fill();

  cr->set_line_width(1.0);
  cr->set_source_rgb(0.35, 0.35, 0.35);
  cr->move_to(0.5, h - 0.5);
  cr->line_to(0.5, 0.5);
  cr->line_to(w - 0.5, 0.5);
  cr->stroke();
  cr->set_source_rgb(1.0, 1.0, 1.0);
  cr->move_to(w - 0.5, 0.5);
  cr->line_to(w - 0.5, h - 0.5);
  cr->line_to(0.5, h - 0.5);
  cr->stroke();

  auto layout = create_pango_layout(text_);
  Pango::FontDescription fd;
  fd.set_family("Sans");
  fd.set_size(11 * PANGO_SCALE);
  layout->set_font_description(fd);
  int tw = 0, th = 0;
  layout->get_pixel_size(tw, th);
  text_width_ = tw;

  cr->set_source_rgb(0.05, 0.05, 0.05);
  const double x = 6.0 - offset_;
  const double y = (h - th) / 2.0;
  cr->move_to(x, y);
  layout->show_in_cairo_context(cr);

  // Loop: draw a second copy so it wraps smoothly
  if (tw > 0) {
    cr->move_to(x + tw + 48.0, y);
    layout->show_in_cairo_context(cr);
  }

  return true;
}

}  // namespace lundukeabout
