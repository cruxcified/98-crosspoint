#include "WallpaperPickerActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "components/Win98Wallpaper.h"

namespace fui = freeink::ui;

namespace {
constexpr int GRID_GAP = 12;
constexpr int RING_GAP = 3;
constexpr int RING_WIDTH = 3;
constexpr int RING_ROOM = RING_GAP + RING_WIDTH;
}  // namespace

WallpaperPickerActivity::WallpaperPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("WallpaperPicker", renderer, mappedInput) {}

void WallpaperPickerActivity::onEnter() {
  names = win98::listWallpapers(win98::MAX_WALLPAPERS, sizeof(SETTINGS.desktopWallpaper));
  labels.clear();
  labels.reserve(names.size());
  int current = 0;
  for (size_t i = 0; i < names.size(); ++i) {
    labels.push_back(FsHelpers::getFileNameWithoutExtension(names[i]));
    if (win98::wallpaperPath(names[i]) == SETTINGS.desktopWallpaper) current = static_cast<int>(i) + 1;
  }
  std::fill(std::begin(slotEntry), std::end(slotEntry), -1);
  // Held so the first frame already starts on the wallpaper in use.
  RenderLock lock(*this);
  UiListActivity::onEnter();
  nav.requestSelection(current);
}

void WallpaperPickerActivity::onExit() {
  UiListActivity::onExit();
  slab.reset();
  thumbBytes = 0;
}

const char* WallpaperPickerActivity::headerTitle() const { return tr(STR_W98_WALLPAPER); }

void WallpaperPickerActivity::activateIndex(const int index) {
  if (index <= 0) {
    SETTINGS.desktopWallpaper[0] = '\0';
  } else if (index <= static_cast<int>(names.size())) {
    const std::string path = win98::wallpaperPath(names[index - 1]);
    strncpy(SETTINGS.desktopWallpaper, path.c_str(), sizeof(SETTINGS.desktopWallpaper) - 1);
    SETTINGS.desktopWallpaper[sizeof(SETTINGS.desktopWallpaper) - 1] = '\0';
  }
  SETTINGS.saveToFile();
  app.clearTapFlash();
  finish();
}

// Paints one thumb at (x, y), from the page cache when it holds it.
void WallpaperPickerActivity::paintThumb(const int index, const int x, const int y) {
  const size_t stride = (static_cast<size_t>(thumbW) + 7) / 8;
  const int slot = index % PAGE_ITEMS;
  uint8_t* bits = slab ? slab.get() + slot * thumbBytes : nullptr;
  // If the slab could not be allocated, decode into a short-lived buffer.
  std::unique_ptr<uint8_t[]> scratch;
  bool ok = bits && slotEntry[slot] == index;
  if (!ok) {
    if (!bits) {
      scratch = makeUniqueNoThrow<uint8_t[]>(thumbBytes);
      bits = scratch.get();
    }
    if (bits) ok = win98::ditherWallpaperThumb(win98::wallpaperPath(names[index - 1]).c_str(), thumbW, thumbH, bits);
    // Only successes are kept, so a failure from low memory is retried.
    if (slab) slotEntry[slot] = ok ? index : -1;
  }
  if (!ok) {
    renderer.fillRectDither(x, y, thumbW, thumbH, Color::DarkGray);
    return;
  }
  for (int row = 0; row < thumbH; ++row) {
    const uint8_t* line = bits + row * stride;
    for (int col = 0; col < thumbW; ++col) {
      if (line[col >> 3] & (0x80 >> (col & 7))) renderer.drawPixel(x + col, y + row, true);
    }
  }
}

void WallpaperPickerActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int count = listCount();
  const fui::Rect area{
      static_cast<int16_t>(safe.x + metrics.contentSidePadding),
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing),
      static_cast<int16_t>(safe.width - 2 * metrics.contentSidePadding),
      static_cast<int16_t>(safe.height - metrics.topPadding - metrics.headerHeight - 2 * metrics.verticalSpacing)};

  fui::CoverGridProps grid;
  grid.columns = COLUMNS;
  grid.gap = GRID_GAP;
  grid.rowGap = 0;
  grid.titleText = screen.theme().smallText;
  grid.titleText.maxLines = 1;
  const int lineHeight = screen.target().lineHeight(grid.titleText.font);
  grid.rowHeight = static_cast<int16_t>(area.height / ROWS);
  const int scrollRoom = grid.scrollIndicatorWidth + grid.scrollIndicatorGap;
  const int cellWidth = (area.width - scrollRoom - (COLUMNS - 1) * GRID_GAP) / COLUMNS;

  // Thumbs have the desktop's shape; the desktop is always portrait.
  const int deskW = std::min(renderer.getScreenWidth(), renderer.getScreenHeight());
  const int deskH = std::max(renderer.getScreenWidth(), renderer.getScreenHeight());
  const int h = std::min(grid.rowHeight - 2 * RING_ROOM - lineHeight, (cellWidth - 2 * RING_ROOM) * deskH / deskW);
  const int w = h * deskW / deskH;
  if (w < 16 || h < 16) return;
  if (w != thumbW || h != thumbH || !slab) {
    thumbW = w;
    thumbH = h;
    thumbBytes = (static_cast<size_t>(w) + 7) / 8 * h;
    slab.reset();
    slab = HalMemory::allocatePsram(thumbBytes * PAGE_ITEMS);
    if (!slab) LOG_ERR("W98", "OOM: wallpaper thumb cache");
    std::fill(std::begin(slotEntry), std::end(slotEntry), -1);
  }

  grid.coverSize = {static_cast<int16_t>(w), static_cast<int16_t>(h)};
  grid.cellInset = fui::Insets{RING_ROOM, 0, 0, 0};
  grid.labelGap = RING_ROOM;
  grid.labelHeight = static_cast<int16_t>(lineHeight);
  grid.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
  grid.selectedCoverFrameGap = RING_GAP;
  grid.selectedCoverFrameWidth = RING_WIDTH;
  grid.cellStyles.explicitlySet = true;
  grid.cellStyles.normal.background = fui::Paint::solid(fui::Color::White);
  grid.cellStyles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  grid.cellStyles.selected = grid.cellStyles.normal;
  grid.cellStyles.selected.border = fui::Paint::solid(fui::Color::Black);
  grid.cellStyles.focused = grid.cellStyles.selected;
  grid.cellStyles.active = grid.cellStyles.selected;

  // Whole pages, as in My Books: ListNav keeps the linear selection, and the
  // viewport snaps to the page that holds it.
  const int prevTop = nav.top;
  fui::ListProps navProps;
  nav.syncToProps(fui::Rect{0, 0, 1, static_cast<int16_t>(PAGE_ITEMS)}, 1, 0, count, navProps);
  const int selected = navProps.selectedIndex;
  const int totalRows = (count + COLUMNS - 1) / COLUMNS;
  const int lastTop = std::max(0, totalRows - ROWS) * COLUMNS;
  int top = navProps.topIndex;
  if (nav.followPending) {
    const bool onPage = selected >= prevTop && selected < prevTop + PAGE_ITEMS && prevTop % COLUMNS == 0;
    top = onPage ? prevTop
                 : fui::coverGridTopIndexFor(static_cast<uint16_t>(selected), static_cast<uint16_t>(count), COLUMNS,
                                             static_cast<uint16_t>(PAGE_ITEMS));
  } else {
    top = (top + COLUMNS - 1) / COLUMNS * COLUMNS;
  }
  top = std::min(top, lastTop);
  nav.top = top;
  nav.drawnCount = count;
  nav.onListRendered(static_cast<uint16_t>(top), PAGE_ITEMS, true);

  grid.count = static_cast<uint16_t>(count);
  grid.topIndex = static_cast<uint16_t>(top);
  grid.selectedIndex = static_cast<int16_t>(selected);
  grid.action = ACTION_ROW;
  grid.inputMask = fui::InputTouch;
  grid.itemProviderUserData = this;
  grid.itemProvider = [](const uint16_t index, void* user) {
    const auto* self = static_cast<const WallpaperPickerActivity*>(user);
    const char* label = index == 0 ? tr(STR_NONE_OPT) : self->labels[index - 1].c_str();
    return fui::coverGridItem(label, index);
  };
  grid.coverPainterUserData = this;
  grid.coverPainter = [](fui::DrawTarget& target, const fui::Rect rect, const fui::CoverGridItem&, const uint16_t index,
                         void* user) {
    auto* self = static_cast<WallpaperPickerActivity*>(user);
    if (index == 0) {
      // "None" is the plain grey desktop.
      target.fill(rect, fui::Paint::dither(fui::Color::LightGray));
    } else {
      target.fill(rect, fui::Paint::solid(fui::Color::White));
      self->paintThumb(index, rect.x, rect.y);
    }
    target.stroke(rect, fui::Paint::solid(fui::Color::Black), 1, 0);
    return true;
  };
  fui::coverGrid(screen.frame(), area, grid);
}
