#include "CoverPickerActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <OpenLibraryJsonParser.h>
#include <Txt.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/PluginHttp.h"

namespace fui = freeink::ui;

namespace {
constexpr char CACHE_DIR[] = "/.crosspoint";
constexpr char PICKER_DIR[] = "/.crosspoint/cover_picker";
// Three search hits are enough: the first is the book, its editions add the
// variety, and the others cover a first hit without covers.
constexpr char SEARCH_URL[] = "https://openlibrary.org/search.json?fields=key,cover_i&limit=3";
constexpr int GRID_GAP = 12;
constexpr int RING_GAP = 3;
constexpr int RING_WIDTH = 3;
constexpr int RING_ROOM = RING_GAP + RING_WIDTH;

// The grid shows "M" covers (~180 px wide); the chosen one is saved from "L"
// (~500 px), falling back to "M".
std::string coverUrl(const uint32_t id, const char size) {
  return "https://covers.openlibrary.org/b/id/" + std::to_string(id) + "-" + size + ".jpg";
}

std::string mediumPath(const int index) { return std::string(PICKER_DIR) + "/" + std::to_string(index) + ".jpg"; }
}  // namespace

CoverPickerActivity::CoverPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath)
    : UiListActivity("CoverPicker", renderer, mappedInput), bookPath(std::move(bookPath)) {}

void CoverPickerActivity::onEnter() {
  UiListActivity::onEnter();
  Epub(bookPath, CACHE_DIR).loadMetadata(title, author);
  if (title.empty()) title = FsHelpers::getFileNameWithoutExtension(bookPath);
  coverHeight = computeCoverHeight();

  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             finish();
                           } else {
                             onWifiConnected();
                           }
                         });
}

void CoverPickerActivity::onExit() {
  removePickerFiles();
  UiListActivity::onExit();
  // Same as the other WiFi screens: restart to drop the WiFi/TLS heap
  // fragmentation, landing back on the library.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToLibrary();
  }
}

const char* CoverPickerActivity::headerTitle() const { return tr(STR_FIND_COVER); }

int CoverPickerActivity::computeCoverHeight() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int areaWidth = safe.width - 2 * metrics.contentSidePadding;
  const int areaHeight = safe.height - metrics.topPadding - metrics.headerHeight - 2 * metrics.verticalSpacing;
  const int cellWidth = (areaWidth - (COLUMNS - 1) * GRID_GAP) / COLUMNS;
  return std::min(areaHeight / ROWS - 3 * RING_ROOM, (cellWidth - 2 * RING_ROOM) * 3 / 2) & ~7;
}

std::string CoverPickerActivity::thumbPath(const int index) const {
  return std::string(PICKER_DIR) + "/" + std::to_string(index) + ".bmp";
}

void CoverPickerActivity::removePickerFiles() const {
  if (Storage.exists(PICKER_DIR)) Storage.removeDir(PICKER_DIR);
}

void CoverPickerActivity::setStatus(const char* message) {
  {
    RenderLock lock(*this);
    state = State::Working;
    status = message;
  }
  requestUpdateAndWait();
}

void CoverPickerActivity::showMessage(const char* message) {
  {
    RenderLock lock(*this);
    state = State::Message;
    status = message;
  }
  requestUpdate();
}

void CoverPickerActivity::showFailure(const char* step, const int httpStatus) {
  LOG_ERR("COV", "Failed at %s (HTTP %d)", step, httpStatus);
  {
    RenderLock lock(*this);
    if (httpStatus > 0) {
      snprintf(statusBuf, sizeof(statusBuf), "%s (%s: HTTP %d)", tr(STR_COVER_FAILED), step, httpStatus);
    } else if (httpStatus == 0) {
      snprintf(statusBuf, sizeof(statusBuf), "%s (%s: no answer)", tr(STR_COVER_FAILED), step);
    } else {
      snprintf(statusBuf, sizeof(statusBuf), "%s (%s)", tr(STR_COVER_FAILED), step);
    }
    state = State::Message;
    status = statusBuf;
  }
  requestUpdate();
}

bool CoverPickerActivity::preventAutoSleep() {
  // The blocking work happens inside one loop() call, so the inactivity timer
  // last saw a key press before it: keep it reset briefly after the work ends.
  return state == State::Working || millis() - workEndedMs < 2000;
}

void CoverPickerActivity::onWifiConnected() {
  findCandidates();
  workEndedMs = millis();
  if (state == State::Picking) {
    nav.selected = 0;
    requestUpdate();
  }
}

