// SPDX-License-Identifier: GPL-3.0-or-later
#include "memory_bar.hpp"

#include <algorithm>
#include <cmath>

namespace lundukeabout {

MemoryBarLayout layout_memory_captions(double width, double height, double used_frac,
                                       bool show_free, double used_text_w, double used_text_h,
                                       double free_text_w, double free_text_h) {
  MemoryBarLayout out;
  const double x = 0.5;
  const double y = 0.5;
  const double bar_w = std::max(0.0, width - 1.0);
  const double bar_h = std::max(0.0, height - 1.0);
  out.bar_x = x;
  out.bar_y = y;
  out.bar_w = bar_w;
  out.bar_h = bar_h;
  out.widget_height = static_cast<int>(std::ceil(std::max(1.0, height)));
  if (bar_w <= 1.0 || bar_h <= 1.0) return out;

  used_frac = std::clamp(used_frac, 0.0, 1.0);
  const double pad = 3.0;
  const double true_blue = bar_w * used_frac;
  const bool used_text = used_text_w > 0.5 && used_text_h > 0.5;
  const bool free_text = show_free && free_text_w > 0.5 && free_text_h > 0.5;
  const bool vertically = (!used_text || used_text_h <= bar_h - 1.0) &&
                          (!free_text || free_text_h <= bar_h - 1.0);

  auto fits = [&](double seg, double tw, double th) {
    return seg + 0.01 >= tw + pad * 2.0 && th <= bar_h - 1.0;
  };

  double blue = true_blue;
  bool used_inside = used_text && fits(blue, used_text_w, used_text_h);
  bool free_inside = free_text && fits(bar_w - blue, free_text_w, free_text_h);

  // Move the boundary so each caption that stays on the bar has a segment
  // of its own. The picture is no longer a strict gauge once a caption
  // would otherwise land on the other fill.
  if (vertically && free_text && used_text && !(used_inside && free_inside)) {
    const double min_used = used_text_w + pad * 2.0;
    const double min_free = free_text_w + pad * 2.0;
    if (min_used + min_free <= bar_w + 0.01) {
      blue = std::clamp(true_blue, min_used, std::max(min_used, bar_w - min_free));
      used_inside = true;
      free_inside = true;
    }
  }

  out.blue_w = blue;

  auto place_inside = [&](MemoryCaptionBox& box, double seg_x, double seg_w, double tw, double th) {
    box.visible = true;
    box.below = false;
    box.w = tw;
    box.h = th;
    const double min_x = seg_x + 1.0;
    const double max_x = seg_x + seg_w - tw - 1.0;
    double tx = seg_x + (seg_w - tw) / 2.0;
    if (max_x >= min_x) tx = std::clamp(tx, min_x, max_x);
    else tx = seg_x;
    box.x = tx;
    box.y = y + std::max(0.0, (bar_h - th) / 2.0);
  };

  const double below_y = y + bar_h + 3.0;
  double extra = 0.0;

  if (used_text && used_inside) {
    place_inside(out.used, x, blue, used_text_w, used_text_h);
  } else if (used_text) {
    out.used.visible = true;
    out.used.below = true;
    out.used.w = used_text_w;
    out.used.h = used_text_h;
    out.used.x = x + 2.0;
    out.used.y = below_y;
    extra = std::max(extra, used_text_h);
  }

  if (free_text && free_inside) {
    place_inside(out.free, x + blue, std::max(0.0, bar_w - blue), free_text_w, free_text_h);
  } else if (free_text) {
    out.free.visible = true;
    out.free.below = true;
    out.free.w = free_text_w;
    out.free.h = free_text_h;
    out.free.x = x + std::max(0.0, bar_w - free_text_w - 2.0);
    if (out.used.below) {
      const bool collide = out.used.x + out.used.w + 6.0 > out.free.x;
      if (collide) {
        out.free.y = below_y + used_text_h + 2.0;
        extra = std::max(extra, used_text_h + 2.0 + free_text_h);
      } else {
        out.free.y = below_y;
        extra = std::max(extra, std::max(used_text_h, free_text_h));
      }
    } else {
      out.free.y = below_y;
      extra = std::max(extra, free_text_h);
    }
  }

  if (extra > 0.0) {
    out.widget_height = static_cast<int>(std::ceil(below_y + extra + 2.0));
  }
  return out;
}

MemoryBar::MemoryBar() {
  set_size_request(200, 22);
  set_hexpand(true);
  set_halign(Gtk::ALIGN_FILL);
  set_valign(Gtk::ALIGN_CENTER);
}

void MemoryBar::set_unknown() {
  unknown_ = true;
  used_kb_ = 0;
  total_kb_ = 0;
  show_free_ = false;
  used_caption_.clear();
  free_caption_.clear();
  queue_draw();
}

void MemoryBar::set_memory(long used_kb, long total_kb, const std::string& used_amount,
                           const std::string& free_amount) {
  unknown_ = false;
  const long raw_used = used_kb;
  const long raw_total = total_kb;
  used_kb_ = std::max(0L, raw_used);
  total_kb_ = std::max(0L, raw_total);
  if (total_kb_ > 0 && used_kb_ > total_kb_) used_kb_ = total_kb_;
  const long free_kb = (raw_total > raw_used) ? (raw_total - raw_used) : 0;
  show_free_ = free_kb > 0 && !free_amount.empty();
  used_caption_ = used_amount + " RAM Used";
  free_caption_ = free_amount + " RAM Free";
  queue_draw();
}

void MemoryBar::measure_captions(double& used_w, double& used_h, double& free_w, double& free_h) {
  auto measure = [&](const std::string& text, double& tw, double& th) {
    tw = 0;
    th = 0;
    if (text.empty()) return;
    auto layout = create_pango_layout(text);
    Pango::FontDescription fd = get_style_context()->get_font();
    if (fd.get_family().empty()) fd.set_family("Sans");
    if (fd.get_size() <= 0) fd.set_size(10 * PANGO_SCALE);
    fd.set_weight(Pango::WEIGHT_BOLD);
    layout->set_font_description(fd);
    int pw = 0;
    int ph = 0;
    layout->get_pixel_size(pw, ph);
    tw = pw;
    th = ph;
  };
  measure(used_caption_, used_w, used_h);
  if (show_free_) measure(free_caption_, free_w, free_h);
  else {
    free_w = 0;
    free_h = 0;
  }
}

void MemoryBar::apply_layout_height(const MemoryBarLayout& layout) {
  int req_w = 0;
  int req_h = 0;
  get_size_request(req_w, req_h);
  const int want = std::max(22, layout.widget_height);
  if (want != req_h) set_size_request(std::max(req_w, 200), want);
}

MemoryBarLayout MemoryBar::layout_at(int width, int height) {
  double used_w = 0;
  double used_h = 0;
  double free_w = 0;
  double free_h = 0;
  measure_captions(used_w, used_h, free_w, free_h);
  const double frac = (!unknown_ && total_kb_ > 0)
                          ? std::clamp(static_cast<double>(used_kb_) / total_kb_, 0.0, 1.0)
                          : 0.0;
  return layout_memory_captions(width, height, frac, show_free_ && !unknown_, used_w, used_h, free_w,
                                free_h);
}

bool MemoryBar::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
  const int w = get_allocated_width();
  const int h = get_allocated_height();
  if (w <= 4 || h <= 4) return true;

