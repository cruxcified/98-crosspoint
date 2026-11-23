#include "LibraryListActivity.h"

#include <BoardConfig.h>
#include <FreeInkUIIcon.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/Win98Chrome.h"
#include "components/icons/headerIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/search32.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr int STATUS_BAR_GAP = 6;

// Side clearance for the footer text in the panel's bottom corners.
int bottomCornerClearance() {
  const auto& board = BoardConfig::ACTIVE;
  return board.bottomCornerRadius != 0 ? board.bottomCornerRadius : board.cornerRadius;
}

constexpr unsigned long LONG_PRESS_MS = 1000;
constexpr unsigned long COVER_PASS_MS = 1000;

constexpr int RECENT_TAB = 0;
constexpr int TITLE_TAB = 1;
constexpr int AUTHOR_TAB = 2;
// Recently added: arrival order without the opened books pinned on top.
constexpr int NEW_TAB = 3;
// One shelf per card folder; opens as the list of shelves.
constexpr int SHELF_TAB = 4;
constexpr int TAB_SLOTS = SHELF_TAB + 1;
// Folders past this share the "no shelf" bucket; keeps the name list small.
constexpr size_t MAX_SHELVES = 64;

constexpr bool isDescending(const library::SortOrder order) {
  return order == library::SortOrder::RecentDesc || order == library::SortOrder::TitleDesc ||
         order == library::SortOrder::AuthorDesc;
}

constexpr bool isRecentSort(const library::SortOrder order) {
  return order == library::SortOrder::RecentAsc || order == library::SortOrder::RecentDesc;
}

constexpr bool isAuthorSort(const library::SortOrder order) {
  return order == library::SortOrder::AuthorAsc || order == library::SortOrder::AuthorDesc;
}

constexpr library::SortOrder orderForTab(const int tab, const uint8_t descendingTabs) {
  const bool descending = (descendingTabs & (1u << tab)) != 0;
  if (tab == TITLE_TAB) return descending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  if (tab == AUTHOR_TAB) return descending ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc;
  if (tab == SHELF_TAB) return library::SortOrder::TitleAsc;
  return descending ? library::SortOrder::RecentDesc : library::SortOrder::RecentAsc;
}

const char* tabLabelFor(const int tab) {
  if (tab == TITLE_TAB) return tr(STR_LIBRARY_TAB_TITLE);
  if (tab == AUTHOR_TAB) return tr(STR_LIBRARY_TAB_AUTHOR);
  if (tab == NEW_TAB) return tr(STR_LIBRARY_TAB_NEW);
  if (tab == SHELF_TAB) return tr(STR_LIBRARY_TAB_SHELF);
  return tr(STR_LIBRARY_TAB_RECENT);
}

// Shelf label for a book: the name of the folder holding it, empty at the root.
void shelfNameFor(const std::string& path, std::string& out) {
  out.clear();
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) return;
  const size_t start = path.rfind('/', slash - 1);
  out.assign(path, start == std::string::npos ? 0 : start + 1, slash - (start == std::string::npos ? 0 : start + 1));
}

}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("Library", renderer, mappedInput, true) {
  win98Tabs = true;
}

void LibraryListActivity::onEnter() {
  // One lock across the base lifecycle AND the data phase: the base onEnter
  // schedules a paint, and the render task must not read the index or the
  // filter before they are in place. The rebuild also needs the lock: the
  // render task's SD-loaded fonts read glyph data at draw time, and the walk
  // needs the card to itself.
  RenderLock lock(*this);
  UiTabListActivity::onEnter();
  app.on(ACTION_SEARCH, &LibraryListActivity::searchActionTrampoline, this);
  app.on(ACTION_REBUILD, &LibraryListActivity::rebuildActionTrampoline, this);
  app.on(ACTION_BACK, &LibraryListActivity::backActionTrampoline, this);

  // Recent is backed by the resident store. Prune before opening the index so
  // its persistence write never overlaps the long-lived index reader.
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();

  // Rebuild when the index is missing, invalid, or was built with the other
  // metadata mode. Otherwise entering the screen stays instant.
  const bool readMetadata = SETTINGS.libraryUseMetadata != 0;
  const bool rebuildNeeded = library::isLibraryIndexDirty() || !index.open(library::libraryIndexPath()) ||
                             index.header().metadataEnabled != readMetadata;
  if (rebuildNeeded) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    rebuildIndex();
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  }
  degraded = index.isOpen() && index.ranksDegraded();
  if (index.isOpen() && index.dedupDegraded()) {
    LOG_ERR("LIB", "index was built without duplicate detection");
  }
  resolvePinned();

  // Entered while Confirm was still held (typical when launched from the home
  // menu): ignore its release, or we would open whatever sits at row 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate(true);
}

void LibraryListActivity::onExit() {
  index.close();
  covers.release();
  Activity::onExit();
}

bool LibraryListActivity::rebuildIndex() {
  library::BuildStats stats;
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
  if (!ok) {
    LOG_ERR("LIB", "index build failed");
    return false;
  }
  LOG_INF("LIB", "reconciled: %u unchanged, %u added, %u renamed, %u removed, %u enriched (%u dup, %u unreadable)",
          static_cast<unsigned>(stats.unchanged), static_cast<unsigned>(stats.added),
          static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
          static_cast<unsigned>(stats.enriched), static_cast<unsigned>(stats.duplicatesDropped),
          static_cast<unsigned>(stats.unreadableSkipped));
  if (stats.dedupDegraded) LOG_ERR("LIB", "rebuild completed without duplicate detection");
  return true;
}

void LibraryListActivity::swallowHeldReleases() {
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
}

int LibraryListActivity::selectedEntry() const {
  const int entry = ringPos() - 1;
  return entry < 0 ? 0 : entry;
}

// The pinned overlay applies only to the shelf that reads as "what am I up
// to": the unfiltered Recent sort, newest first. A search result is a flat
// list the reader narrowed down on purpose, and the ascending toggle asks for
// oldest-first, which pinned fresh reads would contradict.
int LibraryListActivity::pinnedCount() const {
  if (activeTabIndex != RECENT_TAB || !query.empty() || !isDescending(sortOrder)) return 0;
  return pinnedTotal;
}

void LibraryListActivity::resolvePinned() {
  const auto& books = RECENT_BOOKS.getBooks();
  pinnedTotal = static_cast<uint8_t>(std::min<size_t>(books.size(), RecentBooksStore::MAX_RECENT_BOOKS));
  for (int i = 0; i < pinnedTotal; i++) pinnedAscRows[i] = 0xFFFF;
  if (pinnedTotal > 0 && index.isOpen()) {
    library::BookIdentity identities[RecentBooksStore::MAX_RECENT_BOOKS];
    for (int i = 0; i < pinnedTotal; i++) {
      const std::string& path = books[static_cast<size_t>(i)].path;
      identities[i].pathHash = library::clixPathHash(path.data(), path.size());
      // Size is only a lookup prefilter; 0 (stat failed, e.g. the index handle
      // is the card's one open reader) falls back to hash-only matching.
      identities[i].fileSize = 0;
      HalFile file;
      if (Storage.openFileForRead("LIB", path.c_str(), file)) {
        identities[i].fileSize = static_cast<uint32_t>(file.fileSize());
      }
    }
    if (!index.recentRowsFor(identities, pinnedTotal, pinnedAscRows)) {
      // Without the match the overlay would duplicate every pinned book that is
      // also in the index; better to drop the pins than to show doubles.
      LOG_ERR("LIB", "recent-book lookup failed; overlay disabled");
      pinnedTotal = 0;
    }
  }
  refreshOverlap();
}

