#pragma once

#include <cstdint>
#include <string>

#include "activities/UiListActivity.h"

// Find Cover: looks the book up on Open Library over WiFi, shows up to six
// covers from its editions, and saves the chosen one as the book's custom
// cover (Epub::getCustomCoverPath). Every cover and thumb of the book is then
// made again from it.
//
// Memory: the candidate thumbs live on the SD card (/.crosspoint/cover_picker)
// and are painted from there; RAM holds six ids and one JSON parser (~2.2 KB)
// while a response streams in.
class CoverPickerActivity final : public UiListActivity {
 public:
  CoverPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool preventAutoSleep() override;

 private:
  static constexpr int COLUMNS = 3;
  static constexpr int ROWS = 2;
  static constexpr int MAX_CANDIDATES = COLUMNS * ROWS;

  enum class State : uint8_t { Wifi, Working, Picking, Message };

  int listCount() const override { return state == State::Picking ? candidateCount : 0; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  void onWifiConnected();
  // Search, then download and convert the candidates; ends in Picking or Message.
  void findCandidates();
  bool search(const std::string& title, const std::string& author);
  void saveChoice(int index);
  void showMessage(const char* message);
  // STR_COVER_FAILED plus the step that failed and its HTTP status, so a
  // failure on the device can be told apart without a serial log.
  void showFailure(const char* step, int httpStatus);
  void setStatus(const char* message);
  // Cover height that fits ROWS x COLUMNS on this screen (thumbs are made at it).
  int computeCoverHeight() const;
  std::string thumbPath(int index) const;
  void removePickerFiles() const;

  std::string bookPath;
  std::string title;
  std::string author;
  State state = State::Wifi;
  // Points at the i18n table or statusBuf; read by the render task.
  const char* status = nullptr;
  char statusBuf[128] = {};
  uint32_t candidates[MAX_CANDIDATES] = {};
  int candidateCount = 0;
  // Set by search() when it fails: the step and its HTTP status
  // (0 = no answer, -1 = not an HTTP step).
  const char* searchFailStep = nullptr;
  int searchFailStatus = 0;
  int coverHeight = 0;
  unsigned long workEndedMs = 0;
};
