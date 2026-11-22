// A text screen in front (Scene::typing): a linked keyboard types into it instead of turning pages.

#include <gtest/gtest.h>

#include <cstring>

#include "Fakes.h"
#include "Runtime.h"

namespace {

using bleturner::Config;
using bleturner::Scene;
using bleturner::TypedKey;
using fake::host;
using fake::radio;

class TypingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fake::reset();
    config.enabled = 1;
    std::strcpy(config.peerAddr, "7d:de:5c:bd:ae:ca");
    bleturner::begin(fake::hostFns(), config);
    radio().queueStarts = true;
  }
  static Scene notepad(const uint32_t visit = 1) {
    Scene s = fake::reading(visit);
    s.typing = true;
    return s;
  }
  bool pass(const Scene& s) {
    const bool acted = bleturner::tick(s);
    if (radio().startQueued) radio().runStart();
    return acted;
  }
  void linkKeyboard() {
    radio().connected = true;
    radio().addr = "7d:de:5c:bd:ae:ca";
    radio().name = "Keyboard";
  }
  static bleturner::port::KeyPress key(const TypedKey typed, const char ch, const uint8_t keycode) {
    bleturner::port::KeyPress k;
    k.keycode = keycode;
    k.pressed = true;
    k.typed = typed;
    k.ch = ch;
    return k;
  }
  Config config;
};

TEST_F(TypingTest, KeysTypeAndTurnNoPage) {
  pass(notepad());
  ASSERT_TRUE(radio().running);
  linkKeyboard();
  pass(notepad());  // the link is picked up; queued input from before is dropped
  radio().keys.push_back(key(TypedKey::Char, 'H', 0x0B));
  radio().keys.push_back(key(TypedKey::Char, ' ', bleturner::kUsageSpace));
  radio().keys.push_back(key(TypedKey::Enter, 0, bleturner::kUsageEnter));
  radio().keys.push_back(key(TypedKey::Backspace, 0, bleturner::kUsageBackspace));
  EXPECT_TRUE(pass(notepad())) << "typing counts as user activity";
  EXPECT_EQ(host().typed, std::string("H \n\b"));
  EXPECT_TRUE(host().delivered.empty());
}

TEST_F(TypingTest, IgnoresKeysThatTypeNothing) {
  pass(notepad());
  linkKeyboard();
  pass(notepad());
  radio().keys.push_back(key(TypedKey::None, 0, bleturner::kUsageRight));
  auto release = key(TypedKey::Char, 'a', 0x04);
  release.pressed = false;
  radio().keys.push_back(release);
  EXPECT_FALSE(pass(notepad()));
  EXPECT_TRUE(host().typed.empty());
  EXPECT_TRUE(host().delivered.empty());
}

TEST_F(TypingTest, RadioStaysUpWaitingForTheKeyboard) {
  pass(notepad());
  ASSERT_TRUE(radio().running);
  radio().now += 2 * bleturner::kIdleOffMs;
  pass(notepad());
  EXPECT_TRUE(radio().running) << "a keyboard switched on later must still link";

  pass(fake::reading());
  radio().now += 2 * bleturner::kIdleOffMs;
  pass(fake::reading());
  pass(fake::reading());
  EXPECT_FALSE(radio().running) << "a book still stops an idle radio";
}

TEST_F(TypingTest, TheSameKeysTurnPagesInABook) {
  pass(fake::reading());
  linkKeyboard();
  pass(fake::reading());
  radio().keys.push_back(key(TypedKey::Char, ' ', bleturner::kUsageSpace));
  pass(fake::reading());
  EXPECT_TRUE(host().typed.empty());
  ASSERT_EQ(host().delivered.size(), 1u);
  EXPECT_EQ(host().delivered.front(), bleturner::Action::NextPage);
}

}  // namespace