void LibraryListActivity::refreshOverlap() {
  overlapCount = 0;
  const int total = static_cast<int>(index.bookCount());
  for (int i = 0; i < pinnedTotal; i++) {
    if (pinnedAscRows[i] == 0xFFFF || pinnedAscRows[i] >= total) continue;
    const uint16_t row =
        isDescending(sortOrder) ? static_cast<uint16_t>(total - 1 - pinnedAscRows[i]) : pinnedAscRows[i];
    overlapRows[overlapCount++] = row;
  }
  std::sort(overlapRows, overlapRows + overlapCount);
}

void LibraryListActivity::openSelectedBook() {
  std::string path;
  if (selectedEntry() < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (selectedEntry() >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(selectedEntry())].path;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(selectedEntry())));
    if (ordinal == 0xFFFF) return;

    library::ClixRecord record{};
    if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
      LOG_ERR("LIB", "cannot resolve path for row %d", selectedEntry());
      return;
    }
  }
  openBookByPath(path);
}

// Shared by row activation and the options menu: the reader screen this opens
// has its own surfaces; a lingering tap flash would gray an unrelated element
// there. The index handle is released first — on hardware only one reader can
// hold a file open at a time, and the reader is about to open files of its own.
void LibraryListActivity::openBookByPath(const std::string& path) {
  app.clearTapFlash();
  index.close();
  onSelectBook(path);
}

void LibraryListActivity::activateIndex(const int index) {
  if (groupsCollapsed) {
    expandGroup(index);
  } else {
    openSelectedBook();
  }
}

// Row long-press prompts delete wherever grouping does not own the gesture:
// an active search is already a flat list the reader narrowed down on purpose
// ("find it, hold it, delete it"). Unfiltered Title/Author lists keep
// collapse-to-groups. The Recent tab always opens the row options menu.
bool LibraryListActivity::deleteEligible() const { return !groupsCollapsed && (!query.empty() || !groupable()); }

bool LibraryListActivity::filterActive() const { return !query.empty() || shelfMode; }

void LibraryListActivity::onRowLongPress(const int index) {
  if (isRecentSort(sortOrder)) {
    showRecentBookOptions(index);
  } else if (deleteEligible()) {
    promptDeleteBook(index);
  } else if (!groupsCollapsed && groupable()) {
    collapseGroups(index);
  } else {
    activateIndex(index);
  }
}

// Recent-shelf long-press menu (button hold and touch long-press). The first
// rows may come from RecentBooksStore; the rest are index rows sorted by
// modification time. Only store rows can be removed from recents.
void LibraryListActivity::showRecentBookOptions(const int entry) {
  if (entry < 0 || entry >= listCount()) return;

  std::string path;
  std::string title;
  const bool isStoreRow = entry < pinnedCount();
  if (isStoreRow) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry >= static_cast<int>(books.size())) return;
    path = books[static_cast<size_t>(entry)].path;
    title = books[static_cast<size_t>(entry)].title;
  } else {
    if (!index.isOpen()) return;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    std::string author;
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path) ||
        !rowTextFor(entry, title, author)) {
      LOG_ERR("LIB", "cannot resolve Recent row %d", entry);
      return;
    }
  }

  const char* STORE_OPTIONS[] = {tr(STR_OPEN), tr(STR_REMOVE_FROM_RECENTS), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  const char* INDEX_OPTIONS[] = {tr(STR_OPEN), tr(STR_DELETE), tr(STR_LIBRARY_REBUILD)};
  app.clearTapFlash();
  optionPopup.show(tr(STR_LIBRARY), title.c_str(), isStoreRow ? STORE_OPTIONS : INDEX_OPTIONS, isStoreRow ? 4 : 3, 0,
                   [this, path, title, isStoreRow](const int choice) {
                     swallowHeldReleases();
                     switch (choice) {
                       case 0:
                         openBookByPath(path);
                         break;
                       case 1:
                         if (isStoreRow) {
                           promptRemoveRecentBook(path, title);
                         } else {
                           promptDeleteBookByPath(path, title);
                         }
                         break;
                       case 2:
                         if (isStoreRow)
                           promptDeleteBookByPath(path, title);
                         else
                           promptRebuildIndex();
                         break;
                       case 3:
                         if (isStoreRow) promptRebuildIndex();
                         break;
                       default:
                         break;
                     }
                   });
  requestUpdate();
}

// Manual index refresh, same card discipline as the onEnter rebuild: the walk
// wants the card to itself, and the render task must not read the index (or
// the filter) around it.
void LibraryListActivity::promptRebuildIndex() {
  RenderLock lock(*this);
  GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
  index.close();
  rebuildIndex();
  if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot open library index");
  resetAfterRebuild();
  requestUpdate(true);
}

void LibraryListActivity::resetAfterRebuild() {
  // A delete inside a shelf comes back to that shelf. Folder ids change on a
  // rebuild, so match the folder path.
  const bool hadOpenShelf = openShelf >= 0;
  const std::string openFolder = hadOpenShelf ? shelfFolders[openShelf] : std::string();
  // Sort positions, group starts, and pinned rows all point into the old order.
  applyFilter();
  resolvePinned();
  if (hadOpenShelf && shelfMode) {
    for (int shelf = 0; shelf < groupCount; shelf++) {
      if (shelfFolders[shelf] != openFolder) continue;
      openShelf = shelf;
      groupsCollapsed = false;
      break;
    }
    // The shelf emptied out: back on the list, at its top.
    if (openShelf < 0) activeNav().reset(groupCount > 0 ? 1 : 0);
  }
  auto& nav = activeNav();
  const int count = listCount();
  if (count == 0) {
    nav.selected = 0;
  } else if (nav.selected > count) {
    nav.selected = count;
  }
  nav.followOnBuild = true;
}

void LibraryListActivity::promptRemoveRecentBook(const std::string& path, const std::string& title) {
  const bool reopenIndex = index.isOpen();
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: recent removal confirmation");
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path, reopenIndex](const ActivityResult& result) {
    swallowHeldReleases();
    if (reopenIndex && !index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    if (!result.isCancelled && RECENT_BOOKS.removeByPath(path)) {
      resolvePinned();
      closeRouting();
      auto& nav = activeNav();
      const int count = listCount();
      if (count == 0) {
        nav.selected = 0;
      } else if (nav.selected > count) {
        nav.selected = count;
      }
      nav.followOnBuild = true;
    }
  });
}

void LibraryListActivity::promptDeleteBook(const int entry) {
  if (!index.isOpen() || entry < 0 || entry >= bookRowCount()) return;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  if (ordinal == 0xFFFF) return;

  std::string path;
  library::ClixRecord record{};
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", entry);
    return;
  }
  std::string title;
  std::string author;
  rowTextFor(entry, title, author);
  promptDeleteBookByPath(path, title);
}

