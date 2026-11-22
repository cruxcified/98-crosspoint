#include "Win98DesktopActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "CrossPointSettings.h"
#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/Win98Chrome.h"
#include "components/Win98Wallpaper.h"
#include "components/icons/blocks.h"
#include "components/icons/book.h"
#include "components/icons/computer.h"
#include "components/icons/gear.h"
#include "components/icons/library.h"
#include "components/icons/notepad.h"
#include "components/icons/transfer.h"
#include "fontIds.h"

namespace {

// Taps above this line open the frontlight panel (ActivityManager), so the
// icon grid starts below it.
constexpr int DESKTOP_TOP = 52;
constexpr int CELL_W = 160;
constexpr int CELL_H = 136;
constexpr int ICON = 32;       // source icon size
constexpr int ICON_SCALE = 2;  // desktop icons are drawn at 64 px
constexpr int LABEL_PAD = 3;

constexpr int TASKBAR_H = 48;
constexpr int START_W = 104;
constexpr int TASKBAR_BUTTON_H = 36;

constexpr int MENU_W = 380;
constexpr int MENU_ROW_H = 52;
constexpr int MENU_SEPARATOR_H = 10;
constexpr int MENU_BANNER_W = 32;
constexpr int MENU_BORDER = 3;

constexpr unsigned long CLOCK_CHECK_MS = 5000;

// The UI font slots hold the VCR OSD Mono pixel font (see main.cpp).
constexpr int DESKTOP_FONT = UI_10_FONT_ID;

// Icons are 1-bpp, MSB first, 0 = ink, stored rotated as GfxRenderer::drawIcon
// expects: screen pixel (sx, sy) is row 31 - sx, column sy.
bool inkAt(const uint8_t* bitmap, const int sx, const int sy) {
  const int row = ICON - 1 - sx;
  const int col = sy;
  return ((bitmap[row * (ICON / 8) + (col >> 3)] >> (7 - (col & 7))) & 1) == 0;
}

// Draws an icon at `scale`. With `fillInside`, pixels enclosed by ink are
// painted white so the icon reads cleanly on the dithered desktop.
void drawIconScaled(const GfxRenderer& r, const uint8_t* bitmap, const int x, const int y, const int scale,
                    const bool black, const bool fillInside) {
  if (fillInside) {
    // Bit sx of outside[sy]: reachable from the icon edge without crossing ink.
    uint32_t outside[ICON] = {};
    bool changed = true;
    while (changed) {
      changed = false;
      for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < ICON * ICON; ++i) {
          const int k = pass == 0 ? i : ICON * ICON - 1 - i;
          const int sx = k % ICON;
          const int sy = k / ICON;
          if ((outside[sy] >> sx) & 1u || inkAt(bitmap, sx, sy)) continue;
          const bool edge = sx == 0 || sy == 0 || sx == ICON - 1 || sy == ICON - 1;
          if (edge || (outside[sy] >> (sx - 1)) & 1u || (outside[sy] >> (sx + 1)) & 1u ||
              (outside[sy - 1] >> sx) & 1u || (outside[sy + 1] >> sx) & 1u) {
            outside[sy] |= 1u << sx;
            changed = true;
          }
        }
      }
    }
    for (int sy = 0; sy < ICON; ++sy) {
      for (int sx = 0; sx < ICON; ++sx) {
        if (!((outside[sy] >> sx) & 1u) && !inkAt(bitmap, sx, sy)) {
          r.fillRect(x + sx * scale, y + sy * scale, scale, scale, !black);
        }
      }
    }
  }
  for (int sy = 0; sy < ICON; ++sy) {
    for (int sx = 0; sx < ICON; ++sx) {
      if (inkAt(bitmap, sx, sy)) r.fillRect(x + sx * scale, y + sy * scale, scale, scale, black);
    }
  }
}

// Four-pane window logo for the Start button.
void drawLogo(const GfxRenderer& r, const int x, const int y) {
  r.fillRect(x, y, 8, 8, true);
  r.fillRectDither(x + 10, y, 8, 8, Color::DarkGray);
  r.fillRectDither(x, y + 10, 8, 8, Color::DarkGray);
  r.fillRect(x + 10, y + 10, 8, 8, true);
}

