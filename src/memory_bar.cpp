// SPDX-License-Identifier: GPL-3.0-or-later
#include "memory_bar.hpp"

#include <algorithm>

namespace lundukeabout {

MemoryBar::MemoryBar() {
  set_size_request(120, 14);
  set_hexpand(true);
  set_valign(Gtk::ALIGN_CENTER);
}

void MemoryBar::set_usage(double used_fraction) {
  used_ = std::clamp(used_fraction, 0.0, 1.0);
  queue_draw();
}

void MemoryBar::set_relative_width(double relative) {
  relative_ = std::clamp(relative, 0.08, 1.0);
  queue_draw();
}

bool MemoryBar::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
  const int w = get_allocated_width();
  const int h = get_allocated_height();
  if (w <= 2 || h <= 2) return true;

  const double bar_w = std::max(8.0, w * relative_);
  const double bar_h = std::min(12.0, static_cast<double>(h) - 2.0);
  const double x = 0.5;
  const double y = (h - bar_h) / 2.0;

  // Outer frame (allocated) — light fill + dark bevel like Platinum
  cr->set_source_rgb(0.92, 0.92, 0.92);
  cr->rectangle(x, y, bar_w, bar_h);
  cr->fill();

  // Used (blue) with slight top highlight
  const double fill_w = bar_w * used_;
  if (fill_w > 0.5) {
    cr->set_source_rgb(0.20, 0.35, 0.85);  // Mac-ish blue
    cr->rectangle(x, y, fill_w, bar_h);
    cr->fill();

    // Top highlight stripe
    cr->set_source_rgba(1.0, 1.0, 1.0, 0.35);
    cr->rectangle(x, y, fill_w, std::max(2.0, bar_h * 0.35));
    cr->fill();
  }

  // Bevel border: white top/left, dark bottom/right
  cr->set_line_width(1.0);
  cr->set_source_rgb(1.0, 1.0, 1.0);
  cr->move_to(x, y + bar_h);
  cr->line_to(x, y);
  cr->line_to(x + bar_w, y);
  cr->stroke();

  cr->set_source_rgb(0.25, 0.25, 0.25);
  cr->move_to(x + bar_w, y);
  cr->line_to(x + bar_w, y + bar_h);
  cr->line_to(x, y + bar_h);
  cr->stroke();

  return true;
}

}  // namespace lundukeabout