bool CoverPickerActivity::search(const std::string& bookTitle, const std::string& bookAuthor) {
  // ~2.2 KB (mostly the JSON token buffer), freed when the search ends.
  searchFailStep = nullptr;
  searchFailStatus = 0;
  auto parser = makeUniqueNoThrow<OpenLibraryJsonParser>();
  if (!parser) {
    searchFailStep = "memory";
    searchFailStatus = -1;
    return false;
  }
  auto fetch = [&parser](const std::string& url) {
    parser->beginResponse();
    const bool ok = HttpDownloader::fetchUrl(url, [&parser](const uint8_t* data, const size_t len) {
      parser->feed(reinterpret_cast<const char*>(data), len);
      return !parser->hasError();
    });
    if (!ok) LOG_ERR("COV", "Request failed (HTTP %d): %s", HttpDownloader::lastStatus(), url.c_str());
    return ok;
  };

  std::string url = std::string(SEARCH_URL) + "&title=" + pluginhttp::urlEncodeQuery(bookTitle);
  if (!bookAuthor.empty()) url += "&author=" + pluginhttp::urlEncodeQuery(bookAuthor);
  bool ok = fetch(url);
  // Open Library is often slow or busy (no answer, 429, 5xx); one retry.
  const int firstStatus = HttpDownloader::lastStatus();
  if (!ok && !parser->hasError() && (firstStatus == 0 || firstStatus == 429 || firstStatus >= 500)) {
    delay(2000);
    ok = fetch(url);
  }
  if (!ok) {
    searchFailStep = parser->hasError() ? "search reply" : "search";
    searchFailStatus = HttpDownloader::lastStatus();
    return false;
  }
  const size_t searchCount = parser->coverCount();
  // Other editions of the top hit. Without them the search covers still count.
  if (parser->workKey()[0] != '\0') {
    fetch(std::string("https://openlibrary.org") + parser->workKey() + "/editions.json?limit=50");
  }

  // The top hit's cover first, then its editions, then the other hits.
  const size_t total = parser->coverCount();
  candidateCount = 0;
  auto take = [this, &parser](const size_t i) {
    if (candidateCount < MAX_CANDIDATES) candidates[candidateCount++] = parser->coverAt(i);
  };
  if (searchCount > 0) take(0);
  for (size_t i = searchCount; i < total; i++) take(i);
  for (size_t i = 1; i < searchCount; i++) take(i);
  LOG_DBG("COV", "Found %d covers (%u from search) for \"%s\"", candidateCount, static_cast<unsigned>(searchCount),
          bookTitle.c_str());
  return true;
}

void CoverPickerActivity::findCandidates() {
  setStatus(tr(STR_COVER_SEARCHING));
  removePickerFiles();
  if (WiFi.status() != WL_CONNECTED) {
    showFailure("WiFi", -1);
    return;
  }
  if (!Storage.mkdir(PICKER_DIR)) {
    showFailure("SD card", -1);
    return;
  }

  bool searched = search(title, author);
  // Author names in book metadata ("Tolkien, J. R. R.") do not always match.
  if (searched && candidateCount == 0 && !author.empty() && HttpDownloader::lastStatus() != 0) {
    searched = search(title, "");
  }
  if (!searched) {
    showFailure(searchFailStep ? searchFailStep : "search", searchFailStatus);
    return;
  }
  if (candidateCount == 0) {
    showMessage(tr(STR_COVER_NOT_FOUND));
    return;
  }

  // Download each candidate and make its grid thumb; drop the ones that fail.
  const int found = candidateCount;
  int kept = 0;
  bool anyDownloaded = false;
  int downloadStatus = 0;
  const std::string download = std::string(PICKER_DIR) + "/download.jpg";
  for (int i = 0; i < found; i++) {
    {
      RenderLock lock(*this);
      snprintf(statusBuf, sizeof(statusBuf), tr(STR_COVER_DOWNLOADING), i + 1, found);
    }
    setStatus(statusBuf);
    if (HttpDownloader::downloadToFile(coverUrl(candidates[i], 'M'), download) != HttpDownloader::OK) {
      downloadStatus = HttpDownloader::lastStatus();
      // No answer at all (WiFi lost, timeout): the rest would wait just as long.
      if (downloadStatus == 0) break;
      continue;
    }
    anyDownloaded = true;
    const std::string medium = mediumPath(kept);
    if (!Storage.rename(download.c_str(), medium.c_str())) continue;
    if (!Txt::convertCoverImageToBmp(medium, thumbPath(kept), coverHeight)) {
      Storage.remove(medium.c_str());
      continue;
    }
    candidates[kept++] = candidates[i];
  }

  if (kept == 0) {
    candidateCount = 0;
    // Downloaded but none decoded: the image, not the network.
    showFailure(anyDownloaded ? "image" : "cover download", anyDownloaded ? -1 : downloadStatus);
    return;
  }
  RenderLock lock(*this);
  candidateCount = kept;
  state = State::Picking;
  status = nullptr;
}

void CoverPickerActivity::activateIndex(const int index) {
  if (state != State::Picking || index < 0 || index >= candidateCount) return;
  app.clearTapFlash();
  saveChoice(index);
}

