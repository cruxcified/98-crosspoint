#include "Win98Chrome.h"

#include <GfxRenderer.h>

namespace win98 {

void drawRaised(const GfxRenderer& r, int x, int y, int w, int h) {
  r.drawLine(x, y, x + w - 2, y, false);
  r.drawLine(x, y, x, y + h - 2, false);
  r.drawLine(x, y + h - 1, x + w - 1, y + h - 1, true);
  r.drawLine(x + w - 1, y, x + w - 1, y + h - 1, true);
  r.fillRectDither(x + 1, y + h - 2, w - 2, 1, Color::DarkGray);
  r.fillRectDither(x + w - 2, y + 1, 1, h - 2, Color::DarkGray);
}

void drawSunken(const GfxRenderer& r, int x, int y, int w, int h) {
  r.fillRectDither(x, y, w - 1, 1, Color::DarkGray);
  r.fillRectDither(x, y, 1, h - 1, Color::DarkGray);
  r.drawLine(x + 1, y + 1, x + w - 3, y + 1, true);
  r.drawLine(x + 1, y + 1, x + 1, y + h - 3, true);
  r.drawLine(x, y + h - 1, x + w - 1, y + h - 1, false);
  r.drawLine(x + w - 1, y, x + w - 1, y + h - 1, false);
}

void drawButton(const GfxRenderer& r, int x, int y, int w, int h) {
  r.fillRect(x, y, w, h, false);
  r.drawRect(x, y, w, h, true);
  drawRaised(r, x + 1, y + 1, w - 2, h - 2);
}

void drawArrow(const GfxRenderer& r, int cx, int cy, bool up) {
  for (int i = 0; i < 5; ++i) {
    const int half = up ? i : 4 - i;
    r.drawLine(cx - half, cy - 2 + i, cx + half, cy - 2 + i, true);
  }
}

}  // namespace win98
