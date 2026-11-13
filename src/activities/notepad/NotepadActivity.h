#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Win98-style Notepad: opens or creates a .txt file in /notes, appends lines
// typed on the on-screen keyboard, and saves. Drawn directly with GfxRenderer
// (title bar, File menu, sunken text box, scrollbar); touch hit tests use the
// same layout() as render().
class NotepadActivity final : public Activity {
 public:
  explicit NotepadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Notepad", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  void prepareForSleep() override;
  bool handleHomeGesture() override;

 private:
  struct Box {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };

  struct Layout {
    Box window;
    Box titleBar;
    Box closeButton;
    Box menuBar;
    Box fileMenuLabel;
    Box textBox;    // sunken frame, including the scrollbar
    Box textArea;   // where text lines are drawn
    Box scrollBar;  // track, including both arrow buttons
    Box scrollUp;
    Box scrollDown;
    Box statusBar;
    Box dropdown;
    int lineHeight = 0;
    int visibleLines = 0;
  };

  enum class MenuItem : uint8_t { New, Open, Save, SaveAs, Exit };
  static constexpr int MENU_ITEM_COUNT = 5;

  // What to do once a "save changes?" prompt or a save finishes.
  enum class AfterSave : uint8_t { Stay, New, Open, Exit };

  // Notes are held in RAM while open. 32 KB keeps 16-bit wrap offsets and
  // fits the C3's heap as well as the S3's.
  static constexpr size_t MAX_FILE_BYTES = 32 * 1024;
  static constexpr size_t MAX_LINES = 4096;  // wrapped lines; 8 KB of offsets
  static constexpr size_t MAX_LINE_BYTES = 512;
  static constexpr int MAX_OPEN_FILES = 16;  // OptionPopup's option limit

  std::string path;  // empty = Untitled
  std::string text;
  bool dirty = false;
  bool firstRender = true;
  bool buffersReady = false;

  // Wrapped line start offsets into `text`, rebuilt on the loop side whenever
  // the text changes; render() only reads them.
  std::vector<uint16_t> lineStarts;
  std::string scratch;  // reused by measure()
  int topLine = 0;
  bool followEnd = true;  // keep the last line in view after loads and appends

  bool menuOpen = false;
  int menuSelection = 0;

  OptionPopup popup;
  std::vector<std::string> openChoices;  // file names for the Open popup
  std::string statusMessage;

  Layout layout() const;
  bool rewrap(int width);  // false when the note needs more than MAX_LINES
  bool rewrapToLayout();
  int measure(size_t start, size_t end);
  size_t codePointStart(size_t pos) const;
  size_t nextBreak(size_t start, int maxWidth);
  void clampScroll(const Layout& l);
  void scrollBy(int lines);

  std::string displayName() const;
  void activateMenuItem(MenuItem item);
  void startTyping();
  void appendLine(const std::string& line);
  void autosave();

  void confirmDiscard(AfterSave next);
  void runAfterSave(AfterSave next);
  void saveOrPromptName(AfterSave next);
  void promptFileName(AfterSave next);
  void saveTo(const std::string& newPath, AfterSave next);
  bool writeFile(const std::string& target);
  void showOpenList();
  void openFile(const std::string& target);
  void newFile();
  void exitNotepad();
  void showMessage(const char* title, const std::string& message);

  void drawWindow(const Layout& l) const;
  void drawMenuBar(const Layout& l) const;
  void drawText(const Layout& l) const;
  void drawScrollBar(const Layout& l) const;
  void drawStatusBar(const Layout& l) const;
  void drawDropdown(const Layout& l) const;
};
