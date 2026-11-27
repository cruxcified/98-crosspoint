#pragma once

#include <LibraryIndexFile.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "LibraryCoverCache.h"
#include "RecentBooksStore.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"

// One Library screen: every indexed book on the card shown by recency, title,
// author, arrival (New), or folder (Shelf); books show as a cover grid. The
// Recent tab orders by file modification time (when a book landed on the card)
// and pins the recently OPENED books from RecentBooksStore on top, so active
// reads and fresh arrivals share one list. New is the same order without pins.
//
// Books render as a fui::coverGrid bookshelf (covers on planks, one-line
// titles); collapsed groups and the shelf list stay fui::list rows. Both sit on
// the UiTabListActivity ring (0 = the sort strip, 1..N = the entries), which is
// what brings touch to entries and tabs. The grid keeps that ring's linear
// selection: Next/Previous walk the covers in reading order.
//
// Only the visible window is materialized per render (strings and items for at
// most one page). The ordinary shelf therefore keeps one page of strings; an
// active search additionally uses one fallible uint16_t slot per indexed book
// so an allocation failure remains recoverable on the C3.
class LibraryListActivity final : public UiTabListActivity {
 public:
  LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 protected:
  // --- UiListActivity / UiTabListActivity contract ---------------------------
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  int tabCount() const override;
  int activeTab() const override;
  const char* tabLabel(int index) const override;
  freeink::ui::TabIndicator tabIndicator(int index) const override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void navigateButtons() override;
  // The FreeInkUI header owns both the title and search touch target.
  void drawChrome() override;
  void drawFooter() override;
  // OptionPopup is a self-contained modal: it owns rendering (and the button
  // hints) whenever it is up.
  void render(RenderLock&& lock) override;

 private:
  // The screen's own actions, after the base's ACTION_ROW / ACTION_TAB.
  static constexpr freeink::ui::ActionId ACTION_SEARCH = ACTION_TAB_USER;
  static constexpr freeink::ui::ActionId ACTION_REBUILD = ACTION_SEARCH + 1;
  static constexpr freeink::ui::ActionId ACTION_BACK = ACTION_REBUILD + 1;

  // Walk the card and write a fresh index. Blocking, with a popup: at ~70 books
  // it is well under a second, and it only runs when the index is missing or the
  // user asks.
  bool rebuildIndex();

  // Input
  void openSelectedBook();
  void openSearch();
  // Shared tail of row activation and the options menu's Open entry.
  void openBookByPath(const std::string& path);
  void promptRebuildIndex();
  void resetAfterRebuild();
  // Recent-row long-press menu: open / find cover / use book's own cover /
  // remove from recents / delete / rebuild.
  void showRecentBookOptions(int entry);
  // Find Cover (Open Library over WiFi) and its undo.
  void openCoverPicker(const std::string& path);
  void restoreBookCover(const std::string& path);
  void promptRemoveRecentBook(const std::string& path, const std::string& title);
  // Long-press delete owns the gesture where grouping does not apply: the
  // Recent sort, degraded lists, and any active search result.
  bool deleteEligible() const;
  // Resolves the row's path and title, then confirms via promptDeleteBookByPath.
  void promptDeleteBook(int entry);
  void promptDeleteBookByPath(const std::string& path, const std::string& title);
  bool collapseGroups(int bookEntry);
  void expandGroup(int groupEntry);
  void restoreExpandedList();
  // Shelf tab: one group per card folder, built over the title order.
  bool buildShelves();
  void closeShelf();
  int shelfEnd(int shelf) const;
  const char* shelfLabel(int shelf) const;
  // A search or the shelf order narrows/reorders rows through `filtered`.
  bool filterActive() const;
  void selectTab(int index, bool toggleIfActive);
  void toggleSortDirection();
  // Sub-screens act on button press, so a button still held when we resume must
  // not also act here. Records what to swallow on the next release.
  void swallowHeldReleases();
  // Staged back-out shared by Button::Back and the header back arrow.
  void handleBackAction();
  static void searchActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void rebuildActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void backActionTrampoline(const freeink::ui::ActionEvent& event, void* user);

  // Data
  void applyFilter();
  int bookRowCount() const;
  int rowFor(int entry) const;
  // bookPath, when asked for, is the full card path (for the cover thumb).
  bool rowTextFor(int entry, std::string& title, std::string& author, std::string* bookPath = nullptr);
  uint32_t titleInitialFor(int entry);
  bool buildGroupStarts();
  int groupForBook(int bookEntry) const;
  bool groupable() const;

