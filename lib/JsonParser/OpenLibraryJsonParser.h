#pragma once

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>

// Collects cover ids from Open Library JSON as it streams in:
// - search.json: "cover_i" of each doc, and the first "/works/..." key;
// - works/<id>/editions.json: the first id of each entry's "covers" array.
// Fixed-size storage; ids past MAX_COVERS and repeated ids are dropped.
class OpenLibraryJsonParser {
 public:
  static constexpr size_t MAX_COVERS = 12;
  static constexpr size_t MAX_WORK_KEY = 40;

  OpenLibraryJsonParser();

  OpenLibraryJsonParser(const OpenLibraryJsonParser&) = delete;
  OpenLibraryJsonParser& operator=(const OpenLibraryJsonParser&) = delete;

  // Forgets the parse state but keeps the ids, so a second response adds to them.
  void beginResponse();
  void feed(const char* data, size_t len);
  bool hasError() const { return parser.hasError(); }

  size_t coverCount() const { return count; }
  uint32_t coverAt(size_t index) const { return index < count ? covers[index] : 0; }
  // First work key seen ("/works/OL123W"), or empty.
  const char* workKey() const { return work; }

 private:
  enum class LastKey : uint8_t { NONE, COVER_I, COVERS, KEY };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnValue(void* ctx);
  static void sOnBool(void* ctx, bool value);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void addCover(const char* value, size_t len);

  StreamingJsonParser parser;
  LastKey lastKey = LastKey::NONE;
  // Inside a "covers" array (depth counts nested arrays), and whether its
  // first id was taken.
  uint8_t coversDepth = 0;
  bool coversTaken = false;

  uint32_t covers[MAX_COVERS] = {};
  size_t count = 0;
  char work[MAX_WORK_KEY] = {};
};
