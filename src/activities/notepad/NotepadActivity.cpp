#include "NotepadActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/HeaderBackTapTarget.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* NOTES_DIR = "/notes";
constexpr const char* TMP_SUFFIX = ".tmp";

constexpr int TITLE_BAR_H = 30;
constexpr int MENU_BAR_H = 28;
constexpr int STATUS_BAR_H = 26;
constexpr int SCROLLBAR_W = 24;
constexpr int BORDER = 4;  // outer window frame: 2 px bevel + 2 px face
constexpr int TEXT_PAD = 6;
constexpr int MENU_ROW_H = 34;
constexpr int MENU_W = 190;
constexpr int SEPARATOR_H = 10;
// Longest run measured as one piece. No 256-byte run fits a line, so longer
// unbroken text only needs the hard-cut search.
constexpr size_t MAX_MEASURE_BYTES = 256;

bool hasTxtExtension(const char* name) {
  const size_t len = strlen(name);
  return len > 4 && strcasecmp(name + len - 4, ".txt") == 0;
}

bool isUtf8Continuation(const char c) { return (static_cast<uint8_t>(c) & 0xC0) == 0x80; }

// Keeps a typed file name to one safe path component.
std::string sanitizeFileName(const std::string& raw) {
  std::string name;
  name.reserve(raw.size() + 4);
  for (const char c : raw) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      name.push_back('_');
    } else if (static_cast<uint8_t>(c) >= 0x20) {
      name.push_back(c);
    }
  }
  while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
  while (!name.empty() && (name.front() == ' ' || name.front() == '.')) name.erase(name.begin());
  if (name.empty()) return name;
  if (!hasTxtExtension(name.c_str())) name += ".txt";
  return name;
}

// Win98 raised edge: white top-left, black bottom-right, dark grey inner shadow.
void drawRaised(const GfxRenderer& r, int x, int y, int w, int h) {
  r.drawLine(x, y, x + w - 2, y, false);
  r.drawLine(x, y, x, y + h - 2, false);
  r.drawLine(x, y + h - 1, x + w - 1, y + h - 1, true);
  r.drawLine(x + w - 1, y, x + w - 1, y + h - 1, true);
  r.fillRectDither(x + 1, y + h - 2, w - 2, 1, Color::DarkGray);
  r.fillRectDither(x + w - 2, y + 1, 1, h - 2, Color::DarkGray);
}

// Sunken edge: the raised edge mirrored.
void drawSunken(const GfxRenderer& r, int x, int y, int w, int h) {
  r.fillRectDither(x, y, w - 1, 1, Color::DarkGray);
  r.fillRectDither(x, y, 1, h - 1, Color::DarkGray);
  r.drawLine(x + 1, y + 1, x + w - 3, y + 1, true);
  r.drawLine(x + 1, y + 1, x + 1, y + h - 3, true);
  r.drawLine(x, y + h - 1, x + w - 1, y + h - 1, false);
  r.drawLine(x + w - 1, y, x + w - 1, y + h - 1, false);
}

// Button with a raised edge. The face stays white so the thumb and arrows
// stand out from the grey scrollbar track on e-ink.
void drawButton(const GfxRenderer& r, int x, int y, int w, int h) {
  r.fillRect(x, y, w, h, false);
  r.drawRect(x, y, w, h, true);
  drawRaised(r, x + 1, y + 1, w - 2, h - 2);
}

// Small solid triangle pointing up or down, centred in a box.
void drawArrow(const GfxRenderer& r, int cx, int cy, bool up) {
  for (int i = 0; i < 5; ++i) {
    const int half = up ? i : 4 - i;
    r.drawLine(cx - half, cy - 2 + i, cx + half, cy - 2 + i, true);
  }
}

const char* menuLabel(const int index) {
  static constexpr StrId labels[] = {StrId::STR_NOTEPAD_NEW, StrId::STR_NOTEPAD_OPEN, StrId::STR_NOTEPAD_SAVE,
                                     StrId::STR_NOTEPAD_SAVE_AS, StrId::STR_NOTEPAD_EXIT};
  return I18N.get(labels[index]);
}

}  // namespace