void LibraryListActivity::promptDeleteBookByPath(const std::string& path, const std::string& title) {
  // The dialog and the delete both want the card; reopen when we resume.
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_ITEM_CONFIRM), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: delete confirmation");
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }

  startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
    swallowHeldReleases();
    {
      // Same lock rationale as onEnter: the walk wants the card to itself, and
      // the render task must not read the index (or the filter) around the
      // rebuild.
      RenderLock lock(*this);
      if (!result.isCancelled) {
        LOG_DBG("LIB", "deleting %s", path.c_str());
        if (!removeBookFile(path)) LOG_ERR("LIB", "cannot delete %s", path.c_str());
        if (RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
        GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
        rebuildIndex();
      }
      if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
      if (!result.isCancelled) {
        resetAfterRebuild();
      }
    }
    if (!result.isCancelled) {
      closeRouting();
      requestUpdate(true);
    }
  });
}

void LibraryListActivity::openSearch() {
  app.clearTapFlash();
  // No key filtering here on purpose. Greying out the letters that lead nowhere
  // was built, tested on device and removed: a letter you can see but cannot
  // reach reads as a broken keyboard, and the eye keeps returning to it.
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query, 48,
                                                           InputType::Text);
  if (!keyboard) {
    LOG_ERR("LIB", "OOM: search keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    swallowHeldReleases();
    if (result.isCancelled) return;
    RenderLock lock(*this);
    query = std::get<KeyboardResult>(result.data).text;
    applyFilter();
    auto& nav = activeNav();
    if (!query.empty() && filteredCount == 0 && !degraded) {
      // Up from the tab bar reopens Search even with no results.
      nav.selected = 0;
    } else {
      // A non-empty result belongs to the list: land on
      // its first surviving row, not on the strip.
      nav.selected = 1;
    }
    nav.top = 0;
    requestUpdate();
  });
}

void LibraryListActivity::stepTab(const int direction) {
  const int next = (activeTab() + (direction > 0 ? 1 : TAB_SLOTS - 1)) % TAB_SLOTS;
  selectTab(next, false);
}

void LibraryListActivity::onTabAction(const int index) {
  app.clearTapFlash();
  selectTab(index, true);
}

void LibraryListActivity::selectTab(const int index, const bool toggleIfActive) {
#if FREEINK_CAP_TOUCH
  closeSwipeDelete();
#endif
  if (index < 0 || index >= TAB_SLOTS) return;
  // The Shelf tab is always A-Z, so re-selecting it changes nothing.
  if (toggleIfActive && index == activeTab() && index == SHELF_TAB) return;
  if (toggleIfActive && index == activeTab()) descendingTabs ^= static_cast<uint8_t>(1u << index);
  // The render task reads the filter and shelf arrays this replaces.
  RenderLock lock(*this);
  sortOrder = orderForTab(index, descendingTabs);
  activeTabIndex = index;
  // The filter and the overlap rows hold positions in the old order, so they
  // must be rebuilt.
  applyFilter();
  refreshOverlap();
  // Tab changes happen only while the bar owns focus. A tab's remembered row
  // must not pull focus back into the list after the switch.
  auto& nav = activeNav();
  nav.selected = 0;
  nav.top = 0;
  requestUpdate();
}

void LibraryListActivity::toggleSortDirection() { selectTab(activeTab(), true); }

int LibraryListActivity::tabCount() const { return TAB_SLOTS; }

int LibraryListActivity::activeTab() const { return activeTabIndex; }

const char* LibraryListActivity::tabLabel(const int index) const { return tabLabelFor(index); }

fui::TabIndicator LibraryListActivity::tabIndicator(const int index) const {
  // The Shelf tab has no sort arrow: shelves are always A-Z.
  if (index != activeTab() || index == SHELF_TAB) return fui::TabIndicator::None;
  return isDescending(sortOrder) ? fui::TabIndicator::Down : fui::TabIndicator::Up;
}

int LibraryListActivity::bookRowCount() const {
  if (openShelf >= 0) return shelfEnd(openShelf) - groupStarts[openShelf];
  if (filterActive()) return static_cast<int>(filteredCount);
  // Pinned books already in the index are skipped below the pins, not doubled;
  // pinned books the index missed still show, so the difference stays split.
  const int pinned = pinnedCount();
  return static_cast<int>(index.bookCount()) + (pinned > 0 ? pinned - overlapCount : 0);
}

int LibraryListActivity::listCount() const { return groupsCollapsed ? static_cast<int>(groupCount) : bookRowCount(); }

// Entry position on screen to row position in the sort order. Identity while
// unfiltered and unpinned, so the shelf costs nothing when nothing is typed.
// With pins active, entries below pinnedCount() belong to the store and must
// not reach this; the rest walk past the pinned books' own sort rows.
int LibraryListActivity::rowFor(const int entry) const {
  if (filterActive()) {
    const int position = entry + (openShelf >= 0 ? groupStarts[openShelf] : 0);
    if (position < 0 || position >= static_cast<int>(filteredCount) || !filtered) return 0;
    return filtered[position];
  }
  const int pinned = pinnedCount();
  if (pinned == 0) return entry;
  int row = entry - pinned;
  for (int i = 0; i < overlapCount; i++) {
    if (overlapRows[i] <= row) row++;
  }
  return row;
}

// An open shelf is already one group; its rows are flat, so hold deletes.
bool LibraryListActivity::groupable() const {
  return !degraded && !isRecentSort(sortOrder) && openShelf < 0 && bookRowCount() > 0;
}

int LibraryListActivity::shelfEnd(const int shelf) const {
  return shelf + 1 < groupCount ? groupStarts[shelf + 1] : static_cast<int>(filteredCount);
}

uint32_t LibraryListActivity::titleInitialFor(const int entry) {
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return 0;
  return library::foldedGroupInitial(std::string_view(record.fold, record.foldLen));
}

bool LibraryListActivity::buildGroupStarts() {
  const int count = bookRowCount();
  if (count <= 0) return false;
  if (groupCapacity < count) {
    auto starts = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(count));
    if (!starts) {
      LOG_ERR("LIB", "cannot allocate %u-byte group map", static_cast<unsigned>(count * sizeof(uint16_t)));
      return false;
    }
    groupStarts = std::move(starts);
    groupCapacity = static_cast<uint16_t>(count);
  }

  groupCount = 0;
  uint32_t previousInitial = 0;
  std::string previousAuthor;
  std::string title;
  std::string author;
  previousAuthor.reserve(128);
  title.reserve(128);
  author.reserve(128);
  for (int entry = 0; entry < count; entry++) {
    bool startsGroup = entry == 0;
    if (isAuthorSort(sortOrder)) {
      rowTextFor(entry, title, author);
      startsGroup = startsGroup || author != previousAuthor;
      previousAuthor = author;
    } else {
      const uint32_t initial = titleInitialFor(entry);
      startsGroup = startsGroup || initial != previousInitial;
      previousInitial = initial;
    }
    if (startsGroup) groupStarts[groupCount++] = static_cast<uint16_t>(entry);
  }
  LOG_DBG("LIB", "group map: %u groups, %u bytes", static_cast<unsigned>(groupCount),
          static_cast<unsigned>(groupCapacity * sizeof(uint16_t)));
  return groupCount > 0;
}

