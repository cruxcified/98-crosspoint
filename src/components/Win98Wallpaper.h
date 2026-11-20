#pragma once

#include <cstddef>
#include <string>
#include <vector>

class GfxRenderer;

// Desktop wallpaper drawn from a BMP in the sleep folder.
namespace win98 {

constexpr char WALLPAPER_DIR[] = "/sleep";

// Wallpapers need PSRAM for the cached frame; without it every desktop
// redraw would re-read and re-dither the BMP.
bool wallpaperSupported();

// Identifies the wallpaper file as it is now (path, size, date, screen size).
// Opens the file, so call it outside the render path. Empty if unusable.
std::string wallpaperKey(const GfxRenderer& r, const char* path);

// Draws the wallpaper over the whole screen as a black-and-white dither,
// scaled to cover the screen. The first call for a key reads and dithers the
// BMP and keeps the frame in PSRAM; later calls only copy it. Returns false if
// it cannot be drawn.
bool drawWallpaper(const GfxRenderer& r, const char* path, const std::string& key);

// Frees the cached frame (no wallpaper set).
void releaseWallpaper();

// Sorted BMP file names in WALLPAPER_DIR whose full path fits in
// `maxPathLen` bytes (with the terminator), at most `limit` of them.
std::vector<std::string> listWallpapers(size_t limit, size_t maxPathLen);

}  // namespace win98