void NotepadActivity::onEnter() {
  Activity::onEnter();
  Storage.mkdir(NOTES_DIR);
  RenderLock lock;
  path.clear();
  dirty = false;
  firstRender = true;
  topLine = 0;
  followEnd = true;
  menuOpen = false;
  statusMessage.clear();
  // Both buffers are sized once for the screen's lifetime, so typing and
  // opening never grow them (no doubling spike on the C3's heap). On the
  // X4 Pro these large blocks land in PSRAM.
  const size_t needed = MAX_FILE_BYTES + 8 + MAX_LINES * sizeof(uint16_t) + 1024;
  buffersReady = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= needed;
  if (buffersReady) {
    text.reserve(MAX_FILE_BYTES + 8);
    lineStarts.reserve(MAX_LINES);
    rewrapToLayout();
  } else {
    LOG_ERR("NOTE", "OOM: need %u bytes", static_cast<unsigned>(needed));
    const char* options[] = {tr(STR_OK_BUTTON)};
    popup.showMessage(tr(STR_NOTEPAD_TITLE), tr(STR_NOTEPAD_NO_MEMORY), options, 1, 0,
                      [](int) { activityManager.goHome(); });
  }
  requestUpdate();
}

void NotepadActivity::onExit() {
  Activity::onExit();
  popup.dismiss();
  // Leaving by any route (including Home while the keyboard is open) keeps
  // typed lines; "No" on the save prompt clears `dirty` first.
  autosave();
  std::string().swap(text);
  std::vector<uint16_t>().swap(lineStarts);
  std::vector<std::string>().swap(openChoices);
}

void NotepadActivity::prepareForSleep() { autosave(); }

bool NotepadActivity::handleHomeGesture() {
  RenderLock lock;
  if (!dirty) return false;
  menuOpen = false;
  confirmDiscard(AfterSave::Exit);
  return true;
}

void NotepadActivity::autosave() {
  if (!dirty) return;
  std::string target = path;
  if (target.empty()) {
    // An Untitled note gets the first free untitled-N.txt name.
    char name[48];
    for (int i = 1; i < 1000 && target.empty(); ++i) {
      snprintf(name, sizeof(name), "%s/untitled-%d.txt", NOTES_DIR, i);
      if (!Storage.exists(name)) target = name;
    }
  }
  if (!target.empty() && writeFile(target)) {
    path = target;
    dirty = false;
  }
}

NotepadActivity::Layout NotepadActivity::layout() const {
  Layout l;
  const Rect content = UITheme::getContentArea(renderer);
  l.window = {content.x, content.y, content.width, content.height};

  const int inner = l.window.x + BORDER;
  const int innerW = l.window.w - 2 * BORDER;
  l.titleBar = {inner, l.window.y + BORDER, innerW, TITLE_BAR_H};
  l.closeButton = {l.titleBar.x + l.titleBar.w - TITLE_BAR_H + 2, l.titleBar.y + 3, TITLE_BAR_H - 6, TITLE_BAR_H - 7};
  l.menuBar = {inner, l.titleBar.y + l.titleBar.h + 1, innerW, MENU_BAR_H};
  const int fileW = renderer.getTextWidth(UI_12_FONT_ID, tr(STR_NOTEPAD_FILE)) + 16;
  l.fileMenuLabel = {l.menuBar.x + 2, l.menuBar.y + 2, fileW, l.menuBar.h - 4};

  l.statusBar = {inner, l.window.y + l.window.h - BORDER - STATUS_BAR_H, innerW, STATUS_BAR_H};
  const int textTop = l.menuBar.y + l.menuBar.h + 2;
  l.textBox = {inner, textTop, innerW, l.statusBar.y - 2 - textTop};
  l.scrollBar = {l.textBox.x + l.textBox.w - 2 - SCROLLBAR_W, l.textBox.y + 2, SCROLLBAR_W, l.textBox.h - 4};
  l.scrollUp = {l.scrollBar.x, l.scrollBar.y, SCROLLBAR_W, SCROLLBAR_W};
  l.scrollDown = {l.scrollBar.x, l.scrollBar.y + l.scrollBar.h - SCROLLBAR_W, SCROLLBAR_W, SCROLLBAR_W};
  l.textArea = {l.textBox.x + 2 + TEXT_PAD, l.textBox.y + 2 + TEXT_PAD / 2,
                l.scrollBar.x - (l.textBox.x + 2) - 2 * TEXT_PAD, l.textBox.h - 4 - TEXT_PAD};

  l.lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  l.visibleLines = std::max(1, l.textArea.h / std::max(1, l.lineHeight));

  const int menuH = MENU_ITEM_COUNT * MENU_ROW_H + SEPARATOR_H + 6;
  l.dropdown = {l.fileMenuLabel.x, l.menuBar.y + l.menuBar.h - 1, MENU_W, menuH};
  return l;
}

