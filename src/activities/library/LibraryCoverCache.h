#pragma once

#include <FreeInkUICore.h>

#include <cstdint>
#include <memory>
#include <string>

// Small cover thumbnails for the My Books rows.
//
// Thumbs are 1-bit BMPs in each book's cache folder (the same files the home
// screen uses, at this screen's own height). The render task decodes thumbs
// that already exist (small reads). Generating a missing thumb parses the
// book, so it runs in the activity loop instead, and a page of new books fills
// in gradually rather than blocking the first frame.
//
// Memory: one fallible block of SLOTS * SLOT_BYTES (8 KB), allocated on first
// use and released by release(). Bounded by the slot count, not the library.
class LibraryCoverCache {
 public:
  static constexpr int COVER_HEIGHT = 64;
  // Thumbs keep the cover's aspect, so a wide cover can reach the full slot.
  static constexpr int COVER_MAX_WIDTH = COVER_HEIGHT;
  // More than one page of rows, so paging back does not decode again.
  static constexpr int SLOTS = 16;

  // Bits for this book: its cover when ready, otherwise a blank cover with
  // `icon` in the middle. Queues the book when its thumb must be generated.
  // Render task only (reads the card). Call
  // beginFrame() once per render first; only books asked for in the latest
  // frame are loaded.
  freeink::ui::BitmapRef lookup(const std::string& path, freeink::ui::BitmapRef icon);
  void beginFrame() { ++frame; }

  // True when a book from the latest frame still needs loading.
  bool hasWork() const;
  // Loads one queued book (may generate its thumb: slow, touches the SD card).
  // The caller holds the render lock. True when the row's bits changed.
  bool loadNext();

  void release();

 private:
  enum class State : uint8_t { Empty, Queued, Ready, NoCover };
  // Retry: memory or I/O, try again on a later visit. NoCover: record it.
  enum class Generated : uint8_t { Done, Retry, NoCover };
  struct Slot {
    std::string path;
    freeink::ui::BitmapRef icon{};
    uint32_t lastFrame = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    State state = State::Empty;
  };
  static constexpr int SLOT_BYTES = ((COVER_MAX_WIDTH + 7) / 8) * COVER_HEIGHT;

  bool ensureBits();
  uint8_t* bitsFor(int slot) const { return bits.get() + slot * SLOT_BYTES; }
  void drawPlaceholder(int slot, freeink::ui::BitmapRef icon);
  // Path of the thumb BMP and of the "no cover" marker; false for files that
  // cannot have a cover.
  static bool thumbPaths(const std::string& book, std::string& thumb, std::string& marker);
  static Generated generateThumb(const std::string& book);
  bool decode(int slot, const std::string& thumbPath);

  std::unique_ptr<uint8_t[]> bits;
  bool allocFailed = false;
  Slot slots[SLOTS];
  uint32_t frame = 0;
};