int LibraryListActivity::groupForBook(const int bookEntry) const {
  int group = 0;
  while (group + 1 < groupCount && groupStarts[group + 1] <= bookEntry) group++;
  return group;
}

bool LibraryListActivity::collapseGroups(const int bookEntry) {
  if (!groupable() || !buildGroupStarts()) return false;
  expandedNav = activeNav();
  groupsCollapsed = true;
  auto& nav = activeNav();
  nav.reset(groupForBook(bookEntry) + 1);
  requestUpdate();
  return true;
}

void LibraryListActivity::expandGroup(const int groupEntry) {
  if (!groupsCollapsed || groupEntry < 0 || groupEntry >= groupCount) return;
  if (shelfMode) {
    // A shelf opens on its own, titled with its name; Back returns here. The
    // tap flash would otherwise land on whichever book now sits under it.
    app.clearTapFlash();
    RenderLock lock(*this);
    openShelf = groupEntry;
    groupsCollapsed = false;
    activeNav().reset(1);
    requestUpdate();
    return;
  }
  const int bookEntry = groupStarts[groupEntry];
  groupsCollapsed = false;
  activeNav() = expandedNav;
  auto& nav = activeNav();
  nav.selected = bookEntry + 1;
  nav.top = bookEntry;
  nav.followOnBuild = true;
  requestUpdate();
}

void LibraryListActivity::closeShelf() {
  const int shelf = openShelf;
  app.clearTapFlash();
  RenderLock lock(*this);
  openShelf = -1;
  groupsCollapsed = true;
  activeNav().reset(shelf + 1);
  requestUpdate();
}

const char* LibraryListActivity::shelfLabel(const int shelf) const {
  if (shelf < 0 || shelf >= static_cast<int>(shelfNames.size())) return "";
  return shelfNames[shelf].empty() ? tr(STR_LIBRARY_NO_SHELF) : shelfNames[shelf].c_str();
}

// Title order, regrouped by folder with a stable counting sort. Shelves are
// keyed on the index folder id, so two folders with the same name stay apart.
// Temporary cost is one byte per book for the shelf ids; the result reuses
// `filtered` (two bytes per book) and `groupStarts` (one entry per shelf).
bool LibraryListActivity::buildShelves() {
  const int total = static_cast<int>(index.bookCount());
  if (total <= 0) return true;
  // The kept array first, so freeing the temporary leaves no hole below it.
  auto order = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(total));
  auto ids = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(total));
  if (!ids || !order) {
    LOG_ERR("LIB", "cannot allocate %u-byte shelf map", static_cast<unsigned>(total * 3));
    return false;
  }
  // Per shelf id: its folder and one book in it (to read the folder's path).
  uint16_t folderOf[MAX_SHELVES];
  uint16_t sampleOf[MAX_SHELVES];
  size_t shelfTotal = 0;
  for (int row = 0; row < total; row++) {
    ids[row] = 0;
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    size_t id = 0;
    while (id < shelfTotal && folderOf[id] != record.folderId) id++;
    if (id == shelfTotal) {
      // Folders past the cap go to the "no shelf" bucket.
      if (shelfTotal == MAX_SHELVES) {
        ids[row] = UINT8_MAX;
        continue;
      }
      folderOf[id] = record.folderId;
      sampleOf[id] = ordinal;
      shelfTotal++;
    }
    ids[row] = static_cast<uint8_t>(id);
  }

  // Names from the folder paths; the root folder and the overflow share the
  // unnamed "no shelf" bucket, kept last.
  shelfNames.clear();
  shelfNames.reserve(shelfTotal + 1);
  shelfFolders.clear();
  shelfFolders.reserve(shelfTotal + 1);
  std::string path;
  for (size_t id = 0; id < shelfTotal; id++) {
    library::ClixRecord record{};
    std::string name;
    std::string folder;
    if (index.readRecord(sampleOf[id], record) && index.readPath(record, path)) {
      shelfNameFor(path, name);
      folder = FsHelpers::extractFolderPath(path);
    }
    shelfNames.push_back(std::move(name));
    shelfFolders.push_back(std::move(folder));
  }
  const size_t bucketCount = shelfTotal + 1;  // + overflow
  shelfNames.emplace_back();
  shelfFolders.emplace_back();
  // A folder without a name (the card root) is "no shelf" too.
  uint8_t bucketOf[MAX_SHELVES];
  for (size_t id = 0; id < shelfTotal; id++) {
    bucketOf[id] = static_cast<uint8_t>(shelfNames[id].empty() ? shelfTotal : id);
  }

  uint8_t byRank[MAX_SHELVES + 1];
  uint8_t rank[MAX_SHELVES + 1];
  {
    std::vector<std::string> folds;
    folds.reserve(bucketCount);
    for (const auto& name : shelfNames) folds.push_back(library::fold(name));
    for (size_t i = 0; i < bucketCount; i++) byRank[i] = static_cast<uint8_t>(i);
    std::stable_sort(byRank, byRank + bucketCount, [&folds](const uint8_t a, const uint8_t b) {
      if (folds[a].empty() != folds[b].empty()) return folds[b].empty();
      return folds[a] < folds[b];
    });
  }
  for (size_t r = 0; r < bucketCount; r++) rank[byRank[r]] = static_cast<uint8_t>(r);
  const auto rankOf = [&](const uint8_t id) { return rank[id == UINT8_MAX ? shelfTotal : bucketOf[id]]; };

  uint16_t counts[MAX_SHELVES + 1] = {};
  for (int row = 0; row < total; row++) counts[rankOf(ids[row])]++;
  uint16_t fill[MAX_SHELVES + 1];
  uint16_t next = 0;
  for (size_t r = 0; r < bucketCount; r++) {
    fill[r] = next;
    next = static_cast<uint16_t>(next + counts[r]);
  }
  for (int row = 0; row < total; row++) order[fill[rankOf(ids[row])]++] = static_cast<uint16_t>(row);
  ids.reset();

  if (groupCapacity < bucketCount) {
    auto groups = makeUniqueNoThrow<uint16_t[]>(bucketCount);
    if (!groups) {
      LOG_ERR("LIB", "OOM: shelf starts");
      return false;
    }
    groupStarts = std::move(groups);
    groupCapacity = static_cast<uint16_t>(bucketCount);
  }
  // Shelves in rank order, empty buckets left out.
  std::vector<std::string> names;
  std::vector<std::string> folders;
  names.reserve(bucketCount);
  folders.reserve(bucketCount);
  groupCount = 0;
  uint16_t start = 0;
  for (size_t r = 0; r < bucketCount; r++) {
    if (counts[r] != 0) {
      groupStarts[groupCount++] = start;
      names.push_back(std::move(shelfNames[byRank[r]]));
      folders.push_back(std::move(shelfFolders[byRank[r]]));
    }
    start = static_cast<uint16_t>(start + counts[r]);
  }
  shelfNames = std::move(names);
  shelfFolders = std::move(folders);
  filtered = std::move(order);
  filteredCount = static_cast<uint16_t>(total);
  return true;
}

