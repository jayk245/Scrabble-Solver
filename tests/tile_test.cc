#include "scrabble/tile.h"

#include <gtest/gtest.h>

namespace scrabble {
namespace {

TEST(TileTest, BagHoldsStandardDistribution) {
  EXPECT_EQ(kTotalTiles, 100);
  EXPECT_EQ(kTileCounts['E' - 'A'], 12);
  EXPECT_EQ(kTileCounts[kBlank], 2);
}

TEST(TileTest, TotalBagValueIs187) {
  int total = 0;
  for (int i = 0; i < kAlphabetSize; ++i) {
    total += kTileValues[static_cast<std::size_t>(i)] *
             kTileCounts[static_cast<std::size_t>(i)];
  }
  EXPECT_EQ(total, 187);
}

TEST(TileTest, CharRoundTrip) {
  for (Letter l = 0; l < kNumLetters; ++l) {
    EXPECT_EQ(CharToLetter(LetterToChar(l)), l);
  }
  EXPECT_EQ(CharToLetter('?'), kBlank);
  EXPECT_EQ(LetterToChar(kBlank), '?');
}

TEST(TileTest, CharToLetterIsCaseInsensitive) {
  EXPECT_EQ(CharToLetter('q'), CharToLetter('Q'));
}

TEST(TileTest, CharToLetterRejectsInvalidInput) {
  EXPECT_FALSE(CharToLetter('1').has_value());
  EXPECT_FALSE(CharToLetter(' ').has_value());
}

}  // namespace
}  // namespace scrabble
