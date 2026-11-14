#pragma once

class GfxRenderer;

// Win98-style 1-px bevels shared by the Win98 desktop and Notepad.
namespace win98 {

// Raised edge: white top-left, black bottom-right, dark grey inner shadow.
void drawRaised(const GfxRenderer& r, int x, int y, int w, int h);
// Sunken edge: the raised edge mirrored.
void drawSunken(const GfxRenderer& r, int x, int y, int w, int h);
// Button with a raised edge. The face stays white so it stands out from
// dithered grey on e-ink.
void drawButton(const GfxRenderer& r, int x, int y, int w, int h);
// Small solid triangle pointing up or down, centred on (cx, cy).
void drawArrow(const GfxRenderer& r, int cx, int cy, bool up);

}  // namespace win98