void LibraryListActivity::restoreExpandedList() {
  if (!groupsCollapsed) return;
  groupsCollapsed = false;
  activeNav() = expandedNav;
  requestUpdate();
}

// One pass over the sort order, keeping what matches. No index, no cache: at the
// 4096-book format cap this is 4096 comparisons of at most 96 bytes. The result
// array is allocated once with the exact upper bound and fails back to an
// explicit message rather than letting vector growth abort the firmware.
void LibraryListActivity::applyFilter() {
  groupsCollapsed = false;
  groupCount = 0;
  filtered.reset();
  filteredCount = 0;
  filterFailed = false;
  openShelf = -1;
  shelfNames.clear();
  shelfFolders.clear();
  // The header shows the active query in place of the screen title, so the
  // reader can see what narrowed the list without reopening the keyboard.
  headerSearchTitle = query.empty() ? std::string() : "“" + query + "”";
  // A search on the Shelf tab is a plain title search.
  shelfMode = activeTabIndex == SHELF_TAB && query.empty() && !degraded;
  if (shelfMode) {
    if (!buildShelves()) {
      shelfMode = false;
      filterFailed = true;
      return;
    }
    groupsCollapsed = groupCount > 0;
    expandedNav = activeNav();
    return;
  }
  if (query.empty()) return;

  const std::string needle = library::fold(query);
  const int total = static_cast<int>(index.bookCount());
  if (total <= 0) return;

  auto matches = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(total));
  if (!matches) {
    LOG_ERR("LIB", "cannot allocate %u-byte search result buffer", static_cast<unsigned>(total * sizeof(uint16_t)));
    filterFailed = true;
    return;
  }

  uint16_t matchCount = 0;
  std::string author;
  for (int row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    if (library::matchesQuery(std::string_view(record.fold, record.foldLen), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
      continue;
    }
    // The stored fold covers the title only, so the author has to be read and
    // folded here. That is the search most worth having: the reader who knows
    // the author usually also knows where the book is, while "emily" finding
    // Alice Hunter is the case the shelf exists to answer.
    author.clear();
    if (index.readAuthor(record, author) && library::matchesQuery(library::fold(author), needle)) {
      matches[matchCount++] = static_cast<uint16_t>(row);
    }
  }
  filtered = std::move(matches);
  filteredCount = matchCount;
}

// Staged back-out, shared by the Back button and the header's back arrow:
// clear the search, expand collapsed groups, return focus to the tabs, then
// leave for home.
void LibraryListActivity::handleBackAction() {
  auto& nav = activeNav();
  if (!query.empty()) {
    RenderLock lock(*this);
    query.clear();
    applyFilter();
    nav.selected = 0;
    nav.top = 0;
    requestUpdate();
  } else if (openShelf >= 0) {
    closeShelf();
  } else if (groupsCollapsed && !shelfMode) {
    restoreExpandedList();
  } else if (!tabsFocused() && !degraded) {
    // Keep the current list and viewport while returning focus to the tabs.
    nav.selected = 0;
    requestUpdate();
  } else {
    onGoHome();
  }
}

void LibraryListActivity::searchActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openSearch();
}

void LibraryListActivity::backActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->handleBackAction();
}

void LibraryListActivity::loop() {
  UiTabListActivity::loop();
  if (!coverWorkPending) return;
  bool changed = false;
  {
    // Thumb generation parses the book on the card, which the render task's
    // SD fonts also read. Bounded per pass so input is polled again soon; the
    // repaint shows what is done so far.
    RenderLock lock(*this);
    const unsigned long start = millis();
    while (covers.hasWork() && millis() - start < COVER_PASS_MS) changed = covers.loadNext() || changed;
    coverWorkPending = covers.hasWork();
  }
  if (changed) requestUpdate();
}

void LibraryListActivity::rebuildActionTrampoline(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->promptRebuildIndex();
}

// Title and author for one entry, read straight from the index. Only ever
// called for rows about to be drawn, so at most a screenful of strings exists
// at once.
bool LibraryListActivity::rowTextFor(const int entry, std::string& title, std::string& author, std::string* bookPath) {
  title.clear();
  author.clear();
  if (bookPath) bookPath->clear();
  if (entry < pinnedCount()) {
    const auto& books = RECENT_BOOKS.getBooks();
    if (entry < 0 || entry >= static_cast<int>(books.size())) return false;
    const auto& book = books[static_cast<size_t>(entry)];
    title = book.title;
    author = book.author;
    if (bookPath) *bookPath = book.path;
    return true;
  }
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record)) {
    // The build already decided both fields — from the book's own metadata when
    // it has any, and with one spelling chosen per author across the library.
    // Re-parsing the name here would throw that away, and only works while the
    // name still looks like "Title - Author".
    if (!index.readAuthor(record, author)) author.clear();
    // The stored title when the book gave one, the filename otherwise.
    if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
    if (bookPath && !index.readPath(record, *bookPath)) bookPath->clear();
  }
  if (title.empty()) title = tr(STR_LIBRARY_UNKNOWN_TITLE);
  return true;
}

bool LibraryListActivity::handleCustomInput() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return true;
  }
  if (lockNextBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockNextBackRelease = false;
    return true;
  }

  return false;
}

bool LibraryListActivity::handleButtons() {
  const int count = listCount();

  // Every hold action fires at the threshold, mid-hold, including the ones
  // that open a dialog (remove-recent, delete). The release that follows is
  // armed as suppressed by wasLongPressed() and consumed globally by
  // ActivityManager::loop() before any activity runs, so it cannot land in
  // the freshly opened confirmation and select its default.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    if (tabsFocused()) {
      if (!degraded) toggleSortDirection();
    } else if (isRecentSort(sortOrder)) {
      showRecentBookOptions(selectedEntry());
    } else if (deleteEligible()) {
      if (count > 0) promptDeleteBook(selectedEntry());
    } else if (!groupsCollapsed && groupable()) {
      collapseGroups(selectedEntry());
    } else {
      activateIndex(selectedEntry());
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    handleBackAction();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (tabsFocused()) {
      stepTab(1);
      return true;
    }
    if (count > 0) activateIndex(selectedEntry());
    return true;
  }

  return false;
}

void LibraryListActivity::navigateButtons() {
  const int count = listCount();
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    navigationStartedOnTabs = tabsFocused();
  }
  buttonNavigator.onNextPress([this, count] {
    if (count > 0) moveRingTo(ringPos() == count ? 1 : ringPos() + 1);
  });
  buttonNavigator.onPreviousPress([this, count] {
    if ((!navigationStartedOnTabs || degraded) && count > 0) {
      moveRingTo(ringPos() <= 1 ? count : ringPos() - 1);
    }
  });
  // Search is an activation: defer it so holding Previous can still step tabs.
  buttonNavigator.onPreviousRelease([this] {
    if (navigationStartedOnTabs && tabsFocused() && !degraded) openSearch();
  });
  // A held button steps tabs while the strip has focus (the base behaviour
  // Settings keeps) and page-jumps once the selection is down in the rows,
  // where fast travel through a long shelf is what a hold means.
  buttonNavigator.onNextContinuous([this, count] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::nextPageIndex(selectedEntry(), count, activeNav().pageRows()) + 1);
    }
  });
  buttonNavigator.onPreviousContinuous([this, count] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepTab(-1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::previousPageIndex(selectedEntry(), count, activeNav().pageRows()) + 1);
    }
  });
}

