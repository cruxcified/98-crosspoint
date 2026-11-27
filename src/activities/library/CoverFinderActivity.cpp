#include "CoverFinderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <utility>

#include "CoverPickerActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long SPLASH_MIN_MS = 1500;
constexpr unsigned long COVER_PASS_MS = 1000;

constexpr int COLUMNS = 3;
constexpr int ROWS = 3;
constexpr int PAGE = COLUMNS * ROWS;
constexpr int16_t GRID_GAP = 8;
constexpr int16_t RING_GAP = 3;
constexpr int16_t RING_WIDTH = 3;
constexpr int16_t RING_ROOM = RING_GAP + RING_WIDTH;
constexpr int16_t LABEL_GAP = RING_ROOM + 3;

// Drawn in the desktop's monospace pixel font, one string per line.
constexpr const char* SPLASH_ART[] = {
    R"(  ____                           )",    R"( / ___|  ___  __   __  ___  _ __ )",
    R"(| |     / _ \ \ \ / / / _ \| '__|)",    R"(| |___ | (_) | \ V / |  __/| |   )",
    R"( \____| \___/   \_/   \___||_|   )",    R"(                                 )",
    R"( _____  _             _             )", R"(|  ___|(_) _ __    __| |  ___  _ __ )",
    R"(| |_   | || '_ \  / _` | / _ \| '__|)", R"(|  _|  | || | | || (_| ||  __/| |   )",
    R"(|_|    |_||_| |_| \__,_| \___||_|   )",
};
}  // namespace

CoverFinderActivity::CoverFinderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("CoverFinder", renderer, mappedInput) {}

void CoverFinderActivity::onEnter() {
  UiListActivity::onEnter();
  state = State::Splash;
  splashDrawn = false;
  requestUpdate(true);
}

void CoverFinderActivity::onExit() {
  index.close();
  covers.release();
  UiListActivity::onExit();
  // Same as the other WiFi screens: restart to drop the WiFi/TLS heap
  // fragmentation, landing on the desktop.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

const char* CoverFinderActivity::headerTitle() const { return tr(STR_COVER_FINDER); }

void CoverFinderActivity::render(RenderLock&& lock) {
  if (state == State::Splash) {
    renderSplash();
    return;
  }
  UiListActivity::render(std::move(lock));
}

void CoverFinderActivity::renderSplash() {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int artLine = renderer.getLineHeight(DESKTOP_FONT_ID);
  constexpr int artLines = sizeof(SPLASH_ART) / sizeof(SPLASH_ART[0]);
  const int textLine = renderer.getLineHeight(UI_10_FONT_ID);
  const int blockHeight = artLines * artLine + 3 * textLine;
  int y = std::max(0, (height - blockHeight) / 2 - artLine);

  renderer.clearScreen();
  // The lines of each word are the same width, so centering keeps the columns lined up.
  for (const char* line : SPLASH_ART) {
    renderer.drawCenteredText(DESKTOP_FONT_ID, y, line);
    y += artLine;
  }
  y += textLine;
  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_COVER_FINDER_TAGLINE));
  y += textLine * 2;
  renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_COVER_FINDER_SCANNING));
  renderer.drawRect(8, 8, width - 16, height - 16, true);
  renderer.displayBuffer();
  splashShownMs = millis();
  splashDrawn = true;
}

void CoverFinderActivity::scanLibrary() {
  // The rebuild walks the card, which the render task's SD fonts also read.
  RenderLock lock(*this);
  const bool readMetadata = SETTINGS.libraryUseMetadata != 0;
  if (library::isLibraryIndexDirty() || !index.open(library::libraryIndexPath()) ||
      index.header().metadataEnabled != readMetadata) {
    index.close();
    library::BuildStats stats;
    if (!library::buildLibraryIndex("/", stats, readMetadata) || !index.open(library::libraryIndexPath())) {
      LOG_ERR("CVF", "cannot open library index");
      scanFailed = true;
      return;
    }
  }

  const uint16_t total = index.bookCount();
  bookCount = 0;
  if (total == 0) return;
  // One slot per indexed book; the grid reads titles and paths through it.
  ordinals = makeUniqueNoThrow<uint16_t[]>(total);
  if (!ordinals) {
    LOG_ERR("CVF", "OOM: %u book slots", static_cast<unsigned>(total));
    scanFailed = true;
    return;
  }
  std::string name;
  for (uint16_t row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(library::SortOrder::TitleAsc, row);
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readName(record, name)) continue;
    // Find Cover saves through the Epub class, which reads these formats (not XTC).
    if (FsHelpers::hasReflowableBookExtension(name)) ordinals[bookCount++] = ordinal;
  }
  LOG_INF("CVF", "%d of %u books can take a cover", bookCount, static_cast<unsigned>(total));
}

