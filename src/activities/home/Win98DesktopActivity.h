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
                                HomeMenuItem initialMenuItem = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItem) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  void prepareForSleep() override { sleeping = true; }
  bool isHomeActivity() const override { return true; }

 private:
  enum class Action : uint8_t { Resume, Files, Library, Notepad, Transfer, Settings, Plugins, Opds, ShutDown };

  struct Item {
    Action action;
    const uint8_t* icon;  // null: no icon
    std::string label;
  };

  struct Box {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };

  const HomeMenuItem initialMenuItem;
  bool firstRenderDone = false;
  bool menuWasOpen = false;  // render-side: the last frame showed the Start menu
  bool sleeping = false;     // set by prepareForSleep; onExit then skips the refresh promotion

  // Built in onEnter, read-only afterwards.
  std::vector<Item> desktopItems;  // grid order; the recent book comes last
  std::vector<Item> menuItems;     // Start menu: desktop items without the book, then Shut Down
  std::string resumePath;          // most recent book, empty if none

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
  void activate(Action action);
  void checkClock();

  void drawDesktop(int selectedIndex) const;
  void drawTaskbar(bool startPressed);
  void drawStartMenu(int selectedIndex) const;
};
