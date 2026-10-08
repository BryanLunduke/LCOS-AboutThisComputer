// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gtkmm.h>
#include <string>

namespace lundukeabout {

// One caption after layout. `below` is outside the bar, so the glyphs do
// not sit on either fill.
struct MemoryCaptionBox {
  double x = 0;
  double y = 0;
  double w = 0;
  double h = 0;
  bool visible = false;
  bool below = false;
};

// Bar geometry plus the two captions. blue_w is the painted used fill.
// It can be wider or narrower than width * used fraction so each caption
// that stays on the bar fits in its own fill. widget_height grows when a
// caption has to sit under the bar.
struct MemoryBarLayout {
  double bar_x = 0;
  double bar_y = 0;
  double bar_w = 0;
  double bar_h = 0;
  double blue_w = 0;
  MemoryCaptionBox used;
  MemoryCaptionBox free;
  int widget_height = 0;
};

// width and height are the widget allocation in pixels. used_frac is 0..1.
// Text sizes are the measured caption extents at whatever font is in use.
// A free caption is never placed on the blue fill, and a used caption is
// never placed on the light fill, at every fraction and every text size.
MemoryBarLayout layout_memory_captions(double width, double height, double used_frac,
                                       bool show_free, double used_text_w, double used_text_h,
                                       double free_text_w, double free_text_h);

// Full-width Mac OS 9–style RAM bar: blue used fill, light free remainder,
// beveled frame, overlay labels "X RAM Used" / "X RAM Free".
class MemoryBar : public Gtk::DrawingArea {
public:
  MemoryBar();
  // used_amount and free_amount are already rounded together (see
  // format_memory_readout). The bar does not round them again.
  void set_memory(long used_kb, long total_kb, const std::string& used_amount,
                  const std::string& free_amount);
  // MemTotal was missing. Draw an empty bar labelled Unknown, not 0 MB.
  void set_unknown();
  // Layout at a given allocation, using the captions and the widget font.
  MemoryBarLayout layout_at(int width, int height);

protected:
  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;

private:
  void measure_captions(double& used_w, double& used_h, double& free_w, double& free_h);
  void apply_layout_height(const MemoryBarLayout& layout);

  long used_kb_ = 0;
  long total_kb_ = 0;
  bool unknown_ = false;
  bool show_free_ = false;
  std::string used_caption_;
  std::string free_caption_;
};

}  // namespace lundukeabout
