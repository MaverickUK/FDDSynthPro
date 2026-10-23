// Drawing the front panel's OLED screens onto a Canvas (128x64). Pure C++,
// host-tested; the tests render every screen to images (docs/oled).
//
// The HDD Synth's look: a 10-pixel lit title bar with dark text, the content
// below it.
#pragma once

#include "canvas.h"
#include "ui.h"

namespace screens {

constexpr int TITLE_H = 10;

void draw(Canvas& c, const ui::Model& m, const ui::Status& s, const ui::HeadTrace& trace);

// Each screen on its own (draw() picks one).
void home(Canvas& c, const ui::Status& s);
void disks(Canvas& c, const ui::Status& s, size_t selection);
void swap(Canvas& c, const ui::Status& s, size_t to);
void volume(Canvas& c, int percent);
void headTrace(Canvas& c, const ui::Status& s, const ui::HeadTrace& trace);
void sounds(Canvas& c, const ui::Status& s);
void stats(Canvas& c, const ui::Status& s);

}  // namespace screens
