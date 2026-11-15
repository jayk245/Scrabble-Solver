#include "scrabble/scoring.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test_util.h"

namespace scrabble {
namespace {

using testing::MakeBoard;
using testing::ParseOrDie;

int ScoreOn(const std::vector<std::string>& board_moves,
            const std::string& move) {
  return Score(MakeBoard(board_moves), ParseOrDie(move));
}

// Each expectation is worked by hand from the premium layout; the comment
// shows the arithmetic.
TEST(ScoreTest, OpeningOnCentreDoubleWord) {
  // H on centre DW, O on 8L DL: (4 + 1 + 1 + 1 + 1*2) * 2.
  EXPECT_EQ(ScoreOn({}, "8H HELLO"), 18);
  // Same word down: H8 is the centre and O lands on the H12 DL.
  EXPECT_EQ(ScoreOn({}, "H8 HELLO"), 18);
}

TEST(ScoreTest, BlanksScoreZeroButStillTakeWordPremiums) {
  // Blank e: (4 + 0 + 1 + 1 + 2) * 2.
  EXPECT_EQ(ScoreOn({}, "8H HeLLO"), 16);
  // Blank h on the centre DW still doubles the word: (0 + 1 + 1 + 1 + 2) * 2.
  EXPECT_EQ(ScoreOn({}, "8H hELLO"), 10);
}

TEST(ScoreTest, OpeningBingo) {
  // 8D..8J: A on DL. (2 + 3 + 3 + 2 + 1 + 4 + 2) * 2 + 50.
  EXPECT_EQ(ScoreOn({}, "8D ABCDEFG"), 84);
}

TEST(ScoreTest, CrossWordsAndPremiumUnderExistingTile) {
  // AX at 9H-9I under HE: X on a DL. Main AX = 1 + 16 = 17. Cross-words
  // HA = 4 + 1 = 5 (H's centre DW is spent) and EX = 1 + 16 = 17.
  EXPECT_EQ(ScoreOn({"8H HELLO"}, "9H AX"), 39);
}

TEST(ScoreTest, SingleTileScoresSameInBothDirections) {
  // A under H forms only HA (5); the lone A is not a main word across.
  EXPECT_EQ(ScoreOn({"8H HELLO"}, "9H A"), 5);
  EXPECT_EQ(ScoreOn({"8H HELLO"}, "H8 (H)A"), 5);
}

TEST(ScoreTest, PlayThroughDoesNotReapplyPremium) {
  // QI with existing I on the centre DW; Q on 8G (plain). 10 + 1, no DW.
  EXPECT_EQ(ScoreOn({"H7 HIT"}, "8G Q(I)"), 11);
}

TEST(ScoreTest, TripleTripleBingoThroughExistingDoubleLetter) {
  // 1A..1H through an existing D on the 1D DL (not reapplied), with both
  // ends on TWs: (1 + 3 + 3 + 2 + 1 + 4 + 2 + 4) * 9 + 50.
  EXPECT_EQ(ScoreOn({"D1 DO"}, "1A ABC(D)EFGH"), 230);
}

TEST(ScoreTest, DoubleDoubleStacks) {
  // 5E..5K covers both row-5 DWs: (1 + 3 + 3 + 2 + 1 + 4 + 2) * 4 + 50.
  EXPECT_EQ(ScoreOn({}, "5E ABCDEFG"), 114);
}

TEST(ScoreTest, TwoWayDoubleWord) {
  // T on the 5E DW extends CA (E3-E4) to CAT and starts TO across. Both
  // words double: TO = (1 + 1) * 2, CAT = (3 + 1 + 1) * 2.
  EXPECT_EQ(ScoreOn({"E3 CA"}, "5E TO"), 14);
}

TEST(ScoreTest, TripleLetterInBothWords) {
  // Z on the 6F TL forms ZA across and ZO down (O at F7 below it).
  // ZA = 30 + 1, ZO = 30 + 1.
  EXPECT_EQ(ScoreOn({"F7 O"}, "6F ZA"), 62);
}

TEST(ScoreTest, BlankNeighbourMakesZeroPointCrossWord) {
  // B under the blank a forms the cross-word aB = 0 + 3; the main word BE
  // has E on the 9I DL: 3 + 2.
  EXPECT_EQ(ScoreOn({"8G Ha"}, "9H BE"), 8);
}

TEST(ScoreTest, PassAndExchangeScoreZero) {
  const Board board;
  EXPECT_EQ(Score(board, Move::Pass()), 0);
  EXPECT_EQ(Score(board, ParseOrDie("-ABC")), 0);
}

}  // namespace
}  // namespace scrabble
