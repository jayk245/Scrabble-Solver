#include "scrabble/letter_set.h"

#include <gtest/gtest.h>

namespace scrabble {
namespace {

constexpr Letter L(char c) { return static_cast<Letter>(c - 'A'); }

TEST(LetterSetTest, DefaultIsEmpty) {
  constexpr LetterSet set;
  EXPECT_TRUE(set.Empty());
  EXPECT_EQ(set.Count(), 0);
  for (Letter l = 0; l < kNumLetters; ++l) EXPECT_FALSE(set.Contains(l));
}

TEST(LetterSetTest, AllContainsEveryLetter) {
  constexpr LetterSet all = LetterSet::All();
  EXPECT_EQ(all.Count(), kNumLetters);
  for (Letter l = 0; l < kNumLetters; ++l) EXPECT_TRUE(all.Contains(l));
}

TEST(LetterSetTest, AddAndRemove) {
  LetterSet set;
  set.Add(L('Q'));
  set.Add(L('Z'));
  set.Add(L('Q'));  // idempotent
  EXPECT_EQ(set.Count(), 2);
  EXPECT_TRUE(set.Contains(L('Q')));
  EXPECT_TRUE(set.Contains(L('Z')));
  EXPECT_FALSE(set.Contains(L('A')));
  set.Remove(L('Q'));
  set.Remove(L('B'));  // absent: no-op
  EXPECT_EQ(set.Count(), 1);
  EXPECT_FALSE(set.Contains(L('Q')));
}

TEST(LetterSetTest, UnionAndIntersect) {
  LetterSet a;
  a.Add(L('A'));
  a.Add(L('B'));
  LetterSet b;
  b.Add(L('B'));
  b.Add(L('C'));

  const LetterSet u = a.Union(b);
  EXPECT_EQ(u.Count(), 3);
  EXPECT_EQ(u, a | b);

  const LetterSet i = a.Intersect(b);
  EXPECT_EQ(i.Count(), 1);
  EXPECT_TRUE(i.Contains(L('B')));
  EXPECT_EQ(i, a & b);

  EXPECT_TRUE((a & LetterSet()).Empty());
  EXPECT_EQ(a & LetterSet::All(), a);
}

TEST(LetterSetTest, RawConstructorDropsBitsAboveZ) {
  const LetterSet set(0xFFFFFFFFu);
  EXPECT_EQ(set, LetterSet::All());
  EXPECT_EQ(set.bits(), LetterSet::kAllBits);
}

TEST(LetterSetTest, IsUsableInConstantExpressions) {
  constexpr LetterSet vowels = [] {
    LetterSet s;
    for (char c : {'A', 'E', 'I', 'O', 'U'}) s.Add(L(c));
    return s;
  }();
  static_assert(vowels.Count() == 5);
  static_assert(vowels.Contains(L('E')));
  static_assert(!vowels.Contains(L('Y')));
}

}  // namespace
}  // namespace scrabble
