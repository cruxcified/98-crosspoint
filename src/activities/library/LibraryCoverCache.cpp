#include "LibraryCoverCache.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include "components/UITheme.h"

namespace {
// Below this largest free block, generation is skipped for this visit: an
// out-of-memory failure must not be recorded as "this book has no cover".
constexpr size_t MIN_GENERATE_HEAP = 48 * 1024;
}  // namespace

void LibraryCoverCache::setCoverHeight(const int height) {
  if (height == coverHeight) return;
  release();
  coverHeight = height;
}

void LibraryCoverCache::release() {
  for (auto& slot : slots) {
    slot.book.clear();
    slot.book.shrink_to_fit();
    slot.thumb.clear();
    slot.thumb.shrink_to_fit();
    slot.state = State::Empty;
    slot.lastFrame = 0;
  }
}

LibraryCoverCache::Slot* LibraryCoverCache::slotFor(const std::string& book) {
  int oldest = 0;
  for (int i = 0; i < SLOTS; i++) {
    if (slots[i].state != State::Empty && slots[i].book == book) return &slots[i];
    if (slots[i].lastFrame < slots[oldest].lastFrame) oldest = i;
  }
  // Every slot belongs to a cover of this frame: draw a placeholder rather
  // than steal one.
  Slot& slot = slots[oldest];
  if (slot.state != State::Empty && slot.lastFrame == frame) return nullptr;
  slot.book = book;
  std::string marker;
  if (!thumbPaths(book, slot.thumb, marker) || Storage.exists(marker.c_str())) {
    slot.state = State::NoCover;
  } else {
    slot.state = Storage.exists(slot.thumb.c_str()) ? State::Ready : State::Queued;
  }
  return &slot;
}

bool LibraryCoverCache::paint(const GfxRenderer& renderer, const Rect rect, const std::string& book) {
  if (book.empty() || coverHeight <= 0) return false;
  Slot* slot = slotFor(book);
  if (!slot) return false;
  slot->lastFrame = frame;
  if (slot->state != State::Ready) return false;
  HalFile file;
  if (!Storage.openFileForRead("LIB", slot->thumb, file)) return false;
  Bitmap bmp(file);
  bool drawn = false;
  if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0) {
    drawn = GUI.drawCoverThumbFill(renderer, bmp, rect);
  }
  file.close();
  // A thumb that cannot be read keeps its placeholder for this visit.
  if (!drawn) slot->state = State::NoCover;
  return drawn;
}

bool LibraryCoverCache::hasWork() const {
  for (const auto& slot : slots) {
    if (slot.state == State::Queued && slot.lastFrame == frame) return true;
  }
  return false;
}

bool LibraryCoverCache::thumbPaths(const std::string& book, std::string& thumb, std::string& marker) const {
  // Constructors only derive cache paths; parsing happens in generateThumb().
  // ".nocover" replaced ".none" when large JPEG covers became decodable, so
  // books marked before that are tried again.
  const std::string suffix = "/thumb_" + std::to_string(coverHeight) + ".nocover";
  if (FsHelpers::hasReflowableBookExtension(book)) {
    auto epub = makeUniqueNoThrow<Epub>(book, "/.crosspoint");
    if (!epub) return false;
    thumb = epub->getThumbBmpPath(coverHeight);
    marker = epub->getCachePath() + suffix;
    return true;
  }
  if (FsHelpers::hasXtcExtension(book)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book, "/.crosspoint");
    if (!xtc) return false;
    thumb = xtc->getThumbBmpPath(coverHeight);
    marker = xtc->getCachePath() + suffix;
    return true;
  }
  return false;
}

LibraryCoverCache::Generated LibraryCoverCache::generateThumb(const std::string& book) const {
  if (HalMemory::getInternalHeap().largestBlockBytes < MIN_GENERATE_HEAP) return Generated::Retry;
  // One parser at a time, on the heap: they exceed the task stack budget.
  if (FsHelpers::hasReflowableBookExtension(book)) {
    auto epub = makeUniqueNoThrow<Epub>(book, "/.crosspoint");
    if (!epub) return Generated::Retry;
    if (epub->generateThumbBmpFromSource(coverHeight)) return Generated::Done;
    // With enough heap, a failure here is the book: no cover, or one we
    // cannot read. The marker needs the cache folder.
    epub->setupCacheDir();
    return Generated::NoCover;
  }
  if (FsHelpers::hasXtcExtension(book)) {
    // The XTC cover is its first page, so a failure is memory or I/O.
    auto xtc = makeUniqueNoThrow<Xtc>(book, "/.crosspoint");
    if (xtc && xtc->load() && xtc->generateThumbBmp(coverHeight)) return Generated::Done;
    return Generated::Retry;
  }
  return Generated::NoCover;
}

bool LibraryCoverCache::loadNext() {
  Slot* next = nullptr;
  for (auto& slot : slots) {
    if (slot.state == State::Queued && slot.lastFrame == frame) {
      next = &slot;
      break;
    }
  }
  if (!next) return false;
  // Placeholder stays for this visit unless the thumb is made.
  next->state = State::NoCover;
  const Generated result = generateThumb(next->book);
  if (result == Generated::Done) {
    next->state = State::Ready;
    return true;
  }
  if (result == Generated::NoCover) {
    // Remember the miss on the card, so coverless books are not parsed
    // again on every visit. "Delete Book Cache" clears it with the rest.
    std::string thumb;
    std::string marker;
    HalFile file;
    if (thumbPaths(next->book, thumb, marker) && Storage.openFileForWrite("LIB", marker, file)) file.close();
  }
  return false;
}