bool formatClock(char* buf, const size_t size) {
  return halClock.isAvailable() && halClock.formatTime(buf, size, SETTINGS.clockFormat == 1);
}

}  // namespace

bool Win98DesktopActivity::shownOnDesktop(const Action action) {
  switch (action) {
    case Action::Files:
      return SETTINGS.desktopShowComputer != 0;
    case Action::Library:
      return SETTINGS.desktopShowLibrary != 0;
    case Action::Notepad:
      return SETTINGS.desktopShowNotepad != 0;
    case Action::Transfer:
      return SETTINGS.desktopShowNetwork != 0;
    case Action::Plugins:
    case Action::Opds:
      return SETTINGS.desktopShowPlugins != 0;
    default:
      return true;
  }
}

void Win98DesktopActivity::onEnter() {
  Activity::onEnter();

  // Start menu: every app, then Shut Down. At most 7 entries.
  menuItems.clear();
  menuItems.reserve(7);
  menuItems.push_back({Action::Files, ComputerIcon, tr(STR_W98_MY_COMPUTER)});
  menuItems.push_back({Action::Library, LibraryIcon, tr(STR_W98_MY_BOOKS)});
  menuItems.push_back({Action::Notepad, NotepadIcon, tr(STR_NOTEPAD_TITLE)});
  menuItems.push_back({Action::Transfer, TransferIcon, tr(STR_W98_NETWORK)});
  menuItems.push_back({Action::Settings, GearIcon, tr(STR_SETTINGS_TITLE)});
  // Same rule as HomeActivity::slotItem(): Plugins wins over OPDS.
  if (SETTINGS.pluginsEnabled != 0) {
    menuItems.push_back({Action::Plugins, BlocksIcon, tr(STR_PLUGINS)});
  } else if (OPDS_STORE.hasServers()) {
    menuItems.push_back({Action::Opds, BookIcon, tr(STR_OPDS_BROWSER)});
  }

  // Desktop: the apps, then up to three recent books, as the setting allows.
  desktopItems.clear();
  desktopItems.reserve(menuItems.size() + MAX_DESKTOP_BOOKS);
  if (SETTINGS.desktopIcons == CrossPointSettings::DESKTOP_ICONS_ALL) {
    for (const Item& item : menuItems) {
      if (shownOnDesktop(item.action)) desktopItems.push_back(item);
    }
  }
  const bool showBooks = SETTINGS.desktopIcons != CrossPointSettings::DESKTOP_ICONS_NONE;
  bookCount = 0;
  for (const RecentBook& book : RECENT_BOOKS.getBooks()) {
    if (bookCount == MAX_DESKTOP_BOOKS) break;
    if (RecentBooksStore::isMissing(book)) continue;
    if (showBooks) {
      desktopItems.push_back(
          {Action::Resume, BookIcon, book.title.empty() ? tr(STR_CONTINUE_READING) : book.title, bookCount});
    }
    bookPaths[bookCount++] = book.path;
  }
  menuItems.push_back({Action::ShutDown, nullptr, tr(STR_W98_SHUT_DOWN)});

  wallpaperKey = win98::wallpaperKey(renderer, SETTINGS.desktopWallpaper);
  hasWallpaper = !wallpaperKey.empty();
  if (!hasWallpaper) win98::releaseWallpaper();

  // Return to the icon the user opened.
  Action initial = Action::Files;
  switch (initialMenuItem) {
    case HomeMenuItem::LIBRARY:
      initial = Action::Library;
      break;
    case HomeMenuItem::NOTEPAD:
      initial = Action::Notepad;
      break;
    case HomeMenuItem::FILE_TRANSFER:
      initial = Action::Transfer;
      break;
    case HomeMenuItem::SETTINGS_MENU:
      initial = Action::Settings;
      break;
    case HomeMenuItem::PLUGINS:
      initial = Action::Plugins;
      break;
    case HomeMenuItem::OPDS_BROWSER:
      initial = Action::Opds;
      break;
    default:
      break;
  }
  int initialIndex = 0;
  for (int i = 0; i < static_cast<int>(desktopItems.size()); ++i) {
    if (desktopItems[i].action == initial) initialIndex = i;
  }
  selected = initialIndex;
  menuSelected = 0;
  menuOpen = false;
  lastClockCheck = millis();

  requestUpdate();
}

