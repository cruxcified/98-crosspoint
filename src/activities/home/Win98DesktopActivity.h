#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "util/ButtonNavigator.h"

// Win98-style home screen: desktop icons, a taskbar with Start button and
// clock, and a Start menu. Replaces HomeActivity as the home activity, so it
// keeps the "Home" name the activity manager checks for.
class Win98DesktopActivity final : public Activity {
 public:
  explicit Win98DesktopActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                HomeMenuItem initialMenuItem = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItem),
        cleanInitialRefresh(cleanInitialRefresh) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }

 private:
  enum class Action : uint8_t {
    Resume,
    Files,
    Library,
    CoverFinder,
    Notepad,
    Transfer,
    Settings,
    Plugins,
    Opds,
    ShutDown
  };

  struct Item {
    Action action;
    const uint8_t* icon;  // null: no icon
    std::string label;
    int book = -1;  // Resume: index into bookPaths
  };

  static constexpr int MAX_DESKTOP_BOOKS = 3;

  // Per-app Display settings; Settings and books always show.
  static bool shownOnDesktop(Action action);

  struct Box {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };

  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;
  bool firstRenderDone = false;

  // Built in onEnter, read-only afterwards.
  std::vector<Item> desktopItems;            // grid order: apps, then recent books (per SETTINGS.desktopIcons)
  std::vector<Item> menuItems;               // Start menu: every app, then Shut Down
  std::string bookPaths[MAX_DESKTOP_BOOKS];  // recent books, newest first
  int bookCount = 0;
  std::string wallpaperKey;  // empty: no wallpaper
  // Set in onEnter; render() clears it if the wallpaper fails to draw.
  bool hasWallpaper = false;

  // Written by loop(), read by render().
  std::atomic<int> selected{0};
  std::atomic<int> menuSelected{0};
  std::atomic<bool> menuOpen{false};

  // Clock text last drawn; render() writes it under the render lock.
  char shownClock[12] = {};
  unsigned long lastClockCheck = 0;

  ButtonNavigator buttonNavigator;

  int visibleIconCount() const;
  Box iconCell(int index) const;
  Box startButton() const;
  Box startMenu() const;
  Box menuRow(int index) const;
  int taskbarTop() const;

  void handleTap(int x, int y);
  void activate(Action action, int book = 0);
  void checkClock();

  void drawDesktop(int selectedIndex) const;
  void drawTaskbar(bool startPressed);
  void drawStartMenu(int selectedIndex) const;
};
