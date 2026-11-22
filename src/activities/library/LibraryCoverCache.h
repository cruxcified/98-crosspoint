#pragma once

#include <cstdint>
#include <string>

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Cover thumbnails for the My Books cover grid.
//
// Thumbs are 1-bit BMPs in each book's cache folder (the same kind of file the
// home screen uses, at this grid's cover height). The render task paints
// thumbs that already exist straight from the card, so no image is kept in
// RAM. Generating a missing thumb parses the book, so it runs in the activity
// loop instead, and a page of new books fills in gradually rather than
// blocking the first frame.
//
// Memory: SLOTS small path records, bounded by the slot count, not the library.
class LibraryCoverCache {
 public:
  // More than one page of covers, so paging back does not look up again.
  static constexpr int SLOTS = 16;

  // Thumbs are made at this height. A new height forgets every slot.
  void setCoverHeight(int height);

  // Render task only (reads the card). Draws the book's cover centered in
  // `rect`; false when it has none yet, and the caller draws a placeholder.
  // Queues the book when its thumb must be generated. Call beginFrame() once
  // per render first; only books asked for in the latest frame are loaded.
  bool paint(const GfxRenderer& renderer, Rect rect, const std::string& book);
  void beginFrame() { ++frame; }

  // True when a book from the latest frame still needs loading.
  bool hasWork() const;
  // Generates one queued thumb (slow, parses the book on the SD card).
  // The caller holds the render lock. True when a cover became ready.
  bool loadNext();

  void release();

 private:
  enum class State : uint8_t { Empty, Queued, Ready, NoCover };
  // Retry: memory or I/O, try again on a later visit. NoCover: record it.
  enum class Generated : uint8_t { Done, Retry, NoCover };
  struct Slot {
    std::string book;
    std::string thumb;
    uint32_t lastFrame = 0;
    State state = State::Empty;
  };

  // The book's slot, claimed (and checked on the card) on first sight;
  // nullptr when every slot belongs to a cover of this frame.
  Slot* slotFor(const std::string& book);
  // Path of the thumb BMP and of the "no cover" marker; false for files that
  // cannot have a cover.
  bool thumbPaths(const std::string& book, std::string& thumb, std::string& marker) const;
  Generated generateThumb(const std::string& book) const;

  Slot slots[SLOTS];
  uint32_t frame = 0;
  int coverHeight = 0;
};