int NotepadActivity::measure(const size_t start, const size_t end) {
  if (end <= start) return 0;
  // Measuring needs a terminated copy; the scratch buffer keeps its capacity.
  scratch.assign(text, start, end - start);
  return renderer.getTextAdvanceX(UI_12_FONT_ID, scratch.c_str(), EpdFontFamily::REGULAR);
}

size_t NotepadActivity::codePointStart(size_t pos) const {
  while (pos > 0 && pos < text.size() && isUtf8Continuation(text[pos])) --pos;
  return pos;
}

// End of the wrapped line starting at `start`: after the last word that
// fits, or a hard cut for a word wider than the line. Words are measured once
// each and at most MAX_MEASURE_BYTES at a time (kerning across spaces is
// ignored), so wrapping stays linear in the note's length.
size_t NotepadActivity::nextBreak(const size_t start, const int maxWidth) {
  size_t lineEnd = text.find('\n', start);
  if (lineEnd == std::string::npos) lineEnd = text.size();
  const int spaceWidth = renderer.getTextAdvanceX(UI_12_FONT_ID, " ", EpdFontFamily::REGULAR);

  int used = 0;
  size_t pos = start;
  while (pos < lineEnd) {
    size_t wordEnd = text.find(' ', pos);
    if (wordEnd == std::string::npos || wordEnd > lineEnd) wordEnd = lineEnd;
    if (wordEnd - pos > MAX_MEASURE_BYTES) wordEnd = codePointStart(pos + MAX_MEASURE_BYTES);
    const int wordWidth = measure(pos, wordEnd);
    if (used + wordWidth > maxWidth) {
      if (pos > start) return pos;
      // One word wider than the line: binary-search the last code point that
      // fits (prefix width only grows).
      size_t lo = start;    // known to fit
      size_t hi = wordEnd;  // known not to fit
      while (hi - lo > 1) {
        size_t mid = codePointStart(lo + (hi - lo) / 2);
        if (mid <= lo) mid = lo + 1;
        while (mid < hi && isUtf8Continuation(text[mid])) ++mid;
        if (mid >= hi) break;
        if (measure(start, mid) <= maxWidth) {
          lo = mid;
        } else {
          hi = mid;
        }
      }
      if (lo > start) return lo;
      // A single glyph wider than the line: take it anyway so wrapping advances.
      size_t next = start + 1;
      while (next < lineEnd && isUtf8Continuation(text[next])) ++next;
      return next;
    }
    used += wordWidth;
    pos = wordEnd;
    // Trailing spaces stay on this line even past the edge; they draw nothing.
    while (pos < lineEnd && text[pos] == ' ') {
      used += spaceWidth;
      ++pos;
    }
  }
  return lineEnd;
}

bool NotepadActivity::rewrap(const int width) {
  lineStarts.clear();
  size_t pos = 0;
  lineStarts.push_back(0);
  while (pos < text.size()) {
    const size_t end = nextBreak(pos, width);
    if (end < text.size() && text[end] == '\n') {
      pos = end + 1;
    } else {
      pos = end;
      if (pos >= text.size()) break;
    }
    if (lineStarts.size() >= MAX_LINES) return false;
    lineStarts.push_back(static_cast<uint16_t>(pos));
  }
  return true;
}

void NotepadActivity::clampScroll(const Layout& l) {
  const int total = static_cast<int>(lineStarts.size());
  const int maxTop = std::max(0, total - l.visibleLines);
  if (followEnd) topLine = maxTop;
  topLine = std::max(0, std::min(topLine, maxTop));
}