int Win98DesktopActivity::taskbarTop() const { return renderer.getScreenHeight() - TASKBAR_H; }

int Win98DesktopActivity::visibleIconCount() const {
  const int rows = std::max(1, (taskbarTop() - DESKTOP_TOP) / CELL_H);
  const int cols = std::max(1, renderer.getScreenWidth() / CELL_W);
  return std::min(static_cast<int>(desktopItems.size()), rows * cols);
}

// Win98 fills the desktop top to bottom, then left to right.
Win98DesktopActivity::Box Win98DesktopActivity::iconCell(const int index) const {
  const int rows = std::max(1, (taskbarTop() - DESKTOP_TOP) / CELL_H);
  return {(index / rows) * CELL_W, DESKTOP_TOP + (index % rows) * CELL_H, CELL_W, CELL_H};
}

Win98DesktopActivity::Box Win98DesktopActivity::startButton() const {
  return {4, taskbarTop() + (TASKBAR_H - TASKBAR_BUTTON_H) / 2, START_W, TASKBAR_BUTTON_H};
}

Win98DesktopActivity::Box Win98DesktopActivity::startMenu() const {
  const int h = static_cast<int>(menuItems.size()) * MENU_ROW_H + MENU_SEPARATOR_H + 2 * MENU_BORDER;
  return {2, taskbarTop() + 2 - h, MENU_W, h};
}

Win98DesktopActivity::Box Win98DesktopActivity::menuRow(const int index) const {
  const Box m = startMenu();
  const int x = m.x + MENU_BORDER + MENU_BANNER_W;
  // Shut Down is last, below a separator.
  const bool last = index == static_cast<int>(menuItems.size()) - 1;
  const int y = m.y + MENU_BORDER + index * MENU_ROW_H + (last ? MENU_SEPARATOR_H : 0);
  return {x, y, m.x + m.w - MENU_BORDER - x, MENU_ROW_H};
}

void Win98DesktopActivity::loop() {
  checkClock();

  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    handleTap(tx, ty);
    return;
  }

  if (menuOpen) {
    const int count = static_cast<int>(menuItems.size());
    buttonNavigator.onNext([this, count] {
      menuSelected = ButtonNavigator::nextIndex(menuSelected, count);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, count] {
      menuSelected = ButtonNavigator::previousIndex(menuSelected, count);
      requestUpdate();
    });
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      menuOpen = false;
      activate(menuItems[menuSelected].action);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      menuOpen = false;
      requestUpdate();
    }
    return;
  }

  const int count = visibleIconCount();
  if (count == 0) {
    // No icons: Confirm opens the Start menu.
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      menuSelected = 0;
      menuOpen = true;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back) && bookCount > 0) {
      activate(Action::Resume);
    }
    return;
  }
  buttonNavigator.onNext([this, count] {
    selected = ButtonNavigator::nextIndex(selected, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selected = ButtonNavigator::previousIndex(selected, count);
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && selected < count) {
    activate(desktopItems[selected].action, desktopItems[selected].book);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back) && bookCount > 0) {
    // Back is otherwise unused at home: resume the last book, as HomeActivity does.
    activate(Action::Resume);
  }
}

void Win98DesktopActivity::handleTap(const int x, const int y) {
  if (menuOpen) {
    menuOpen = false;
    for (int i = 0; i < static_cast<int>(menuItems.size()); ++i) {
      if (menuRow(i).contains(x, y)) {
        haptic_feedback::touchAction();
        activate(menuItems[i].action);
        return;
      }
    }
    // Any other tap, including Start again, just closes the menu.
    requestUpdate();
    return;
  }

  if (startButton().contains(x, y)) {
    haptic_feedback::touchAction();
    menuSelected = 0;
    menuOpen = true;
    requestUpdate();
    return;
  }

  for (int i = 0; i < visibleIconCount(); ++i) {
    if (iconCell(i).contains(x, y)) {
      haptic_feedback::touchAction();
      selected = i;
      activate(desktopItems[i].action, desktopItems[i].book);
      return;
    }
  }
}

