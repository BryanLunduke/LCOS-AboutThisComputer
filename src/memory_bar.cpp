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

  // Center a caption in its segment when it fits. A segment under ~25% is
  // too narrow for that, but the caption still has to stay on the bar:
  // pin it to the outer edge instead of dropping it.
  auto make_layout = [&](const std::string& text, int& tw, int& th) {
    auto layout = create_pango_layout(text);
    Pango::FontDescription fd;
    fd.set_family("Sans");
    fd.set_size(10 * PANGO_SCALE);
    fd.set_weight(Pango::WEIGHT_BOLD);
    layout->set_font_description(fd);
    layout->get_pixel_size(tw, th);
    return layout;
  };

  const std::string used_text = format_memory_human(used_kb_) + " RAM Used";
  int utw = 0, uth = 0;
  auto used_layout = make_layout(used_text, utw, uth);

  const bool show_free = free_kb > 0;
  int ftw = 0, fth = 0;
  Glib::RefPtr<Pango::Layout> free_layout;
  std::string free_text;
  if (show_free) {
    free_text = format_memory_human(free_kb) + " RAM Free";
    free_layout = make_layout(free_text, ftw, fth);
  }

  auto fits = [](double region_w, int tw) {
    return region_w >= static_cast<double>(tw) + 2.0;
  };

  double used_tx = fits(used_w, utw) ? (x + (used_w - utw) / 2.0) : (x + 3.0);
  double free_tx = 0.0;
  if (show_free) {
    free_tx = fits(free_w, ftw) ? (x + used_w + (free_w - ftw) / 2.0)
                                : (x + bar_w - ftw - 3.0);
    if (used_tx + utw > free_tx - 4.0) {
      used_tx = x + 3.0;
      free_tx = x + bar_w - ftw - 3.0;
    }
  }

  auto clamp_x = [&](double tx, int tw) {
    const double min_x = x + 2.0;
    const double max_x = x + bar_w - tw - 2.0;
    if (max_x < min_x) return min_x;
    return std::clamp(tx, min_x, max_x);
  };
  used_tx = clamp_x(used_tx, utw);
  if (show_free) free_tx = clamp_x(free_tx, ftw);

  const double used_right = x + used_w;
  auto paint = [&](const Glib::RefPtr<Pango::Layout>& layout, double tx, double ty,
                   int tw) {
    const double x1 = tx;
    const double x2 = tx + tw;
    const bool fully_on_blue = x2 <= used_right + 0.5;
    const bool fully_on_light = x1 >= used_right - 0.5;
    const bool crossing = !fully_on_blue && !fully_on_light;
    if (crossing) {
      cr->set_source_rgb(0.05, 0.05, 0.05);
      for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
          if (dx == 0 && dy == 0) continue;
          cr->move_to(tx + dx, ty + dy);
          layout->show_in_cairo_context(cr);
        }
      }
      cr->set_source_rgb(1.0, 1.0, 1.0);
    } else if (fully_on_blue) {
      cr->set_source_rgb(1.0, 1.0, 1.0);
    } else {
      cr->set_source_rgb(0.10, 0.10, 0.10);
    }
    cr->move_to(tx, ty);
    layout->show_in_cairo_context(cr);
  };

  paint(used_layout, used_tx, y + (bar_h - uth) / 2.0, utw);
  if (show_free) {
    paint(free_layout, free_tx, y + (bar_h - fth) / 2.0, ftw);
  }

  return true;
}

}  // namespace lundukeabout