void LibraryListActivity::buildRows(UiScreen& screen) {
  if (!groupsCollapsed) {
    buildGrid(screen);
    return;
  }
  auto& nav = activeNav();
  const int count = listCount();
  const bool authorGrouped = isAuthorSort(sortOrder);

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  // Breathing room between rows; the dense theme default packs the shelf rows
  // (name and count) edge-to-edge.
  props.rowGap = std::max<int16_t>(screen.theme().listRowGap, 6);
  props.headerUnderline = false;
  // Win98 list view: the selected row is a square black highlight.
  props.rowRadius = 0;
  props.rowStyles.explicitlySet = true;
  props.rowStyles.selected.background = fui::Paint::solid(fui::Color::Black);
  props.rowStyles.selected.foreground = fui::Paint::solid(fui::Color::White);
  props.rowStyles.focused = props.rowStyles.selected;
  props.rowStyles.active = props.rowStyles.selected;
  syncTabListViewport(screen, props);

  // Keep one extra entry in the reusable window for a clipped trailing row.
  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1) + 1;
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);

  // Capture this after syncTabListViewport(), which may clamp nav.top.
  const int windowStart = static_cast<int>(props.topIndex);
  for (int entry = windowStart; entry < count && winItems.size() < cap; entry++) {
    const size_t row = winItems.size();
    std::string& title = winTitles[row];
    std::string& author = winAuthors[row];
    fui::ListItem item;
    if (shelfMode) {
      title = shelfLabel(entry);
      char books[24];
      snprintf(books, sizeof(books), tr(STR_LIBRARY_SHELF_COUNT), shelfEnd(entry) - groupStarts[entry]);
      author = books;
      item.subtitle = author.c_str();
      item.icon = listIconFor(UIIcon::Folder, 32);
    } else {
      const int bookEntry = groupStarts[entry];
      if (authorGrouped) {
        rowTextFor(bookEntry, title, author);
        formatAuthorHeading(author, title);
      } else {
        formatInitialHeading(titleInitialFor(bookEntry), title);
      }
    }
    item.label = title.c_str();
    item.actionValue = static_cast<int16_t>(entry);
    winItems.push_back(item);
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
  screen.list(props);
}

namespace {
constexpr uint8_t GRID_COLUMNS = 3;
constexpr int GRID_ROWS = 3;
constexpr int16_t GRID_PAD = 6;
constexpr int16_t GRID_GAP = 8;
// Selection ring around the cover: its gap plus its width.
constexpr int16_t RING_GAP = 3;
constexpr int16_t RING_WIDTH = 3;
constexpr int16_t RING_ROOM = RING_GAP + RING_WIDTH;
// The shelf plank under each row of covers, below the selection ring.
constexpr int16_t PLANK_HEIGHT = 5;
constexpr int16_t LABEL_GAP = RING_ROOM + PLANK_HEIGHT + 3;
// Space between a plank and the next row's ring when covers have no titles.
constexpr int16_t BARE_PLANK_ROOM = 6;
}  // namespace

// Covers stand on planks, three to a row, with an optional title under each. Paging is
// by whole pages: e-ink redraws the screen either way, so a page step shows
// nine new books for the same refresh as one new row.
void LibraryListActivity::buildGrid(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = listCount();
  const fui::Rect area{static_cast<int16_t>(listPanel.x + GRID_PAD), static_cast<int16_t>(listPanel.y + GRID_PAD),
                       static_cast<int16_t>(listPanel.width - 2 * GRID_PAD),
                       static_cast<int16_t>(listPanel.height - 2 * GRID_PAD)};

  fui::CoverGridProps grid;
  grid.columns = GRID_COLUMNS;
  grid.gap = GRID_GAP;
  grid.rowGap = 0;
  grid.titleText = screen.theme().smallText;
  grid.titleText.maxLines = 1;
  coverText = grid.titleText;
  coverText.align = fui::TextAlign::Center;
  coverText.maxLines = 5;
  // Without titles the covers grow into the label line.
  const bool showLabels = SETTINGS.libraryCoverTitles != 0;
  const int16_t lineHeight = showLabels ? screen.target().lineHeight(grid.titleText.font) : 0;
  const int16_t belowCover = showLabels ? LABEL_GAP : RING_ROOM + PLANK_HEIGHT + BARE_PLANK_ROOM;
  grid.rowHeight = static_cast<int16_t>(area.height / GRID_ROWS);
  const int scrollRoom = grid.scrollIndicatorWidth + grid.scrollIndicatorGap;
  const int cellWidth = (area.width - scrollRoom - (GRID_COLUMNS - 1) * GRID_GAP) / GRID_COLUMNS;
  // A 2:3 cover as tall as the row allows, narrow enough for its ring. The
  // height names the thumb files, so it is rounded to a coarse step: small
  // layout changes then reuse the thumbs already on the card.
  const int coverHeight =
      std::min(grid.rowHeight - RING_ROOM - belowCover - lineHeight, (cellWidth - 2 * RING_ROOM) * 3 / 2) & ~7;
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

  // ListNav is a linear list: one "row" per cover. It owns the selection,
  // swipe scrolling and follow-on-build; the viewport is then snapped to
  // whole grid rows here.
  const int pageItems = GRID_ROWS * GRID_COLUMNS;
  const int prevTop = nav.top;
  fui::ListProps navProps;
  nav.syncToProps(fui::Rect{0, 0, 1, static_cast<int16_t>(pageItems)}, 1, 0, count, navProps, 1);
  const int selected = navProps.selectedIndex;
  const int totalRows = (count + GRID_COLUMNS - 1) / GRID_COLUMNS;
  const int lastTop = std::max(0, totalRows - GRID_ROWS) * GRID_COLUMNS;
  int top = navProps.topIndex;
  if (nav.followPending) {
    // Keep the page when the selection is already on it.
    const bool onPage = selected >= prevTop && selected < prevTop + pageItems && prevTop % GRID_COLUMNS == 0;
    top = onPage ? prevTop
                 : fui::coverGridTopIndexFor(static_cast<uint16_t>(selected), static_cast<uint16_t>(count),
                                             GRID_COLUMNS, static_cast<uint16_t>(pageItems));
  } else {
    top = (top + GRID_COLUMNS - 1) / GRID_COLUMNS * GRID_COLUMNS;
  }
  top = std::min(top, lastTop);
  nav.top = top;
  nav.drawnCount = count;
  nav.onListRendered(static_cast<uint16_t>(top), pageItems, true);

  // One page of titles and paths.
  const size_t cap = static_cast<size_t>(pageItems);
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (winPaths.size() < cap) winPaths.resize(cap);
  gridTop = top;
  // The Author tab labels covers with the author: it is what that tab sorts by.
  gridShowsAuthors = isAuthorSort(sortOrder);
  covers.beginFrame();
  for (size_t i = 0; i < cap; i++) {
    const int entry = top + static_cast<int>(i);
    if (entry >= count || !rowTextFor(entry, winTitles[i], winAuthors[i], &winPaths[i])) {
      winTitles[i].clear();
      winAuthors[i].clear();
      winPaths[i].clear();
    } else if (gridShowsAuthors && winAuthors[i].empty()) {
      winAuthors[i] = tr(STR_LIBRARY_UNKNOWN_AUTHOR);
    }
  }

  grid.count = static_cast<uint16_t>(count);
  grid.topIndex = static_cast<uint16_t>(top);
  grid.selectedIndex = static_cast<int16_t>(selected);
  grid.action = ACTION_ROW;
  grid.inputMask = fui::InputTouch | fui::InputLongPress;
  grid.itemProviderUserData = this;
  grid.itemProvider = [](const uint16_t index, void* user) {
    auto* self = static_cast<LibraryListActivity*>(user);
    const size_t slot = static_cast<size_t>(index - self->gridTop);
    const auto& labels = self->gridShowsAuthors ? self->winAuthors : self->winTitles;
    return fui::coverGridItem(slot < labels.size() ? labels[slot].c_str() : "", index);
  };
  grid.coverPainterUserData = this;
  grid.coverPainter = [](fui::DrawTarget& target, const fui::Rect rect, const fui::CoverGridItem&, const uint16_t index,
                         void* user) {
    static_cast<LibraryListActivity*>(user)->paintGridCover(target, rect, index);
    return true;
  };
  fui::coverGrid(screen.frame(), area, grid);

  // Planks: a dark board under each row of covers, from the left wall to the
  // scroll track (or the right wall when there is none).
  const int rowsDrawn = std::min(GRID_ROWS, (count - top + GRID_COLUMNS - 1) / GRID_COLUMNS);
  const auto plankX = static_cast<int16_t>(listPanel.x);
  const int plankRight = count > pageItems ? area.right() - scrollRoom : listPanel.x + listPanel.width;
  const auto plankWidth = static_cast<int16_t>(plankRight - listPanel.x);
  for (int row = 0; row < rowsDrawn; row++) {
    const auto y = static_cast<int16_t>(area.y + row * (grid.rowHeight + grid.rowGap) + grid.cellInset.top +
                                        coverHeight + RING_ROOM);
    screen.target().fill(fui::Rect{plankX, y, plankWidth, 2}, fui::Paint::solid(fui::Color::Black));
    screen.target().fill(fui::Rect{plankX, static_cast<int16_t>(y + 2), plankWidth, PLANK_HEIGHT - 2},
                         fui::Paint::dither(fui::Color::DarkGray));
  }
  coverWorkPending = covers.hasWork();
}

