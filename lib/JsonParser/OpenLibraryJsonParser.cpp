#include "OpenLibraryJsonParser.h"

#include <cstring>

namespace {
bool keyIs(const char* key, const size_t len, const char* name) {
  return len == strlen(name) && memcmp(key, name, len) == 0;
}
}  // namespace

OpenLibraryJsonParser::OpenLibraryJsonParser()
    : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnValue, sOnValue, sOnValue, sOnArrayStart,
                           sOnArrayEnd}) {}

void OpenLibraryJsonParser::beginResponse() {
  parser.reset();
  lastKey = LastKey::NONE;
  coversDepth = 0;
  coversTaken = false;
}

void OpenLibraryJsonParser::feed(const char* data, const size_t len) { parser.feed(data, len); }

void OpenLibraryJsonParser::addCover(const char* value, const size_t len) {
  // Ids are positive integers; Open Library uses -1 for "no cover".
  uint32_t id = 0;
  for (size_t i = 0; i < len; i++) {
    if (value[i] < '0' || value[i] > '9' || id > 400000000u) return;
    id = id * 10 + static_cast<uint32_t>(value[i] - '0');
  }
  if (id == 0) return;
  for (size_t i = 0; i < count; i++) {
    if (covers[i] == id) return;
  }
  if (count < MAX_COVERS) covers[count++] = id;
}

void OpenLibraryJsonParser::sOnKey(void* ctx, const char* key, const size_t len) {
  auto* self = static_cast<OpenLibraryJsonParser*>(ctx);
  if (keyIs(key, len, "cover_i")) {
    self->lastKey = LastKey::COVER_I;
  } else if (keyIs(key, len, "covers")) {
    self->lastKey = LastKey::COVERS;
  } else if (keyIs(key, len, "key")) {
    self->lastKey = LastKey::KEY;
  } else {
    self->lastKey = LastKey::NONE;
  }
}

void OpenLibraryJsonParser::sOnString(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<OpenLibraryJsonParser*>(ctx);
  if (self->lastKey == LastKey::KEY && self->work[0] == '\0' && len < MAX_WORK_KEY && len > 7 &&
      memcmp(value, "/works/", 7) == 0) {
    memcpy(self->work, value, len);
    self->work[len] = '\0';
  }
  if (self->coversDepth == 0) self->lastKey = LastKey::NONE;
}

void OpenLibraryJsonParser::sOnNumber(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<OpenLibraryJsonParser*>(ctx);
  if (self->coversDepth == 1) {
    if (!self->coversTaken) {
      self->coversTaken = true;
      self->addCover(value, len);
    }
    return;
  }
  if (self->coversDepth == 0 && self->lastKey == LastKey::COVER_I) self->addCover(value, len);
  if (self->coversDepth == 0) self->lastKey = LastKey::NONE;
}

void OpenLibraryJsonParser::sOnBool(void* ctx, bool) { sOnValue(ctx); }

void OpenLibraryJsonParser::sOnValue(void* ctx) {
  auto* self = static_cast<OpenLibraryJsonParser*>(ctx);
  if (self->coversDepth == 0) self->lastKey = LastKey::NONE;
}

void OpenLibraryJsonParser::sOnArrayStart(void* ctx) {
  auto* self = static_cast<OpenLibraryJsonParser*>(ctx);
  if (self->coversDepth > 0) {
    self->coversDepth++;
  } else if (self->lastKey == LastKey::COVERS) {
    self->coversDepth = 1;
    self->coversTaken = false;
  }
  self->lastKey = LastKey::NONE;
}

void OpenLibraryJsonParser::sOnArrayEnd(void* ctx) {
  auto* self = static_cast<OpenLibraryJsonParser*>(ctx);
  if (self->coversDepth > 0) self->coversDepth--;
}
