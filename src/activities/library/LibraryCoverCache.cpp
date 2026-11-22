#include "LibraryCoverCache.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include <cstring>

namespace fui = freeink::ui;

namespace {
// A typical 2:3 book cover.
constexpr int PLACEHOLDER_WIDTH = LibraryCoverCache::COVER_HEIGHT * 2 / 3;
// Below this largest free block, generation is skipped for this visit: an
// out-of-memory failure must not be recorded as "this book has no cover".
constexpr size_t MIN_GENERATE_HEAP = 48 * 1024;

// BitmapRef rows are packed: (width + 7) / 8 bytes each.
void setInk(uint8_t* bits, const int w, const int x, const int y) {
  bits[y * ((w + 7) / 8) + x / 8] |= 0x80 >> (x % 8);
}

// 1px black frame, so white covers still read as covers.
void drawFrame(uint8_t* bits, const int w, const int h) {
  for (int x = 0; x < w; x++) {
    setInk(bits, w, x, 0);
    setInk(bits, w, x, h - 1);
  }
  for (int y = 0; y < h; y++) {
    setInk(bits, w, 0, y);
    setInk(bits, w, w - 1, y);
  }
}
}  // namespace

bool LibraryCoverCache::ensureBits() {
  if (bits) return true;
  if (allocFailed) return false;
  bits = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(SLOTS) * SLOT_BYTES);
  if (!bits) {
    // Rows keep their plain icons; no retry every frame.
    LOG_ERR("LIB", "OOM: cover thumbnails");
    allocFailed = true;
    return false;
  }
  return true;
}

void LibraryCoverCache::release() {
  bits.reset();
  allocFailed = false;
  for (auto& slot : slots) {
    slot.path.clear();
    slot.path.shrink_to_fit();
    slot.state = State::Empty;
    slot.lastFrame = 0;
  }
}

void LibraryCoverCache::drawPlaceholder(const int slot, const fui::BitmapRef icon) {
  uint8_t* out = bitsFor(slot);
  memset(out, 0, SLOT_BYTES);
  drawFrame(out, PLACEHOLDER_WIDTH, COVER_HEIGHT);
  if (icon) {
    const fui::Rect area{0, 0, static_cast<int16_t>(PLACEHOLDER_WIDTH), static_cast<int16_t>(COVER_HEIGHT)};
    fui::forEachBitmapPixel(area, icon, fui::BitmapMode::Center, [&](const int16_t x, const int16_t y) {
      if (x >= 0 && x < PLACEHOLDER_WIDTH && y >= 0 && y < COVER_HEIGHT) setInk(out, PLACEHOLDER_WIDTH, x, y);
    });
  }
  slots[slot].width = PLACEHOLDER_WIDTH;
  slots[slot].height = COVER_HEIGHT;
}

fui::BitmapRef LibraryCoverCache::lookup(const std::string& path, const fui::BitmapRef icon) {
  if (path.empty() || !ensureBits()) return icon;
  int found = -1;
  int oldest = 0;
  for (int i = 0; i < SLOTS; i++) {
    if (slots[i].state != State::Empty && slots[i].path == path) {
      found = i;
      break;
    }
    if (slots[i].lastFrame < slots[oldest].lastFrame) oldest = i;
  }
  if (found < 0) {
    // Every slot belongs to a row of this frame: draw the plain icon rather
    // than steal one.
    if (slots[oldest].state != State::Empty && slots[oldest].lastFrame == frame) return icon;
    found = oldest;
    Slot& slot = slots[found];
    slot.path = path;
    slot.icon = icon;
    slot.state = State::Queued;
    drawPlaceholder(found, icon);
    // A cached thumb is a few hundred bytes: read it now so the page draws
    // once, with covers. Only missing thumbs wait for loadNext().
    std::string thumb;
    std::string marker;
    if (!thumbPaths(path, thumb, marker) || Storage.exists(marker.c_str())) {
      slot.state = State::NoCover;
    } else if (Storage.exists(thumb.c_str())) {
      if (decode(found, thumb)) {
        slot.state = State::Ready;
      } else {
        drawPlaceholder(found, icon);
        slot.state = State::NoCover;
      }
    }
  }
  Slot& slot = slots[found];
  slot.lastFrame = frame;
  return fui::BitmapRef{bitsFor(found), slot.width, slot.height, fui::BitmapFormat::BW1, false};
}

bool LibraryCoverCache::hasWork() const {
  if (!bits) return false;
  for (const auto& slot : slots) {
    if (slot.state == State::Queued && slot.lastFrame == frame) return true;
  }
  return false;
}