void NotepadActivity::scrollBy(const int lines) {
  const Layout l = layout();
  const int total = static_cast<int>(lineStarts.size());
  const int maxTop = std::max(0, total - l.visibleLines);
  const int next = std::max(0, std::min(topLine + lines, maxTop));
  followEnd = next >= maxTop;
  if (next != topLine) {
    topLine = next;
    requestUpdate();
  }
}

std::string NotepadActivity::displayName() const {
  if (path.empty()) return tr(STR_NOTEPAD_UNTITLED);
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void NotepadActivity::loop() {
  // render() reads the note on the render task; every change below happens
  // under the same lock. Nothing here waits for a render.
  RenderLock lock;
  if (popup.isActive()) {
    popup.handleInput(mappedInput, [this] { requestUpdate(); });
    return;
  }
  if (!buffersReady) {
    // The out-of-memory message was dismissed without OK.
    activityManager.goHome();
    return;
  }

  const Layout l = layout();
  int tx = 0;
  int ty = 0;
  const bool tapped = mappedInput.wasScreenTapped(tx, ty);

  if (menuOpen) {
    if (tapped) {
      haptic_feedback::touchAction();
      const int itemTop = l.dropdown.y + 3;
      if (l.dropdown.contains(tx, ty)) {
        int row = (ty - itemTop) / MENU_ROW_H;
        // Rows after Save As sit below the separator.
        if (ty - itemTop >= 4 * MENU_ROW_H) row = ty - itemTop >= 4 * MENU_ROW_H + SEPARATOR_H ? 4 : -1;
        if (row >= 0 && row < MENU_ITEM_COUNT) {
          menuOpen = false;
          activateMenuItem(static_cast<MenuItem>(row));
        }
        return;
      }
      menuOpen = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      menuSelection = (menuSelection + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      menuSelection = (menuSelection + 1) % MENU_ITEM_COUNT;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      menuOpen = false;
      activateMenuItem(static_cast<MenuItem>(menuSelection));
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      menuOpen = false;
      requestUpdate();
    }
    return;
  }

  if (tapped) {
    // The drawn [x] is small; accept taps on the whole title-bar corner.
    const Box closeTarget{l.closeButton.x - 16, l.window.y, l.window.x + l.window.w - (l.closeButton.x - 16),
                          l.titleBar.y + l.titleBar.h + 8 - l.window.y};
    if (closeTarget.contains(tx, ty)) {
      haptic_feedback::touchAction();
      exitNotepad();
    } else if (l.fileMenuLabel.contains(tx, ty)) {
      haptic_feedback::touchAction();
      menuOpen = true;
      menuSelection = 0;
      requestUpdate();
    } else if (l.scrollUp.contains(tx, ty)) {
      scrollBy(-std::max(1, l.visibleLines - 1));
    } else if (l.scrollDown.contains(tx, ty)) {
      scrollBy(std::max(1, l.visibleLines - 1));
    } else if (l.scrollBar.contains(tx, ty)) {
      // Track taps page toward the tapped half.
      const int page = std::max(1, l.visibleLines - 1);
      scrollBy(ty < l.scrollBar.y + l.scrollBar.h / 2 ? -page : page);
    } else if (l.textBox.contains(tx, ty)) {
      haptic_feedback::touchAction();
      startTyping();
    }
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    scrollBy(std::max(1, l.visibleLines - 1));
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    scrollBy(-std::max(1, l.visibleLines - 1));
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    scrollBy(-std::max(1, l.visibleLines - 1));
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    scrollBy(std::max(1, l.visibleLines - 1));
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startTyping();
  } else if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, 600)) {
    menuOpen = true;
    menuSelection = 0;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    exitNotepad();
  }
}

void NotepadActivity::activateMenuItem(const MenuItem item) {
  statusMessage.clear();
  switch (item) {
    case MenuItem::New:
      if (dirty) {
        confirmDiscard(AfterSave::New);
      } else {
        newFile();
      }
      break;
    case MenuItem::Open:
      if (dirty) {
        confirmDiscard(AfterSave::Open);
      } else {
        showOpenList();
      }
      break;
    case MenuItem::Save:
      saveOrPromptName(AfterSave::Stay);
      break;
    case MenuItem::SaveAs:
      promptFileName(AfterSave::Stay);
      break;
    case MenuItem::Exit:
      exitNotepad();
      break;
  }
  requestUpdate();
}

