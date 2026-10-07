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
  // used_amount and free_amount are already rounded together (see
  // format_memory_readout). The bar does not round them again.
  void set_memory(long used_kb, long total_kb, const std::string& used_amount,
                  const std::string& free_amount);

protected:
  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;

private:
  long used_kb_ = 0;
  long total_kb_ = 0;
  bool show_free_ = false;
  std::string used_caption_;
  std::string free_caption_;
};

}  // namespace lundukeabout