void Win98DesktopActivity::activate(const Action action, const int book) {
  switch (action) {
    case Action::Resume:
      if (book >= 0 && book < bookCount) activityManager.goToReader(bookPaths[book]);
      break;
    case Action::Files:
      activityManager.goToFileBrowser();
      break;
    case Action::Library:
      activityManager.goToLibrary();
      break;
    case Action::Notepad:
      activityManager.goToNotepad();
      break;
    case Action::Transfer:
      activityManager.goToFileTransfer();
      break;
    case Action::Settings:
      activityManager.goToSettings();
      break;
    case Action::Plugins:
      activityManager.goToPlugins();
      break;
    case Action::Opds:
      activityManager.goToBrowser();
      break;
    case Action::ShutDown:
      activityManager.requestSleep();
      break;
  }
}

// Redraws once the clock text changes. Skips the check while a render holds
// the lock so input polling is never blocked behind an e-ink refresh. The
// clock is read under the lock because render() also reads it.
void Win98DesktopActivity::checkClock() {
  if (millis() - lastClockCheck < CLOCK_CHECK_MS) return;
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return;
  lastClockCheck = millis();
  char now[sizeof(shownClock)] = {};
  formatClock(now, sizeof(now));
  if (strcmp(now, shownClock) != 0) requestUpdate();
}

void Win98DesktopActivity::render(RenderLock&&) {
  const bool open = menuOpen;
  renderer.clearScreen();
  // A wallpaper that fails to draw falls back to the grey desktop for good.
  if (hasWallpaper) hasWallpaper = win98::drawWallpaper(renderer, SETTINGS.desktopWallpaper, wallpaperKey);
  if (!hasWallpaper) renderer.fillRectDither(0, 0, renderer.getScreenWidth(), taskbarTop(), Color::LightGray);
  drawDesktop(open ? -1 : selected.load());
  drawTaskbar(open);
  if (open) drawStartMenu(menuSelected);
  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstRenderDone = true;
}

void Win98DesktopActivity::drawDesktop(const int selectedIndex) const {
  const int iconSize = ICON * ICON_SCALE;
  for (int i = 0; i < visibleIconCount(); ++i) {
    const Box c = iconCell(i);
    const Item& item = desktopItems[i];
    const int ix = c.x + (c.w - iconSize) / 2;
    const int iy = c.y + 4;
    if (hasWallpaper) {
      // White halo so dark parts of the wallpaper do not swallow the outline.
      for (const auto [dx, dy] : {std::pair{-ICON_SCALE, 0}, {ICON_SCALE, 0}, {0, -ICON_SCALE}, {0, ICON_SCALE}}) {
        drawIconScaled(renderer, item.icon, ix + dx, iy + dy, ICON_SCALE, false, false);
      }
    }
    drawIconScaled(renderer, item.icon, ix, iy, ICON_SCALE, true, true);

    // Label drawn straight on the desktop; the selected one sits on a black
    // box. Wrapped to two lines like Win98.
    const bool sel = i == selectedIndex;
    const int font = DESKTOP_FONT;
    const int lineH = renderer.getLineHeight(font);
    const auto lines = renderer.wrappedText(font, item.label.c_str(), c.w - 2 * LABEL_PAD - 2, 2);
    int ly = c.y + 4 + iconSize + 4;
    for (const std::string& line : lines) {
      const int tw = renderer.getTextWidth(font, line.c_str());
      const int bw = tw + 2 * LABEL_PAD;
      const int bx = c.x + (c.w - bw) / 2;
      // On a wallpaper every label gets a box so it stays readable.
      if (sel || hasWallpaper) renderer.fillRect(bx, ly, bw, lineH, sel);
      renderer.drawText(font, bx + LABEL_PAD, ly, line.c_str(), !sel);
      ly += lineH;
    }
  }
}