void NotepadActivity::startTyping() {
  // Room for the line plus up to two CRLF line endings.
  constexpr size_t EOL_RESERVE = 4;
  if (text.size() + EOL_RESERVE >= MAX_FILE_BYTES) {
    showMessage(tr(STR_NOTEPAD_TITLE), tr(STR_NOTEPAD_TOO_LARGE));
    return;
  }
  const size_t room = std::min(MAX_LINE_BYTES, MAX_FILE_BYTES - text.size() - EOL_RESERVE);
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_NOTEPAD_TYPE_LINE), "",
                                                                 room, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           RenderLock lock;
                           appendLine(std::get<KeyboardResult>(result.data).text);
                         });
}

void NotepadActivity::appendLine(const std::string& line) {
  if (line.empty()) return;
  // Match the file's existing line endings (Windows notes use CRLF).
  const char* eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  const size_t oldSize = text.size();
  if (!text.empty() && text.back() != '\n') text += eol;
  text += line;
  text += eol;
  followEnd = true;
  if (text.size() > MAX_FILE_BYTES || !rewrapToLayout()) {
    text.resize(oldSize);
    rewrapToLayout();
    showMessage(tr(STR_NOTEPAD_TITLE), tr(STR_NOTEPAD_TOO_LARGE));
    return;
  }
  dirty = true;
}

bool NotepadActivity::rewrapToLayout() {
  const Layout l = layout();
  const bool ok = rewrap(l.textArea.w);
  clampScroll(l);
  return ok;
}

void NotepadActivity::confirmDiscard(const AfterSave next) {
  const char* options[] = {tr(STR_YES), tr(STR_NO), tr(STR_CANCEL)};
  char question[160];
  snprintf(question, sizeof(question), tr(STR_NOTEPAD_SAVE_CHANGES), displayName().c_str());
  popup.showMessage(tr(STR_NOTEPAD_TITLE), question, options, 3, 0, [this, next](const int choice) {
    if (choice == 0) {
      saveOrPromptName(next);
    } else if (choice == 1) {
      dirty = false;
      runAfterSave(next);
    }
  });
  requestUpdate();
}

void NotepadActivity::runAfterSave(const AfterSave next) {
  switch (next) {
    case AfterSave::Stay:
      break;
    case AfterSave::New:
      newFile();
      break;
    case AfterSave::Open:
      showOpenList();
      break;
    case AfterSave::Exit:
      exitNotepad();
      break;
  }
  requestUpdate();
}

void NotepadActivity::saveOrPromptName(const AfterSave next) {
  if (path.empty()) {
    promptFileName(next);
  } else {
    saveTo(path, next);
  }
}

void NotepadActivity::promptFileName(const AfterSave next) {
  std::string initial = displayName();
  if (path.empty()) initial.clear();
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_NOTEPAD_FILE_NAME),
                                                                 initial, 64, InputType::Text),
                         [this, next](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           RenderLock lock;
                           const std::string name = sanitizeFileName(std::get<KeyboardResult>(result.data).text);
                           if (name.empty()) return;
                           const std::string target = std::string(NOTES_DIR) + "/" + name;
                           if (target != path && Storage.exists(target.c_str())) {
                             const char* options[] = {tr(STR_YES), tr(STR_NO)};
                             char question[160];
                             snprintf(question, sizeof(question), tr(STR_NOTEPAD_REPLACE), name.c_str());
                             popup.showMessage(tr(STR_NOTEPAD_SAVE_AS), question, options, 2, 1,
                                               [this, target, next](const int choice) {
                                                 if (choice == 0) saveTo(target, next);
                                               });
                             return;
                           }
                           saveTo(target, next);
                         });
}

void NotepadActivity::saveTo(const std::string& newPath, const AfterSave next) {
  if (!writeFile(newPath)) {
    showMessage(tr(STR_NOTEPAD_TITLE), tr(STR_NOTEPAD_SAVE_FAILED));
    return;
  }
  path = newPath;
  dirty = false;
  statusMessage = tr(STR_NOTEPAD_SAVED);
  runAfterSave(next);
}

