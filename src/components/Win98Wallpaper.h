#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;

// Desktop wallpaper drawn from a BMP in the sleep folder.
namespace win98 {

constexpr char WALLPAPER_DIR[] = "/sleep";
// Most wallpapers listed. Settings shows them as an enum with uint8_t indexes.
constexpr size_t MAX_WALLPAPERS = 60;

// Full path of a file name from listWallpapers(), as stored in the settings.
std::string wallpaperPath(const std::string& name);

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

// Dithers the BMP at `path` like the wallpaper, scaled to cover a w x h box,
// into `bits` (1 = black, MSB first, (w + 7) / 8 bytes per row). Reads only
// the source rows the box samples. False if the file cannot be read.
bool ditherWallpaperThumb(const char* path, int w, int h, uint8_t* bits);

// Sorted BMP file names in WALLPAPER_DIR whose full path fits in
// `maxPathLen` bytes (with the terminator), at most `limit` of them.
std::vector<std::string> listWallpapers(size_t limit, size_t maxPathLen);

}  // namespace win98
