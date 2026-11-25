add_executable(OpenLibraryJsonParserTest
  OpenLibraryJsonParserTest.cpp
  ${REPO_ROOT}/lib/JsonParser/OpenLibraryJsonParser.cpp
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp
)

target_include_directories(OpenLibraryJsonParserTest PRIVATE
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include
)

target_link_libraries(OpenLibraryJsonParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpenLibraryJsonParserTest)
