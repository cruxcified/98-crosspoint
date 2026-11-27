#pragma once

#include <LibraryIndexFile.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "LibraryCoverCache.h"
#include "activities/UiListActivity.h"

// CoverFinder: a desktop program for Find Cover. It opens with a text-art
// splash while it reads the library, then shows every book that can take a
// cover (EPUB, TXT, Markdown) as a cover grid in title order. Nothing is
// downloaded until a book is tapped; that book alone then goes to
// CoverPickerActivity, which joins WiFi the first time.
//
// WiFi stays on between books and the device restarts to the desktop when the
// program closes, the same heap clean-up the other WiFi screens do.
//
// Memory: one uint16_t per indexed book (the grid's rows, at most 128 KB for
// a full 65535-book index, ~1 KB for 500 books) plus one page of titles and
// paths; covers are painted from thumbs on the card (LibraryCoverCache).
class CoverFinderActivity final : public UiListActivity {
 public:
  CoverFinderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  enum class State : uint8_t { Splash, Browse };

  int listCount() const override { return state == State::Browse ? bookCount : 0; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  bool handleCustomInput() override;
  void drawFooter() override;

  void renderSplash();
  // Opens the index (rebuilding it when needed) and keeps the books that can
  // take a cover. Blocking; runs once behind the splash.
  void scanLibrary();
  bool bookAt(int entry, std::string& title, std::string& path);
  void openPicker(int entry);
  void paintCover(freeink::ui::DrawTarget& target, freeink::ui::Rect rect, int index);

  State state = State::Splash;
  // Written by the render task once the splash is on screen.
  std::atomic<unsigned long> splashShownMs{0};
  std::atomic<bool> splashDrawn{false};

  library::LibraryIndexFile index;
  // Index ordinal of each grid entry, in title order.
  std::unique_ptr<uint16_t[]> ordinals;
  int bookCount = 0;
  bool scanFailed = false;

  // One page of the grid, rebuilt on each render.
  std::vector<std::string> pageTitles;
  std::vector<std::string> pagePaths;
  int pageTop = 0;
  freeink::ui::TextStyle coverText{};

  LibraryCoverCache covers;
  std::atomic<bool> coverWorkPending{false};

  // A button still held when a sub-screen returns: its release is not ours.
  bool swallowBackRelease = false;
  bool swallowConfirmRelease = false;
};
