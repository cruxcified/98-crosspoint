#pragma once

#include <HalMemory.h>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// Desktop wallpaper chooser, opened by holding the Win98 desktop. Shows
// "None" and the BMPs in the sleep folder as dithered thumbnails, the way the
// desktop will draw them; tapping one sets it and returns to the desktop.
//
// Memory: one PSRAM slab holds one page of 1-bit thumbs, so moving the
// selection repaints without reading the SD card again. A thumb that is not
// cached is decoded from the BMP rows it samples.
class WallpaperPickerActivity final : public UiListActivity {
 public:
  WallpaperPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;

 private:
  static constexpr int COLUMNS = 3;
  static constexpr int ROWS = 2;
  static constexpr int PAGE_ITEMS = COLUMNS * ROWS;

  int listCount() const override { return static_cast<int>(names.size()) + 1; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  void paintThumb(int index, int x, int y);

  // Entry 0 is "None"; entry i is names[i - 1].
  std::vector<std::string> names;   // file names in the sleep folder
  std::vector<std::string> labels;  // names without the extension
  int thumbW = 0;
  int thumbH = 0;
  size_t thumbBytes = 0;
  // PAGE_ITEMS thumbs; entry i is cached in slot i % PAGE_ITEMS. Null if the
  // PSRAM allocation failed.
  HalMemory::PsramBuffer slab;
  int slotEntry[PAGE_ITEMS] = {};  // entry drawn into each slot, -1 = empty
};
