#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "lib/JsonParser/OpenLibraryJsonParser.h"

namespace {

const char* kSearch = R"({
  "numFound": 3, "start": 0, "numFoundExact": true,
  "docs": [
    {"key": "/works/OL27482W", "cover_i": 14627509, "first_publish_year": 1937},
    {"key": "/works/OL1W", "first_publish_year": 2001},
    {"key": "/works/OL2W", "cover_i": 555, "first_publish_year": 1999},
    {"key": "/works/OL3W", "cover_i": 14627509}
  ],
  "num_found": 3, "q": "", "offset": null
})";

const char* kEditions = R"({
  "links": {"self": "/works/OL27482W/editions.json", "work": "/works/OL27482W"},
  "size": 4,
  "entries": [
    {"type": {"key": "/type/edition"}, "title": "The Hobbit", "covers": [8406786, 111, 222],
     "works": [{"key": "/works/OL27482W"}], "key": "/books/OL1M", "publish_date": "1997"},
    {"type": {"key": "/type/edition"}, "title": "The Hobbit", "covers": [-1], "key": "/books/OL2M"},
    {"type": {"key": "/type/edition"}, "title": "The Hobbit", "key": "/books/OL3M",
     "identifiers": {"covers": "not an array"}, "number_of_pages": 310},
    {"type": {"key": "/type/edition"}, "covers": [555, 9], "key": "/books/OL4M", "latest_revision": 7}
  ]
})";

void feedInChunks(OpenLibraryJsonParser& parser, const char* json, size_t chunk) {
  const size_t len = strlen(json);
  for (size_t i = 0; i < len; i += chunk) parser.feed(json + i, std::min(chunk, len - i));
}

TEST(OpenLibraryJsonParser, SearchCollectsCoverIdsAndFirstWork) {
  OpenLibraryJsonParser parser;
  parser.beginResponse();
  feedInChunks(parser, kSearch, 7);
  ASSERT_FALSE(parser.hasError());
  EXPECT_STREQ(parser.workKey(), "/works/OL27482W");
  ASSERT_EQ(parser.coverCount(), 2u);
  EXPECT_EQ(parser.coverAt(0), 14627509u);
  EXPECT_EQ(parser.coverAt(1), 555u);
}

TEST(OpenLibraryJsonParser, EditionsTakeFirstCoverOfEachEntry) {
  OpenLibraryJsonParser parser;
  parser.beginResponse();
  feedInChunks(parser, kEditions, 13);
  ASSERT_FALSE(parser.hasError());
  ASSERT_EQ(parser.coverCount(), 2u);
  EXPECT_EQ(parser.coverAt(0), 8406786u);
  EXPECT_EQ(parser.coverAt(1), 555u);
  EXPECT_STREQ(parser.workKey(), "/works/OL27482W");
}

TEST(OpenLibraryJsonParser, SecondResponseAddsWithoutDuplicates) {
  OpenLibraryJsonParser parser;
  parser.beginResponse();
  feedInChunks(parser, kSearch, 64);
  parser.beginResponse();
  feedInChunks(parser, kEditions, 64);
  ASSERT_EQ(parser.coverCount(), 3u);
  EXPECT_EQ(parser.coverAt(0), 14627509u);
  EXPECT_EQ(parser.coverAt(1), 555u);
  EXPECT_EQ(parser.coverAt(2), 8406786u);
}

TEST(OpenLibraryJsonParser, StopsAtCapacity) {
  std::string json = R"({"docs":[)";
  for (int i = 1; i <= 30; i++) {
    if (i > 1) json += ",";
    json += R"({"cover_i":)" + std::to_string(i * 10) + "}";
  }
  json += "]}";
  OpenLibraryJsonParser parser;
  parser.beginResponse();
  parser.feed(json.data(), json.size());
  ASSERT_EQ(parser.coverCount(), OpenLibraryJsonParser::MAX_COVERS);
  EXPECT_EQ(parser.coverAt(0), 10u);
  EXPECT_EQ(parser.coverAt(OpenLibraryJsonParser::MAX_COVERS - 1), OpenLibraryJsonParser::MAX_COVERS * 10u);
  EXPECT_EQ(parser.workKey()[0], '\0');
}

TEST(OpenLibraryJsonParser, NoResults) {
  OpenLibraryJsonParser parser;
  parser.beginResponse();
  const char* json = R"({"numFound":0,"start":0,"docs":[]})";
  parser.feed(json, strlen(json));
  EXPECT_FALSE(parser.hasError());
  EXPECT_EQ(parser.coverCount(), 0u);
}

}  // namespace