bool LibraryCoverCache::thumbPaths(const std::string& book, std::string& thumb, std::string& marker) {
  // Constructors only derive cache paths; parsing happens in generateThumb().
  if (FsHelpers::hasReflowableBookExtension(book)) {
    auto epub = makeUniqueNoThrow<Epub>(book, "/.crosspoint");
    if (!epub) return false;
    thumb = epub->getThumbBmpPath(COVER_HEIGHT);
    marker = epub->getCachePath() + "/thumb_" + std::to_string(COVER_HEIGHT) + ".none";
    return true;
  }
  if (FsHelpers::hasXtcExtension(book)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book, "/.crosspoint");
    if (!xtc) return false;
    thumb = xtc->getThumbBmpPath(COVER_HEIGHT);
    marker = xtc->getCachePath() + "/thumb_" + std::to_string(COVER_HEIGHT) + ".none";
    return true;
  }
  return false;
}

LibraryCoverCache::Generated LibraryCoverCache::generateThumb(const std::string& book) {
  if (HalMemory::getInternalHeap().largestBlockBytes < MIN_GENERATE_HEAP) return Generated::Retry;
  // One parser at a time, on the heap: they exceed the task stack budget.
  if (FsHelpers::hasReflowableBookExtension(book)) {
    auto epub = makeUniqueNoThrow<Epub>(book, "/.crosspoint");
    if (!epub) return Generated::Retry;
    if (epub->generateThumbBmpFromSource(COVER_HEIGHT)) return Generated::Done;
    // With enough heap, a failure here is the book: no cover, or one we
    // cannot read. The marker needs the cache folder.
    epub->setupCacheDir();
    return Generated::NoCover;
  }
  if (FsHelpers::hasXtcExtension(book)) {
    // The XTC cover is its first page, so a failure is memory or I/O.
    auto xtc = makeUniqueNoThrow<Xtc>(book, "/.crosspoint");
    if (xtc && xtc->load() && xtc->generateThumbBmp(COVER_HEIGHT)) return Generated::Done;
    return Generated::Retry;
  }
  return Generated::NoCover;
}

bool LibraryCoverCache::decode(const int slot, const std::string& thumbPath) {
  HalFile file;
  if (!Storage.openFileForRead("LIB", thumbPath, file)) return false;
  Bitmap bmp(file);
  if (bmp.parseHeaders() != BmpReaderError::Ok) return false;
  const int w = bmp.getWidth();
  const int h = bmp.getHeight();
  if (w < 3 || h < 3 || w > COVER_MAX_WIDTH || h > COVER_HEIGHT) {
    LOG_ERR("LIB", "unexpected thumb size %dx%d", w, h);
    return false;
  }
  // readNextRow emits 2 bits per pixel (0 = black .. 3 = white).
  uint8_t gray[(COVER_MAX_WIDTH + 3) / 4];
  uint8_t raw[COVER_MAX_WIDTH * 4];
  if (bmp.getRowBytes() > static_cast<int>(sizeof(raw))) return false;
  uint8_t* out = bitsFor(slot);
  memset(out, 0, SLOT_BYTES);
  for (int i = 0; i < h; i++) {
    if (bmp.readNextRow(gray, raw) != BmpReaderError::Ok) return false;
    const int y = bmp.isTopDown() ? i : h - 1 - i;
    for (int x = 0; x < w; x++) {
      const uint8_t level = (gray[x / 4] >> (6 - 2 * (x % 4))) & 0x03;
      if (level < 2) setInk(out, w, x, y);
    }
  }
  drawFrame(out, w, h);
  slots[slot].width = static_cast<uint16_t>(w);
  slots[slot].height = static_cast<uint16_t>(h);
  return true;
}

bool LibraryCoverCache::loadNext() {
  if (!bits) return false;
  int next = -1;
  for (int i = 0; i < SLOTS; i++) {
    if (slots[i].state == State::Queued && slots[i].lastFrame == frame) {
      next = i;
      break;
    }
  }
  if (next < 0) return false;
  Slot& slot = slots[next];
  // Placeholder stays for this visit unless a cover decodes.
  slot.state = State::NoCover;
  std::string thumb;
  std::string marker;
  if (!thumbPaths(slot.path, thumb, marker)) return false;
  if (!Storage.exists(thumb.c_str())) {
    const Generated result = generateThumb(slot.path);
    if (result == Generated::NoCover) {
      // Remember the miss on the card, so coverless books are not parsed
      // again on every visit. "Delete Book Cache" clears it with the rest.
      HalFile file;
      if (Storage.openFileForWrite("LIB", marker, file)) file.close();
    }
    if (result != Generated::Done) return false;
  }
  if (decode(next, thumb)) {
    slot.state = State::Ready;
    return true;
  }
  // A partial decode would leave a torn image; a later visit retries.
  LOG_ERR("LIB", "cannot decode %s", thumb.c_str());
  drawPlaceholder(next, slot.icon);
  return false;
}