  if (unknown_) {
    const double x = 0.5;
    const double y = 0.5;
    const double bar_w = w - 1.0;
    const double bar_h = h - 1.0;
    cr->set_source_rgb(0.92, 0.92, 0.94);
    cr->rectangle(x, y, bar_w, bar_h);
    cr->fill();
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
    auto layout = create_pango_layout("Unknown");
    Pango::FontDescription fd;
    fd.set_family("Sans");
    fd.set_size(10 * PANGO_SCALE);
    fd.set_weight(Pango::WEIGHT_BOLD);
    layout->set_font_description(fd);
    int tw = 0;
    int th = 0;
    layout->get_pixel_size(tw, th);
    cr->set_source_rgb(0.1, 0.1, 0.1);
    cr->move_to(x + std::max(0.0, (bar_w - tw) / 2.0), y + std::max(0.0, (bar_h - th) / 2.0));
    layout->show_in_cairo_context(cr);
    return true;
  }

  // The gauge itself stays one row tall. Extra allocation is the caption
  // strip under it, used only when the text cannot sit in its own fill.
  const int bar_px = (h > 26) ? 22 : h;
  double used_tw = 0;
  double used_th = 0;
  double free_tw = 0;
  double free_th = 0;
  measure_captions(used_tw, used_th, free_tw, free_th);
  const double used_frac =
      (total_kb_ > 0) ? std::clamp(static_cast<double>(used_kb_) / total_kb_, 0.0, 1.0) : 0.0;
  const MemoryBarLayout layout = layout_memory_captions(
      w, bar_px, used_frac, show_free_, used_tw, used_th, free_tw, free_th);
  apply_layout_height(layout);

  const double x = layout.bar_x;
  const double y = layout.bar_y;
  const double bar_w = layout.bar_w;
  const double bar_h = layout.bar_h;
  const double used_w = std::clamp(layout.blue_w, 0.0, bar_w);

  cr->set_source_rgb(0.92, 0.92, 0.94);
  cr->rectangle(x, y, bar_w, bar_h);
  cr->fill();

  if (used_w > 0.5) {
    cr->set_source_rgb(0.20, 0.35, 0.85);
    cr->rectangle(x, y, used_w, bar_h);
    cr->fill();

    cr->set_source_rgba(1.0, 1.0, 1.0, 0.30);
    cr->rectangle(x, y, used_w, std::max(2.0, bar_h * 0.35));
    cr->fill();
  }

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

  auto paint = [&](const std::string& text, const MemoryCaptionBox& box, bool on_blue) {
    if (!box.visible || text.empty()) return;
    auto layout = create_pango_layout(text);
    Pango::FontDescription fd = get_style_context()->get_font();
    if (fd.get_family().empty()) fd.set_family("Sans");
    if (fd.get_size() <= 0) fd.set_size(10 * PANGO_SCALE);
    fd.set_weight(Pango::WEIGHT_BOLD);
    layout->set_font_description(fd);
    if (on_blue && !box.below) cr->set_source_rgb(1.0, 1.0, 1.0);
    else cr->set_source_rgb(0.10, 0.10, 0.10);
    cr->move_to(box.x, box.y);
    layout->show_in_cairo_context(cr);
  };

  paint(used_caption_, layout.used, true);
  paint(free_caption_, layout.free, false);
  return true;
}

}  // namespace lundukeabout