void CoverPickerActivity::saveChoice(const int index) {
  setStatus(tr(STR_COVER_SAVING));
  const Epub epub(bookPath, CACHE_DIR);

  // The large cover looks better full screen (sleep screen); the medium one
  // already decoded once, so it is the fallback.
  const std::string large = std::string(PICKER_DIR) + "/large.jpg";
  const std::string check = std::string(PICKER_DIR) + "/check.bmp";
  std::string source = mediumPath(index);
  if (WiFi.status() == WL_CONNECTED &&
      HttpDownloader::downloadToFile(coverUrl(candidates[index], 'L'), large) == HttpDownloader::OK &&
      Txt::convertCoverImageToBmp(large, check, coverHeight)) {
    source = large;
  }

  const std::string dest = epub.getCustomCoverPath();
  Storage.mkdir(FsHelpers::extractFolderPath(dest).c_str());
  if (!Storage.replaceFile(source.c_str(), dest.c_str())) {
    LOG_ERR("COV", "Could not save cover to %s", dest.c_str());
    // replaceFile removes the old cover first; drop images made from it.
    if (!Storage.exists(dest.c_str())) epub.clearCoverImages();
    workEndedMs = millis();
    showMessage(tr(STR_COVER_SAVE_FAILED));
    return;
  }
  epub.clearCoverImages();

  // The home screen only regenerates covers of recents that list a thumb.
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (book.path != bookPath) continue;
    const std::string bookTitle = book.title;
    const std::string bookAuthor = book.author;
    RECENT_BOOKS.updateBook(bookPath, bookTitle, bookAuthor, epub.getThumbBmpPath());
    break;
  }
  LOG_INF("COV", "Saved cover %u for %s", static_cast<unsigned>(candidates[index]), bookPath.c_str());
  finish();
}

void CoverPickerActivity::loop() {
  if (state == State::Picking) {
    UiListActivity::loop();
    return;
  }
  if (state != State::Message) return;
  int x = 0;
  int y = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void CoverPickerActivity::render(RenderLock&& lock) {
  if (state == State::Picking) {
    UiListActivity::render(std::move(lock));
    return;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FIND_COVER));
  if (status) {
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const Rect bounds{metrics.contentSidePadding, (pageHeight - lineHeight * 2) / 2,
                      pageWidth - metrics.contentSidePadding * 2, lineHeight * 3};
    UITheme::drawCenteredWrappedText(renderer, bounds, UI_10_FONT_ID, status, 3, true, EpdFontFamily::REGULAR,
                                     UITheme::TextVerticalAlignment::TOP);
  }
  if (state == State::Message) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

void CoverPickerActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int count = listCount();
  if (count == 0 || coverHeight < 24) return;

  const fui::Rect area{
      static_cast<int16_t>(safe.x + metrics.contentSidePadding),
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing),
      static_cast<int16_t>(safe.width - 2 * metrics.contentSidePadding),
      static_cast<int16_t>(safe.height - metrics.topPadding - metrics.headerHeight - 2 * metrics.verticalSpacing)};

  fui::CoverGridProps grid;
  grid.columns = COLUMNS;
  grid.gap = GRID_GAP;
  grid.rowGap = 0;
  grid.rowHeight = static_cast<int16_t>(area.height / ROWS);
  grid.coverSize = {static_cast<int16_t>(coverHeight * 2 / 3), static_cast<int16_t>(coverHeight)};
  grid.cellInset = fui::Insets{RING_ROOM, 0, 0, 0};
  grid.labelHeight = 0;
  grid.scrollIndicator = false;
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

  // One page: ListNav keeps the linear selection that buttons step through.
  fui::ListProps navProps;
  nav.syncToProps(fui::Rect{0, 0, 1, static_cast<int16_t>(MAX_CANDIDATES)}, 1, 0, count, navProps, 1);
  nav.top = 0;
  nav.drawnCount = count;
  nav.onListRendered(0, MAX_CANDIDATES, true);

  grid.count = static_cast<uint16_t>(count);
  grid.topIndex = 0;
  grid.selectedIndex = static_cast<int16_t>(navProps.selectedIndex);
  grid.action = ACTION_ROW;
  grid.inputMask = fui::InputTouch;
  grid.itemProvider = [](const uint16_t index, void*) { return fui::coverGridItem(nullptr, index); };
  grid.coverPainterUserData = this;
  grid.coverPainter = [](fui::DrawTarget& target, const fui::Rect rect, const fui::CoverGridItem&, const uint16_t index,
                         void* user) {
    auto* self = static_cast<CoverPickerActivity*>(user);
    HalFile file;
    bool drawn = false;
    if (Storage.openFileForRead("COV", self->thumbPath(index), file)) {
      Bitmap bmp(file);
      drawn = bmp.parseHeaders() == BmpReaderError::Ok &&
              GUI.drawCoverThumbFill(self->renderer, bmp, Rect{rect.x, rect.y, rect.width, rect.height});
      file.close();
    }
    if (!drawn) target.fill(rect, fui::Paint::dither(fui::Color::LightGray));
    target.stroke(rect, fui::Paint::solid(fui::Color::Black), 1, 0);
    return true;
  };
  fui::coverGrid(screen.frame(), area, grid);
}
