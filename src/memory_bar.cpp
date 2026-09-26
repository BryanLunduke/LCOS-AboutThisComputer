// SPDX-License-Identifier: GPL-3.0-or-later
#include "memory_bar.hpp"
#include "system_info.hpp"

#include <algorithm>
#include <cmath>

namespace lundukeabout {

MemoryBar::MemoryBar() {
  set_size_request(200, 22);
  set_hexpand(true);
  set_halign(Gtk::ALIGN_FILL);
  set_valign(Gtk::ALIGN_CENTER);
}

void MemoryBar::set_memory(long used_kb, long total_kb) {
  used_kb_ = std::max(0L, used_kb);
  total_kb_ = std::max(0L, total_kb);
  if (used_kb_ > total_kb_ && total_kb_ > 0) used_kb_ = total_kb_;
  queue_draw();
}

bool MemoryBar::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
  const int w = get_allocated_width();
  const int h = get_allocated_height();
  if (w <= 4 || h <= 4) return true;

  const double x = 0.5;
  const double y = 0.5;
  const double bar_w = w - 1.0;
  const double bar_h = h - 1.0;

  const double used_frac =
      (total_kb_ > 0) ? std::clamp(static_cast<double>(used_kb_) / total_kb_, 0.0, 1.0)
                      : 0.0;
  const double used_w = bar_w * used_frac;
  const double free_w = bar_w - used_w;
  const long free_kb = (total_kb_ > used_kb_) ? (total_kb_ - used_kb_) : 0;

  // Light unused fill (platinum-ish)
  cr->set_source_rgb(0.92, 0.92, 0.94);
  cr->rectangle(x, y, bar_w, bar_h);
  cr->fill();

  // Blue used fill with top highlight
  if (used_w > 0.5) {
    cr->set_source_rgb(0.20, 0.35, 0.85);
    cr->rectangle(x, y, used_w, bar_h);
    cr->fill();

    cr->set_source_rgba(1.0, 1.0, 1.0, 0.30);
    cr->rectangle(x, y, used_w, std::max(2.0, bar_h * 0.35));
    cr->fill();
  }

  // Beveled frame: light top/left, dark bottom/right (inset look)
  cr->set_line_width(1.0);
  cr->set_source_rgb(0.25, 0.25, 0.25);
  cr->move_to(x, y + bar_h);
  cr->line_to(x, y);
  cr->line_to(x + bar_w, y);
  cr->stroke();

  cr->set_source_rgb(1.0, 1.0, 1.0);
  cr->move_to(x + bar_w, y);
  cr->line_to(x + bar_w, y + bar_h);
  cr->line_to(x, y + bar_h);
  cr->stroke();

  auto draw_centered = [&](const std::string& text, double region_x, double region_w,
                           bool on_blue) {
    if (region_w < 8.0 || text.empty()) return;
    auto layout = create_pango_layout(text);
    Pango::FontDescription fd;
    fd.set_family("Sans");
    fd.set_size(10 * PANGO_SCALE);
    fd.set_weight(Pango::WEIGHT_BOLD);
    layout->set_font_description(fd);
    int tw = 0, th = 0;
    layout->get_pixel_size(tw, th);
    if (tw + 4 > region_w) {
      // Fall back to slightly smaller if it won't fit
      fd.set_size(9 * PANGO_SCALE);
      layout->set_font_description(fd);
      layout->get_pixel_size(tw, th);
    }
    if (tw + 2 > region_w) return;  // still too wide — omit rather than clip badly
    const double tx = region_x + (region_w - tw) / 2.0;
    const double ty = y + (bar_h - th) / 2.0;
    if (on_blue) {
      cr->set_source_rgb(1.0, 1.0, 1.0);
    } else {
      cr->set_source_rgb(0.10, 0.10, 0.10);
    }
    cr->move_to(tx, ty);
    layout->show_in_cairo_context(cr);
  };

  const std::string used_text = format_memory_human(used_kb_) + " RAM Used";
  draw_centered(used_text, x, used_w, true);

  if (free_kb > 0) {
    const std::string free_text = format_memory_human(free_kb) + " RAM Free";
    draw_centered(free_text, x + used_w, free_w, false);
  }

  return true;
}

}  // namespace lundukeabout