void CoverFinderActivity::loop() {
  if (state == State::Splash) {
    // Scan once the splash is on screen, then keep it up for a moment.
    if (!splashDrawn) return;
    if (!index.isOpen() && !scanFailed) scanLibrary();
    if (millis() - splashShownMs < SPLASH_MIN_MS) return;
    {
      RenderLock lock(*this);
      state = State::Browse;
      nav.selected = 0;
    }
    requestUpdate(true);
    return;
  }

  UiListActivity::loop();
  // Also false once a tap has queued the picker (openPicker).
  if (!coverWorkPending) return;
  bool changed = false;
  {
    // Making a thumb parses the book; bounded so input is polled again soon.
    RenderLock lock(*this);
    const unsigned long start = millis();
    while (covers.hasWork() && millis() - start < COVER_PASS_MS) changed = covers.loadNext() || changed;
    coverWorkPending = covers.hasWork();
  }
  if (changed) requestUpdate();
}

bool CoverFinderActivity::bookAt(const int entry, std::string& title, std::string& path) {
  title.clear();
  path.clear();
  if (entry < 0 || entry >= bookCount || !index.isOpen()) return false;
  library::ClixRecord record{};
  if (!index.readRecord(ordinals[entry], record) || !index.readPath(record, path)) return false;
  if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
  return true;
}

void CoverFinderActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const fui::Rect area{
      static_cast<int16_t>(safe.x + metrics.contentSidePadding),
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing),
      static_cast<int16_t>(safe.width - 2 * metrics.contentSidePadding),
      static_cast<int16_t>(safe.height - metrics.topPadding - metrics.headerHeight - 2 * metrics.verticalSpacing)};

  if (bookCount == 0) {
    screen.target().text(area, scanFailed ? tr(STR_LIBRARY_REBUILD_FAILED) : tr(STR_COVER_FINDER_EMPTY),
                         screen.theme().bodyText);
    return;
  }

  fui::CoverGridProps grid;
  grid.columns = COLUMNS;
  grid.gap = GRID_GAP;
  grid.rowGap = 0;
  grid.titleText = screen.theme().smallText;
  grid.titleText.maxLines = 1;
  coverText = grid.titleText;
  coverText.align = fui::TextAlign::Center;
  coverText.maxLines = 5;
  const int16_t lineHeight = screen.target().lineHeight(grid.titleText.font);
  grid.rowHeight = static_cast<int16_t>(area.height / ROWS);
  const int scrollRoom = grid.scrollIndicatorWidth + grid.scrollIndicatorGap;
  const int cellWidth = (area.width - scrollRoom - (COLUMNS - 1) * GRID_GAP) / COLUMNS;
  // Rounded to a coarse step: the height names the thumb files on the card.
  const int coverHeight =
      std::min(grid.rowHeight - RING_ROOM - LABEL_GAP - lineHeight, (cellWidth - 2 * RING_ROOM) * 3 / 2) & ~7;
  if (coverHeight < 24) return;
  covers.setCoverHeight(coverHeight);
  grid.coverSize = {static_cast<int16_t>(coverHeight * 2 / 3), static_cast<int16_t>(coverHeight)};
  grid.cellInset = fui::Insets{RING_ROOM, 0, 0, 0};
  grid.labelGap = LABEL_GAP;
  grid.labelHeight = lineHeight;
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

  // ListNav keeps one linear selection; the viewport snaps to whole pages
  // (e-ink redraws the screen either way).
  fui::ListProps navProps;
  nav.syncToProps(fui::Rect{0, 0, 1, static_cast<int16_t>(PAGE)}, 1, 0, bookCount, navProps);
  const int selected = navProps.selectedIndex;
  const int top = selected / PAGE * PAGE;
  nav.top = top;
  nav.drawnCount = bookCount;
  nav.onListRendered(static_cast<uint16_t>(top), PAGE, true);

  pageTitles.resize(PAGE);
  pagePaths.resize(PAGE);
  pageTop = top;
  covers.beginFrame();
  for (int i = 0; i < PAGE; i++) {
    if (!bookAt(top + i, pageTitles[i], pagePaths[i]) && top + i < bookCount) {
      pageTitles[i] = tr(STR_LIBRARY_UNKNOWN_TITLE);
    }
  }

  grid.count = static_cast<uint16_t>(bookCount);
  grid.topIndex = static_cast<uint16_t>(top);
  grid.selectedIndex = static_cast<int16_t>(selected);
  grid.action = ACTION_ROW;
  grid.inputMask = fui::InputTouch;
  grid.itemProviderUserData = this;
  grid.itemProvider = [](const uint16_t index, void* user) {
    auto* self = static_cast<CoverFinderActivity*>(user);
    const size_t slot = static_cast<size_t>(index - self->pageTop);
    return fui::coverGridItem(slot < self->pageTitles.size() ? self->pageTitles[slot].c_str() : "", index);
  };
  grid.coverPainterUserData = this;
  grid.coverPainter = [](fui::DrawTarget& target, const fui::Rect rect, const fui::CoverGridItem&, const uint16_t index,
                         void* user) {
    static_cast<CoverFinderActivity*>(user)->paintCover(target, rect, index);
    return true;
  };
  fui::coverGrid(screen.frame(), area, grid);
  coverWorkPending = covers.hasWork();
}