// The cover from the card, or a plain bound book with the title on it.
void LibraryListActivity::paintGridCover(fui::DrawTarget& target, const fui::Rect rect, const int index) {
  const size_t slot = static_cast<size_t>(index - gridTop);
  const bool inPage = slot < winPaths.size() && slot < winTitles.size();
  if (!inPage || !covers.paint(renderer, Rect{rect.x, rect.y, rect.width, rect.height}, winPaths[slot])) {
    const char* title = inPage ? winTitles[slot].c_str() : nullptr;
    target.fill(rect, fui::Paint::solid(fui::Color::White));
    // Spine down the left edge.
    target.fill(fui::Rect{rect.x, rect.y, 3, rect.height}, fui::Paint::solid(fui::Color::Black));
    target.fill(fui::Rect{static_cast<int16_t>(rect.x + 3), rect.y, 3, rect.height},
                fui::Paint::dither(fui::Color::DarkGray));
    if (title && *title) {
      const fui::Rect inner{static_cast<int16_t>(rect.x + 8), static_cast<int16_t>(rect.y + rect.height / 5),
                            static_cast<int16_t>(rect.width - 10), static_cast<int16_t>(rect.height * 3 / 5)};
      target.text(inner, title, coverText);
    }
  }
  target.stroke(rect, fui::Paint::solid(fui::Color::Black), 1, 0);
}

void LibraryListActivity::formatInitialHeading(uint32_t initial, std::string& out) {
  out.clear();
  if (initial == 0) {
    out.push_back('#');
    return;
  }
  if (initial >= 'a' && initial <= 'z') initial -= 'a' - 'A';
  utf8AppendCodepoint(initial, out);
}

void LibraryListActivity::formatAuthorHeading(const std::string& author, std::string& out) const {
  out = author.empty() ? std::string(tr(STR_LIBRARY_UNKNOWN_AUTHOR)) : author;
  if (author.empty()) return;
  const size_t lastSpace = out.find_last_of(' ');
  if (lastSpace != std::string::npos && lastSpace + 1 < out.size()) {
    out = out.substr(lastSpace + 1) + ", " + out.substr(0, lastSpace);
  }
}

namespace {
constexpr int16_t CORNER_STATUS_INSET = 10;
constexpr int16_t TITLE_GAP = 10;
constexpr uint8_t CAPTION_RADIUS = 10;
}  // namespace