bool NotepadActivity::writeFile(const std::string& target) {
  // Write a temp file and swap it in, so a failed write keeps the old note.
  const std::string tmp = target + TMP_SUFFIX;
  {
    HalFile file;
    if (!Storage.openFileForWrite("NOTE", tmp, file)) return false;
    const size_t written = text.empty() ? 0 : file.write(text.data(), text.size());
    file.flush();
    const bool ok = written == text.size();
    file.close();
    if (!ok) {
      LOG_ERR("NOTE", "Short write to %s", tmp.c_str());
      Storage.remove(tmp.c_str());
      return false;
    }
  }
  if (!Storage.replaceFile(tmp.c_str(), target.c_str())) {
    LOG_ERR("NOTE", "Cannot replace %s", target.c_str());
    Storage.remove(tmp.c_str());
    return false;
  }
  LOG_INF("NOTE", "Saved %u bytes to %s", static_cast<unsigned>(text.size()), target.c_str());
  return true;
}

void NotepadActivity::showOpenList() {
  // Keep the MAX_OPEN_FILES most recently changed notes; the popup holds no more.
  struct Entry {
    uint32_t modified;
    std::string name;
  };
  std::vector<Entry> newest;
  newest.reserve(MAX_OPEN_FILES + 1);
  HalFile dir = Storage.open(NOTES_DIR);
  if (dir && dir.isDirectory()) {
    char name[128];
    while (HalFile entry = dir.openNextFile()) {
      if (entry.isDirectory()) continue;
      const size_t len = entry.getName(name, sizeof(name));
      if (len == 0 || len >= sizeof(name) || name[0] == '.' || !hasTxtExtension(name)) continue;
      newest.push_back({entry.modificationTime(), name});
      std::sort(newest.begin(), newest.end(), [](const Entry& a, const Entry& b) {
        return a.modified != b.modified ? a.modified > b.modified : strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
      });
      if (static_cast<int>(newest.size()) > MAX_OPEN_FILES) newest.pop_back();
    }
  }
  openChoices.clear();
  for (auto& entry : newest) openChoices.push_back(std::move(entry.name));
  if (openChoices.empty()) {
    showMessage(tr(STR_NOTEPAD_OPEN), tr(STR_NOTEPAD_NO_FILES));
    return;
  }
  std::vector<const char*> labels;
  labels.reserve(openChoices.size());
  for (const auto& choice : openChoices) labels.push_back(choice.c_str());
  popup.show(tr(STR_NOTEPAD_OPEN), labels.data(), static_cast<int>(labels.size()), 0, [this](const int choice) {
    if (choice >= 0 && choice < static_cast<int>(openChoices.size())) {
      openFile(std::string(NOTES_DIR) + "/" + openChoices[choice]);
    }
  });
  requestUpdate();
}

void NotepadActivity::openFile(const std::string& target) {
  HalFile file;
  if (!Storage.openFileForRead("NOTE", target, file)) {
    showMessage(tr(STR_NOTEPAD_OPEN), tr(STR_NOTEPAD_OPEN_FAILED));
    return;
  }
  const size_t size = file.fileSize();
  if (size > MAX_FILE_BYTES) {
    showMessage(tr(STR_NOTEPAD_OPEN), tr(STR_NOTEPAD_TOO_LARGE));
    return;
  }
  // Read straight into the reserved buffer; resize stays within capacity.
  text.resize(size);
  if (size > 0 && file.read(text.data(), size) != static_cast<int>(size)) {
    newFile();
    showMessage(tr(STR_NOTEPAD_OPEN), tr(STR_NOTEPAD_OPEN_FAILED));
    return;
  }
  file.close();
  path = target;
  dirty = false;
  followEnd = true;
  statusMessage.clear();
  if (!rewrapToLayout()) {
    newFile();
    showMessage(tr(STR_NOTEPAD_OPEN), tr(STR_NOTEPAD_TOO_LARGE));
    return;
  }
  requestUpdate();
}

void NotepadActivity::newFile() {
  text.clear();
  path.clear();
  dirty = false;
  followEnd = true;
  topLine = 0;
  rewrapToLayout();
  requestUpdate();
}

void NotepadActivity::exitNotepad() {
  if (dirty) {
    confirmDiscard(AfterSave::Exit);
    return;
  }
  activityManager.goHome();
}

