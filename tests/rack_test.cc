#include "scrabble/rack.h"

#include <gtest/gtest.h>

#include <type_traits>

namespace scrabble {
namespace {

static_assert(std::is_trivially_copyable_v<Rack>);

Letter L(char c) { return *CharToLetter(c); }

TEST(RackTest, StartsEmpty) {
  Rack rack;
  EXPECT_TRUE(rack.IsEmpty());
  EXPECT_EQ(rack.Size(), 0);
  EXPECT_EQ(rack.ToString(), "");
}

TEST(RackTest, AddRemoveCount) {
  Rack rack;
  rack.Add(L('E'));
  rack.Add(L('E'));
  rack.Add(kBlank);
  EXPECT_EQ(rack.Size(), 3);
  EXPECT_EQ(rack.Count(L('E')), 2);
  EXPECT_TRUE(rack.Contains(kBlank));
  EXPECT_FALSE(rack.Contains(L('Q')));

  rack.Remove(L('E'));
  EXPECT_EQ(rack.Count(L('E')), 1);
  EXPECT_EQ(rack.Size(), 2);

  rack.Clear();
  EXPECT_TRUE(rack.IsEmpty());
  EXPECT_EQ(rack.Count(L('E')), 0);
}

TEST(RackTest, ToStringIsCanonical) {
  const auto rack = Rack::FromString("?TSNIEA");
  ASSERT_TRUE(rack.has_value());
  EXPECT_EQ(rack->ToString(), "AEINST?");
}

TEST(RackTest, RoundTrip) {
  for (const char* text : {"", "A", "QZ", "EEEEEEE", "AB??", "AEINST?"}) {
    const auto rack = Rack::FromString(text);
    ASSERT_TRUE(rack.has_value()) << text;
    EXPECT_EQ(rack->ToString(), text);
    EXPECT_EQ(Rack::FromString(rack->ToString()), rack);
  }
}

TEST(RackTest, FromStringAcceptsLowercase) {
  EXPECT_EQ(Rack::FromString("qi"), Rack::FromString("QI"));
}

TEST(RackTest, FromStringRejectsInvalid) {
  EXPECT_FALSE(Rack::FromString("ABCDEFGH").has_value());  // 8 tiles
  EXPECT_FALSE(Rack::FromString("AB1").has_value());
  EXPECT_FALSE(Rack::FromString("A B").has_value());
  EXPECT_FALSE(Rack::FromString("*").has_value());
}

TEST(RackTest, EqualityIgnoresOrder) {
  EXPECT_EQ(Rack::FromString("RETAINS"), Rack::FromString("NASTIER"));
  EXPECT_NE(Rack::FromString("RETAINS"), Rack::FromString("RETAIN?"));
}

TEST(RackTest, Score) {
  EXPECT_EQ(Rack::FromString("QZ?")->Score(), 20);
  EXPECT_EQ(Rack::FromString("AEINST?")->Score(), 6);
  EXPECT_EQ(Rack{}.Score(), 0);
}

}  // namespace
}  // namespace scrabble
