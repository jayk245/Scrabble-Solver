#include "scrabble/board.h"

#include <gtest/gtest.h>

namespace scrabble {
namespace {

int CountPremium(Premium premium) {
  int count = 0;
  for (Premium p : kPremiumLayout) count += (p == premium);
  return count;
}

TEST(PremiumLayoutTest, HasStandardCounts) {
  EXPECT_EQ(CountPremium(Premium::kTripleWord), 8);
  EXPECT_EQ(CountPremium(Premium::kDoubleWord), 17);  // includes centre
  EXPECT_EQ(CountPremium(Premium::kTripleLetter), 12);
  EXPECT_EQ(CountPremium(Premium::kDoubleLetter), 24);
}

TEST(PremiumLayoutTest, CentreIsDoubleWord) {
  EXPECT_EQ(PremiumAt(kCenter, kCenter), Premium::kDoubleWord);
}

// The real board is symmetric under both mirrors and transposition; any
// typo in the layout table breaks at least one of these.
TEST(PremiumLayoutTest, IsSymmetric) {
  constexpr int kLast = kBoardSize - 1;
  for (int r = 0; r < kBoardSize; ++r) {
    for (int c = 0; c < kBoardSize; ++c) {
      EXPECT_EQ(PremiumAt(r, c), PremiumAt(c, r)) << r << "," << c;
      EXPECT_EQ(PremiumAt(r, c), PremiumAt(kLast - r, c)) << r << "," << c;
      EXPECT_EQ(PremiumAt(r, c), PremiumAt(r, kLast - c)) << r << "," << c;
    }
  }
}

TEST(BoardTest, StartsEmpty) {
  Board board;
  EXPECT_TRUE(board.IsEmpty());
  EXPECT_TRUE(board.At(kCenter, kCenter).IsEmpty());
}

TEST(BoardTest, PlaceAndRemoveTrackTileCount) {
  Board board;
  board.Place(kCenter, kCenter, *CharToLetter('Q'));
  board.Place(kCenter, kCenter + 1, *CharToLetter('I'), /*is_blank=*/true);
  EXPECT_EQ(board.TileCount(), 2);
  EXPECT_EQ(board.At(kCenter, kCenter).letter, *CharToLetter('Q'));
  EXPECT_TRUE(board.At(kCenter, kCenter + 1).is_blank);

  board.Remove(kCenter, kCenter);
  EXPECT_EQ(board.TileCount(), 1);
  EXPECT_TRUE(board.At(kCenter, kCenter).IsEmpty());
}

TEST(BoardTest, ToStringRendersTilesAndBlanks) {
  Board board;
  board.Place(kCenter, kCenter, *CharToLetter('A'));
  board.Place(kCenter, kCenter + 1, *CharToLetter('B'), /*is_blank=*/true);
  const std::string text = board.ToString();
  EXPECT_NE(text.find(" 8 = . . ' . . . A b"), std::string::npos) << text;
}

}  // namespace
}  // namespace scrabble