  // Screen building
  void buildHeader(UiScreen& screen);
  // Materializes ListItems and their strings for the visible window only.
  void buildRows(UiScreen& screen);
  // Book views: the cover grid, one page of titles and paths at a time.
  void buildGrid(UiScreen& screen);
  void paintGridCover(freeink::ui::DrawTarget& target, freeink::ui::Rect rect, int index);
  static void formatInitialHeading(uint32_t initial, std::string& out);
  void formatAuthorHeading(const std::string& author, std::string& out) const;
  void drawPositionReadout() const;
  void drawListFrame() const;
  int statusBarHeight() const;
  Rect statusBarRect() const;
  void drawHoldHelp() const;
  const char* headerTitle() const override;

  // Ring 0 is the strip; the selected BOOK is ring - 1, with the strip keeping
  // row 0 as the working selection exactly as the pre-ring code did.
  int selectedEntry() const;
  bool tabsFocused() const { return ringPos() == 0; }

  // --- pinned recently-opened overlay ---------------------------------------
  // On the unfiltered Recent tab the RecentBooksStore entries sit on top, in
  // read order; the modification-time list follows with those books skipped.
  // Entries below pinnedCount() are store rows; the rest go through rowFor().
  int pinnedCount() const;
  // Setting: the Recent tab lists opened books only.
  bool openedOnly() const;
  const RecentBook* storeBook(int entry) const;
  // Re-match the store against the index (chunked scan). Call whenever the
  // index or the store changes.
  void resolvePinned();
  // Direction-space translation of the matched rows. Call on sort toggles.
  void refreshOverlap();

  library::LibraryIndexFile index;
  int activeTabIndex = 0;
  library::SortOrder sortOrder = library::SortOrder::RecentDesc;
  // One bit per tab; Recent starts descending (newest first).
  // Recent (0) and New (3) start descending (newest first); Shelf (4) A-Z.
  uint8_t descendingTabs = (1u << 0) | (1u << 3);
  // Set when the walk finished but the sort did not, so the screen can say the
  // order is discovery order rather than silently showing a wrong one.
  bool degraded = false;

  // Rows surviving the current query, as positions in the active sort order.
  // Empty query means no filtering and this owns no allocation, so the ordinary
  // shelf pays nothing proportional to the library for the feature.
  std::string query;
  // The active query, pre-quoted for the header: headerTitle() returns a
  // stable c_str the render task can hold across a build.
  std::string headerSearchTitle;
  std::unique_ptr<uint16_t[]> filtered;
  uint16_t filteredCount = 0;
  bool filterFailed = false;

  // One start row per group. Grouping is only offered for the sorted <=512-book
  // index, so this fallible allocation is at most 1 KiB and is reused after its
  // first successful allocation.
  std::unique_ptr<uint16_t[]> groupStarts;
  uint16_t groupCapacity = 0;
  uint16_t groupCount = 0;
  bool groupsCollapsed = false;
  // Shelf tab state. While shelfMode, groupStarts holds one start per shelf
  // and shelfNames its folder name (empty = no shelf); openShelf narrows the
  // rows to one of them.
  bool shelfMode = false;
  int openShelf = -1;
  std::vector<std::string> shelfNames;
  // Folder path per shelf ("" for the no-shelf bucket).
  std::vector<std::string> shelfFolders;
  freeink::ui::ListNav expandedNav;

  // Visible-window storage, reused across renders (buildRows, buildGrid).
  // Bounded by one page, never by the library.
  std::vector<freeink::ui::ListItem> winItems;
  std::vector<std::string> winTitles;
  std::vector<std::string> winAuthors;
  // Cover grid page: card path per visible cover, and the entry of the first.
  std::vector<std::string> winPaths;
  int gridTop = 0;
  bool gridShowsAuthors = false;
  // Title printed on a cover-less book.
  freeink::ui::TextStyle coverText{};

  // Pinned overlay state: per store entry its RecentAsc row (0xFFFF when the
  // book is not in the index), and the current-direction rows to skip, sorted
  // ascending, so unpinned entries map to sort rows with a <=10-step walk.
  uint16_t pinnedAscRows[RecentBooksStore::MAX_RECENT_BOOKS] = {};
  uint16_t overlapRows[RecentBooksStore::MAX_RECENT_BOOKS] = {};
  uint8_t pinnedTotal = 0;
  uint8_t overlapCount = 0;

  bool lockNextConfirmRelease = false;
  bool lockNextBackRelease = false;

  // Row options modal (Recent long-press menu); owned here so it outlives the
  // touch event that opened it.
  OptionPopup optionPopup;

  // Grid covers. Painted by the render task; missing thumbs are generated in
  // loop(), which the flag tells cheaply without taking the render lock.
  LibraryCoverCache covers;
  std::atomic<bool> coverWorkPending{false};
  Rect listPanel{};  // list body from the last build, framed by drawFooter
};