void NotepadActivity::showMessage(const char* title, const std::string& message) {
  const char* options[] = {tr(STR_OK_BUTTON)};
  popup.showMessage(title, message.c_str(), options, 1, 0, [](int) {});
  requestUpdate();
}

void NotepadActivity::drawWindow(const Layout& l) const {
  // Window face and frame.
  renderer.fillRectDither(l.window.x, l.window.y, l.window.w, l.window.h, Color::LightGray);
  drawRaised(renderer, l.window.x, l.window.y, l.window.w, l.window.h);
  renderer.drawRect(l.window.x + 1, l.window.y + 1, l.window.w - 3, l.window.h - 3, false);

  // Active title bar: solid black stands in for Win98's navy gradient.
  renderer.fillRect(l.titleBar.x, l.titleBar.y, l.titleBar.w, l.titleBar.h, true);
  const std::string title = displayName() + " - " + tr(STR_NOTEPAD_TITLE);
  const int titleY = l.titleBar.y + (l.titleBar.h - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  const int maxTitleW = l.closeButton.x - l.titleBar.x - 12;
  const std::string fitted = renderer.truncatedText(UI_12_FONT_ID, title.c_str(), maxTitleW, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, l.titleBar.x + 6, titleY, fitted.c_str(), false, EpdFontFamily::BOLD);

  // [x] close button.
  const Box& c = l.closeButton;
  drawButton(renderer, c.x, c.y, c.w, c.h);
  const int cx = c.x + c.w / 2 - 1;
  const int cy = c.y + c.h / 2 - 1;
  for (int d = -4; d <= 4; ++d) {
    renderer.drawPixel(cx + d, cy + d);
    renderer.drawPixel(cx + d + 1, cy + d);
    renderer.drawPixel(cx + d, cy - d);
    renderer.drawPixel(cx + d + 1, cy - d);
  }
}

void NotepadActivity::drawMenuBar(const Layout& l) const {
  // Text sits on white: grey dither behind glyphs is hard to read on e-ink.
  renderer.fillRect(l.menuBar.x, l.menuBar.y, l.menuBar.w, l.menuBar.h, false);
  const Box& f = l.fileMenuLabel;
  const int textY = f.y + (f.h - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  if (menuOpen) {
    // Open menu title is pressed in.
    drawSunken(renderer, f.x, f.y, f.w, f.h);
  }
  const char* label = tr(STR_NOTEPAD_FILE);
  renderer.drawText(UI_12_FONT_ID, f.x + 8, textY, label);
  // Underlined mnemonic on the first letter, like Alt+F.
  const char first[2] = {label[0], '\0'};
  const int underlineY = textY + renderer.getLineHeight(UI_12_FONT_ID) - 3;
  renderer.drawLine(f.x + 8, underlineY, f.x + 8 + renderer.getTextWidth(UI_12_FONT_ID, first) - 1, underlineY);
}

void NotepadActivity::drawText(const Layout& l) const {
  // Sunken white edit box.
  renderer.fillRect(l.textBox.x, l.textBox.y, l.textBox.w, l.textBox.h, false);
  drawSunken(renderer, l.textBox.x, l.textBox.y, l.textBox.w, l.textBox.h);

  if (text.empty()) {
    renderer.fillRectDither(l.textArea.x, l.textArea.y, 2, l.lineHeight, Color::Black);
    renderer.drawText(UI_10_FONT_ID, l.textArea.x + 8, l.textArea.y + 2, tr(STR_NOTEPAD_TAP_TO_TYPE));
    return;
  }

  // One line is at most a screen width, so this copy stays small.
  std::string line;
  const int total = static_cast<int>(lineStarts.size());
  const int last = std::min(total, topLine + l.visibleLines);
  for (int i = topLine; i < last; ++i) {
    const size_t start = std::min<size_t>(lineStarts[i], text.size());
    size_t end = i + 1 < total ? std::min<size_t>(lineStarts[i + 1], text.size()) : text.size();
    while (end > start && (text[end - 1] == '\n' || text[end - 1] == '\r')) --end;
    line.assign(text, start, end - start);
    renderer.drawText(UI_12_FONT_ID, l.textArea.x, l.textArea.y + (i - topLine) * l.lineHeight, line.c_str());
  }
}

void NotepadActivity::drawScrollBar(const Layout& l) const {
  const Box& s = l.scrollBar;
  // Track: the classic 50% checkerboard.
  renderer.fillRect(s.x, s.y, s.w, s.h, false);
  renderer.fillRectDither(s.x, s.y, s.w, s.h, Color::LightGray);
  drawButton(renderer, l.scrollUp.x, l.scrollUp.y, l.scrollUp.w, l.scrollUp.h);
  drawArrow(renderer, l.scrollUp.x + l.scrollUp.w / 2, l.scrollUp.y + l.scrollUp.h / 2, true);
  drawButton(renderer, l.scrollDown.x, l.scrollDown.y, l.scrollDown.w, l.scrollDown.h);
  drawArrow(renderer, l.scrollDown.x + l.scrollDown.w / 2, l.scrollDown.y + l.scrollDown.h / 2, false);

  const int total = static_cast<int>(lineStarts.size());
  const int trackY = l.scrollUp.y + l.scrollUp.h;
  const int trackH = l.scrollDown.y - trackY;
  if (total <= l.visibleLines || trackH <= 8) return;
  const int thumbH = std::max(16, trackH * l.visibleLines / total);
  const int maxTop = total - l.visibleLines;
  const int thumbY = trackY + (trackH - thumbH) * topLine / std::max(1, maxTop);
  drawButton(renderer, s.x, thumbY, s.w, thumbH);
}

void NotepadActivity::drawStatusBar(const Layout& l) const {
  const Box& s = l.statusBar;
  renderer.fillRect(s.x, s.y, s.w, s.h, false);
  drawSunken(renderer, s.x, s.y, s.w, s.h);
  std::string status = statusMessage;
  if (status.empty()) status = path.empty() ? tr(STR_NOTEPAD_NOT_SAVED) : path;
  if (dirty) status = std::string(tr(STR_NOTEPAD_MODIFIED)) + "  |  " + status;
  const int y = s.y + (s.h - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  const std::string fitted = renderer.truncatedText(UI_10_FONT_ID, status.c_str(), s.w - 12);
  renderer.drawText(UI_10_FONT_ID, s.x + 6, y, fitted.c_str());
}

void NotepadActivity::drawDropdown(const Layout& l) const {
  const Box& d = l.dropdown;
  renderer.fillRect(d.x, d.y, d.w, d.h, false);
  drawRaised(renderer, d.x, d.y, d.w, d.h);

  int y = d.y + 3;
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  for (int i = 0; i < MENU_ITEM_COUNT; ++i) {
    if (i == 4) {
      // Separator before Exit: an etched groove.
      const int sy = y + SEPARATOR_H / 2 - 1;
      renderer.fillRectDither(d.x + 4, sy, d.w - 8, 1, Color::DarkGray);
      renderer.drawLine(d.x + 4, sy + 1, d.x + d.w - 5, sy + 1, false);
      y += SEPARATOR_H;
    }
    const bool selected = i == menuSelection;
    if (selected) renderer.fillRect(d.x + 3, y, d.w - 6, MENU_ROW_H, true);
    const char* label = menuLabel(i);
    const int textX = d.x + 22;
    const int textY = y + (MENU_ROW_H - lineH) / 2;
    renderer.drawText(UI_12_FONT_ID, textX, textY, label, !selected);
    const char first[2] = {label[0], '\0'};
    const int underlineY = textY + lineH - 3;
    renderer.drawLine(textX, underlineY, textX + renderer.getTextWidth(UI_12_FONT_ID, first) - 1, underlineY,
                      !selected);
    y += MENU_ROW_H;
  }
}

void NotepadActivity::render(RenderLock&&) {
  HeaderBackTapTarget::clear();
  renderer.clearScreen();
  const Layout l = layout();
  drawWindow(l);
  drawMenuBar(l);
  drawText(l);
  drawScrollBar(l);
  drawStatusBar(l);
  if (menuOpen) drawDropdown(l);

  if (popup.isActive()) {
    popup.render(renderer);
  }
  renderer.displayBuffer(firstRender ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstRender = false;
}
