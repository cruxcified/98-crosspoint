#include "Win98Wallpaper.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

namespace win98 {

namespace {

// Source size limits: bound the row buffers (at most 20 KB for a 32-bit BMP)
// and keep the 16.16 scale arithmetic inside 32 bits.
constexpr int MAX_SOURCE_WIDTH = 4096;
constexpr int MAX_SOURCE_HEIGHT = 8192;

// The last finished wallpaper frame, in framebuffer format. One frame
// (48 KB on the X4 Pro) in PSRAM while a wallpaper is set, so desktop redraws
// and returns skip the SD read and dither. Touched only by the render task,
// except releaseWallpaper() from onEnter before the first render.
struct Cache {
  std::string key;
  HalMemory::PsramBuffer frame;
  size_t size = 0;
};
Cache cache;

void keepFrame(const GfxRenderer& r, const std::string& key) {
  const size_t size = r.getBufferSize();
  if (!cache.frame || cache.size != size) {
    cache.frame = HalMemory::allocatePsram(size);
    cache.size = cache.frame ? size : 0;
    if (!cache.frame) {
      cache.key.clear();
      return;
    }
  }
  memcpy(cache.frame.get(), r.getFrameBuffer(), size);
  cache.key = key;
}

// Scales the bitmap to cover the screen (nearest pixel, centred crop) and
// Floyd-Steinberg dithers it to black and white.
bool ditherToScreen(const GfxRenderer& r, const Bitmap& bmp) {
  const int sw = r.getScreenWidth();
  const int sh = r.getScreenHeight();
  const int bw = bmp.getWidth();
  const int bh = bmp.getHeight();
  if (bw <= 0 || bh <= 0 || bw > MAX_SOURCE_WIDTH || bh > MAX_SOURCE_HEIGHT) return false;

  auto rows = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(bw + bmp.getRowBytes()));
  auto errors = makeUniqueNoThrow<int16_t[]>(static_cast<size_t>(2 * (sw + 2)));
  if (!rows || !errors) {
    LOG_ERR("W98", "OOM: wallpaper row buffers");
    return false;
  }
  uint8_t* gray = rows.get();
  uint8_t* raw = rows.get() + bw;
  int16_t* cur = errors.get();
  int16_t* next = errors.get() + sw + 2;

  // Source pixels per screen pixel, 16.16 fixed point; cover = the smaller step.
  const uint32_t step = std::min((static_cast<uint32_t>(bw) << 16) / sw, (static_cast<uint32_t>(bh) << 16) / sh);
  const uint32_t offX = ((static_cast<uint32_t>(bw) << 16) - step * sw) / 2;
  const uint32_t offY = ((static_cast<uint32_t>(bh) << 16) - step * sh) / 2;

  r.clearScreen();
  int loadedRow = -1;
  for (int sy = 0; sy < sh; ++sy) {
    const int srcY = std::min(bh - 1, static_cast<int>((offY + step * sy) >> 16));
    if (srcY != loadedRow) {
      if (bmp.readRowGray(srcY, gray, raw) != BmpReaderError::Ok) return false;
      loadedRow = srcY;
    }
    std::fill(next, next + sw + 2, 0);
    for (int sx = 0; sx < sw; ++sx) {
      const int srcX = std::min(bw - 1, static_cast<int>((offX + step * sx) >> 16));
      const int value = gray[srcX] + cur[sx + 1];
      const bool black = value < 128;
      const int error = value - (black ? 0 : 255);
      if (black) r.drawPixel(sx, sy, true);
      cur[sx + 2] = static_cast<int16_t>(cur[sx + 2] + error * 7 / 16);
      next[sx] = static_cast<int16_t>(next[sx] + error * 3 / 16);
      next[sx + 1] = static_cast<int16_t>(next[sx + 1] + error * 5 / 16);
      next[sx + 2] = static_cast<int16_t>(next[sx + 2] + error / 16);
    }
    std::swap(cur, next);
  }
  return true;
}

}  // namespace

bool wallpaperSupported() { return HalMemory::getPsramHeap().largestBlockBytes > 0; }

std::string wallpaperKey(const GfxRenderer& r, const char* path) {
  if (path == nullptr || path[0] == '\0' || !wallpaperSupported()) return {};
  HalFile file;
  if (!Storage.openFileForRead("W98", path, file)) return {};
  // The desktop is always portrait, so the screen size covers orientation.
  std::string key = path;
  key += '|' + std::to_string(file.size()) + '|' + std::to_string(file.modificationTime()) + '|' +
         std::to_string(r.getScreenWidth()) + 'x' + std::to_string(r.getScreenHeight());
  return key;
}

bool drawWallpaper(const GfxRenderer& r, const char* path, const std::string& key) {
  if (key.empty()) return false;
  if (cache.frame && cache.key == key && cache.size == r.getBufferSize()) {
    memcpy(r.getFrameBuffer(), cache.frame.get(), cache.size);
    return true;
  }

  HalFile file;
  bool ok = Storage.openFileForRead("W98", path, file);
  if (ok) {
    Bitmap bmp(file);
    ok = bmp.parseHeaders() == BmpReaderError::Ok && ditherToScreen(r, bmp);
  }
  if (!ok) {
    LOG_ERR("W98", "Cannot draw wallpaper %s", path);
    r.clearScreen();
    return false;
  }
  keepFrame(r, key);
  return true;
}

void releaseWallpaper() {
  cache.frame.reset();
  cache.size = 0;
  cache.key.clear();
}

std::vector<std::string> listWallpapers(const size_t limit, const size_t maxPathLen) {
  std::vector<std::string> names;
  auto dir = Storage.open(WALLPAPER_DIR);
  if (!dir || !dir.isDirectory()) return names;
  names.reserve(limit);
  const size_t prefix = strlen(WALLPAPER_DIR) + 1;
  char name[128];
  for (auto entry = dir.openNextFile(); entry && names.size() < limit; entry = dir.openNextFile()) {
    if (entry.isDirectory()) continue;
    entry.getName(name, sizeof(name));
    if (name[0] == '\0' || name[0] == '.' || !FsHelpers::hasBmpExtension(name)) continue;
    if (prefix + strlen(name) >= maxPathLen) continue;  // would not fit in the setting
    names.emplace_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace win98