void Win98DesktopActivity::drawTaskbar(const bool startPressed) {
  const int top = taskbarTop();
  const int width = renderer.getScreenWidth();
  renderer.fillRectDither(0, top, width, TASKBAR_H, Color::LightGray);
  renderer.drawLine(0, top, width - 1, top, true);
  renderer.drawLine(0, top + 1, width - 1, top + 1, false);

  // Start button: white face, raised; sunken while the menu is open.
  const Box s = startButton();
  renderer.fillRect(s.x, s.y, s.w, s.h, false);
  renderer.drawRect(s.x, s.y, s.w, s.h, true);
  if (startPressed) {
    win98::drawSunken(renderer, s.x + 1, s.y + 1, s.w - 2, s.h - 2);
  } else {
    win98::drawRaised(renderer, s.x + 1, s.y + 1, s.w - 2, s.h - 2);
  }
  const int press = startPressed ? 1 : 0;
  drawLogo(renderer, s.x + 10 + press, s.y + (s.h - 18) / 2 + press);
  const char* start = tr(STR_W98_START);
  const int startFont = DESKTOP_FONT;
  const int textH = renderer.getLineHeight(startFont);
  renderer.drawText(startFont, s.x + 36 + press, s.y + (s.h - textH) / 2 + press, start, true);

  // Tray: battery and clock in a sunken white box.
  char clock[sizeof(shownClock)] = {};
  formatClock(clock, sizeof(clock));
  memcpy(shownClock, clock, sizeof(shownClock));
  char tray[32];
  snprintf(tray, sizeof(tray), clock[0] ? "%u%%   %s" : "%u%%", powerManager.getBatteryPercentage(), clock);
  const int trayTextW = renderer.getTextWidth(DESKTOP_FONT, tray);
  const int trayW = trayTextW + 24;
  const int trayX = width - 4 - trayW;
  renderer.fillRect(trayX, s.y, trayW, s.h, false);
  win98::drawSunken(renderer, trayX, s.y, trayW, s.h);
  const int lineH = renderer.getLineHeight(DESKTOP_FONT);
  renderer.drawText(DESKTOP_FONT, trayX + 12, s.y + (s.h - lineH) / 2, tray, true);
}

void Win98DesktopActivity::drawStartMenu(const int selectedIndex) const {
  const Box m = startMenu();
  renderer.fillRect(m.x, m.y, m.w, m.h, false);
  renderer.drawRect(m.x, m.y, m.w, m.h, true);
  win98::drawRaised(renderer, m.x + 1, m.y + 1, m.w - 2, m.h - 2);

  // Side banner; the rotated text starts at y and runs upward.
  const int bx = m.x + MENU_BORDER;
  const int by = m.y + MENU_BORDER;
  const int bh = m.h - 2 * MENU_BORDER;
  renderer.fillRect(bx, by, MENU_BANNER_W, bh, true);
  const char* banner = tr(STR_W98_BANNER);
  renderer.drawTextRotated90CW(DESKTOP_FONT, bx + 4, by + bh - 8, banner, false);

  // Separator above Shut Down: a dark line over a white one.
  const Box last = menuRow(static_cast<int>(menuItems.size()) - 1);
  const int sepY = last.y - MENU_SEPARATOR_H / 2 - 1;
  renderer.drawLine(last.x + 4, sepY, last.x + last.w - 5, sepY, true);
  renderer.drawLine(last.x + 4, sepY + 1, last.x + last.w - 5, sepY + 1, false);

  for (int i = 0; i < static_cast<int>(menuItems.size()); ++i) {
    const Box row = menuRow(i);
    const bool sel = i == selectedIndex;
    if (sel) renderer.fillRect(row.x, row.y, row.w, row.h, true);
    if (menuItems[i].icon) {
      drawIconScaled(renderer, menuItems[i].icon, row.x + 8, row.y + (row.h - ICON) / 2, 1, !sel, !sel);
    }
    const int textX = row.x + 8 + ICON + 12;
    const int font = DESKTOP_FONT;
    const int lineH = renderer.getLineHeight(font);
    const std::string label = renderer.truncatedText(font, menuItems[i].label.c_str(), row.x + row.w - 8 - textX);
    renderer.drawText(font, textX, row.y + (row.h - lineH) / 2, label.c_str(), !sel);
  }
}