void LibraryListActivity::buildHeader(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  fui::HeaderProps header;
  header.title = headerTitle();
  header.titleText = theme.titleText;
  header.titleText.align = theme.headerTitleAlign;
  header.sidePadding = theme.headerSidePadding;
  header.minTouchSize = theme.minTouchSize;
  // Same battery/clock band as every GUI.drawHeader screen; the header
  // heights are unified across themes, so the buttons derive from the band.
  GUI.applyHeaderStatus(renderer, header);
  // A white title bar with a dark rule under it: softer on e-ink than a solid
  // black bar. Caption buttons are rounded.
  header.styles = fui::StyleSet{};
  header.styles.explicitlySet = true;
  header.styles.normal.background = fui::Paint::solid(fui::Color::White);
  header.styles.normal.border = fui::Paint::solid(fui::Color::Black);
  header.styles.normal.borderWidth = 2;
  header.borderEdges = fui::EdgeBottom;
  header.titleText.bold = true;
  // Clock and battery sit in the corners, in line with the caption buttons.
  header.status.edgeInset = CORNER_STATUS_INSET;
  // Room between the back button and the title.
  header.leftReserve = TITLE_GAP;
  fui::StyleSet captionButton;
  captionButton.explicitlySet = true;
  captionButton.normal.background = fui::Paint::solid(fui::Color::White);
  captionButton.normal.border = fui::Paint::solid(fui::Color::Black);
  captionButton.normal.borderWidth = 1;
  captionButton.normal.foreground = fui::Paint::solid(fui::Color::Black);
  captionButton.selected = captionButton.normal;
  captionButton.focused = captionButton.normal;
  captionButton.active = captionButton.normal;
  captionButton.active.background = fui::Paint::solid(fui::Color::Black);
  captionButton.active.foreground = fui::Paint::solid(fui::Color::White);
  header.leadingStyles = captionButton;
  header.trailingStyles = captionButton;
  header.leadingRadius = CAPTION_RADIUS;
  header.trailingRadius = CAPTION_RADIUS;
  constexpr int16_t CAPTION_BUTTON = 40;
  header.actionOffsetY = static_cast<int16_t>(header.actionOffsetY + (header.leadingSize - CAPTION_BUTTON) / 2);
  header.leadingSize = CAPTION_BUTTON;
  header.trailingSize = CAPTION_BUTTON;
  if (mappedInput.hasTouch()) {
    header.leadingIcon = fui::bitmapFromIcon(icon_header_back_32);
    header.leadingAction = ACTION_BACK;
  }
  if (!degraded) {
    // Keep both touch actions together on the right; button boards reach
    // rebuild through the row options menu.
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
    if (mappedInput.hasTouch()) {
      header.trailingAdjacentIcon = fui::bitmapFromIcon(icon_refresh_cw_32);
      header.trailingAdjacentAction = ACTION_REBUILD;
    }
    // Vertical placement comes from applyHeaderStatus: buttons center on the
    // unified band.
  }
  const auto frameRect = screen.frame().screen();
  // Header and tabs share a screen-relative boundary, independent of bezel insets.
  const fui::Rect band = BaseTheme::headerBand(renderer,
                                               fui::Rect{frameRect.x, static_cast<int16_t>(metrics.topPadding),
                                                         frameRect.width, static_cast<int16_t>(metrics.headerHeight)},
                                               header);
  fui::header(screen.frame(), band, header);
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The position readout owns the line above the hints; rows must not overlap
  // it.
  const int16_t readoutReserved = static_cast<int16_t>(statusBarHeight() + STATUS_BAR_GAP);
  const Rect area = UITheme::getContentArea(renderer);
  const auto bottomInset = static_cast<int16_t>(renderer.getScreenHeight() - area.y - area.height);
  // Only a cover grid built below asks loop() for thumbs.
  coverWorkPending = false;
  buildHeader(screen);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                  static_cast<int16_t>(bottomInset + metrics.buttonHintsHeight + readoutReserved), 0});

  if (!degraded) buildTabBar(screen);
  // White list view on the grey window face, ending above the status bar.
  const auto body = screen.body();
  const Rect bar = statusBarRect();
  listPanel = Rect{bar.x, body.y, bar.width, bar.y - STATUS_BAR_GAP - body.y};
  screen.target().fill(fui::Rect{static_cast<int16_t>(listPanel.x), static_cast<int16_t>(listPanel.y),
                                 static_cast<int16_t>(listPanel.width), static_cast<int16_t>(listPanel.height)},
                       fui::Paint::solid(fui::Color::White));
  if (bookRowCount() == 0) {
    const char* message = tr(STR_LIBRARY_NO_RESULTS);
    if (filterFailed) {
      message = tr(STR_LIBRARY_SEARCH_UNAVAILABLE);
    } else if (query.empty()) {
      message = tr(STR_LIBRARY_EMPTY);
    }
    screen.centeredText(message);
    return;
  }
  buildRows(screen);
}

int LibraryListActivity::statusBarHeight() const { return renderer.getLineHeight(SMALL_FONT_ID) + 8; }

// Win98 status bar along the bottom: a sunken box holding the readout.
Rect LibraryListActivity::statusBarRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect area = UITheme::getContentArea(renderer);
  const int side = area.x + 6;
  const int h = statusBarHeight();
  const int y = area.y + area.height - metrics.buttonHintsHeight - h - std::max(6, bottomCornerClearance() / 2);
  return Rect{side, y, renderer.getScreenWidth() - 2 * side, h};
}

// "12/69 books" in the status bar: which book is selected, out of how many.
//
// NOT a page count: the position stays the same when the view changes (grid
// or group list), and it answers the question the reader actually has: how
// far in am I, and how much is left.
void LibraryListActivity::drawPositionReadout() const {
  const Rect bar = statusBarRect();
  renderer.fillRect(bar.x, bar.y, bar.width, bar.height, false);
  win98::drawSunken(renderer, bar.x, bar.y, bar.width, bar.height);
  const int count = listCount();
  if (count <= 0) return;

  char buf[32];
  const char* positionFormat = groupsCollapsed ? tr(STR_LIBRARY_GROUP_POSITION) : tr(STR_LIBRARY_POSITION);
  if (groupsCollapsed && shelfMode) positionFormat = tr(STR_LIBRARY_SHELF_POSITION);
  snprintf(buf, sizeof(buf), positionFormat, selectedEntry() + 1, count);
  const int width = renderer.getTextWidth(SMALL_FONT_ID, buf);
  const int y = bar.y + (bar.height - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
  renderer.drawText(SMALL_FONT_ID, bar.x + bar.width - width - 8, y, buf, true);
}

// Sunken white frame around the list, like a Win98 list view.
void LibraryListActivity::drawListFrame() const {
  if (listPanel.width <= 0 || listPanel.height <= 0) return;
  win98::drawSunken(renderer, listPanel.x, listPanel.y, listPanel.width, listPanel.height);
}

const char* LibraryListActivity::headerTitle() const {
  if (!headerSearchTitle.empty()) return headerSearchTitle.c_str();
  if (openShelf >= 0) return shelfLabel(openShelf);
  return degraded ? tr(STR_LIBRARY_TITLE_UNSORTED) : tr(STR_W98_MY_BOOKS);
}

void LibraryListActivity::drawHoldHelp() const {
  if (mappedInput.hasTouch() || groupsCollapsed) return;
  const char* help = nullptr;
  if (tabsFocused() && !degraded && activeTabIndex != SHELF_TAB)
    help = tr(STR_LIBRARY_HOLD_SORT);
  else if (!tabsFocused() && isRecentSort(sortOrder) && listCount() > 0)
    help = tr(STR_LIBRARY_HOLD_OPTIONS);  // recent rows: hold opens the row menu
  else if (!tabsFocused() && deleteEligible() && listCount() > 0)
    help = tr(STR_HOLD_OPEN_TO_DELETE);
  else if (!tabsFocused() && groupable())
    help = tr(STR_LIBRARY_HOLD_GROUPS);
  if (!help) return;

  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const Rect bar = statusBarRect();
  GUI.drawHelpText(renderer, Rect{bar.x + 8, bar.y + (bar.height - lineHeight) / 2, bar.width / 2 - 8, lineHeight},
                   help);
}

// OptionPopup is a self-contained modal: it owns the whole frame (hints
// included) whenever it is up, mirroring the FileBrowser pattern.
void LibraryListActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiTabListActivity::render(std::move(lock));
}

// Grey Win98 window face; the header, tabs and list view paint over it.
void LibraryListActivity::drawChrome() {
  renderer.fillRectDither(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(), Color::LightGray);
}

void LibraryListActivity::drawFooter() {
  drawListFrame();
  drawPositionReadout();
  drawHoldHelp();

  const bool backGoesHome = tabsFocused() && (!groupsCollapsed || shelfMode) && query.empty() && openShelf < 0;
  const char* backLabel = backGoesHome ? tr(STR_HOME) : tr(STR_BACK);
  const char* confirmLabel = groupsCollapsed ? tr(STR_SELECT) : tr(STR_OPEN);
  const bool canSearch = tabsFocused() && !degraded;
  const auto labels = mappedInput.mapLabels(backLabel, tabsFocused() ? tr(STR_TOGGLE) : confirmLabel,
                                            canSearch ? tr(STR_SEARCH) : tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