// The book's cover, or its title on a blank one (Find Cover is for those).
void CoverFinderActivity::paintCover(fui::DrawTarget& target, const fui::Rect rect, const int index) {
  const size_t slot = static_cast<size_t>(index - pageTop);
  const bool inPage = slot < pagePaths.size();
  if (!inPage || !covers.paint(renderer, Rect{rect.x, rect.y, rect.width, rect.height}, pagePaths[slot])) {
    target.fill(rect, fui::Paint::solid(fui::Color::White));
    if (inPage && !pageTitles[slot].empty()) {
      const fui::Rect inner{static_cast<int16_t>(rect.x + 4), static_cast<int16_t>(rect.y + rect.height / 5),
                            static_cast<int16_t>(rect.width - 8), static_cast<int16_t>(rect.height * 3 / 5)};
      target.text(inner, pageTitles[slot].c_str(), coverText);
    }
  }
  target.stroke(rect, fui::Paint::solid(fui::Color::Black), 1, 0);
}

bool CoverFinderActivity::handleCustomInput() {
  if (swallowBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    swallowBackRelease = false;
    return true;
  }
  if (swallowConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    swallowConfirmRelease = false;
    return true;
  }
  return false;
}

void CoverFinderActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_FIND_COVER), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CoverFinderActivity::activateIndex(const int index) {
  if (state != State::Browse || index < 0 || index >= bookCount) return;
  app.clearTapFlash();
  openPicker(index);
}

void CoverFinderActivity::openPicker(const int entry) {
  std::string title;
  std::string path;
  {
    // The render task reads the same index file while it draws the grid.
    RenderLock lock(*this);
    if (!bookAt(entry, title, path)) return;
    // The picker reads and writes the card; WiFi stays on for the next book.
    index.close();
    coverWorkPending = false;
  }
  auto picker = makeUniqueNoThrow<CoverPickerActivity>(renderer, mappedInput, path, false);
  if (!picker) {
    LOG_ERR("CVF", "OOM: cover picker");
    {
      RenderLock lock(*this);
      if (!index.open(library::libraryIndexPath())) LOG_ERR("CVF", "cannot reopen library index");
    }
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(picker), [this](const ActivityResult&) {
    // The WiFi list cancels on press; its release must not close this screen.
    swallowBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
    swallowConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    {
      RenderLock lock(*this);
      if (!index.open(library::libraryIndexPath())) LOG_ERR("CVF", "cannot reopen library index");
      // A new cover may have been saved; make the slots load it again.
      covers.release();
    }
    requestUpdate(true);
  });
}
