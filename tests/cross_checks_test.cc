#include "scrabble/cross_checks.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "scrabble/position.h"
#include "test_util.h"

namespace scrabble {
namespace {

using testing::EnableGaddag;
using testing::MakeBoard;
using testing::ParseOrDie;

LetterSet Letters(const std::string& letters) {
  LetterSet set;
  for (char c : letters) set.Add(static_cast<Letter>(c - 'A'));
  return set;
}

const Gaddag& SmallLexicon() {
  static const Gaddag gaddag = Gaddag::Build(std::vector<std::string>{
      "AT", "AS", "TA", "CAT", "CATS", "SCAT", "CUT", "CUTS", "UT", "BA"});
  return gaddag;
}

TEST(CrossChecksTest, EmptyBoardAllowsEverything) {
  const CrossChecks checks = CrossChecks::ComputeAll(Board(), SmallLexicon());
  EXPECT_EQ(checks, CrossChecks());
  const CrossCheck& check = checks.For(Direction::kAcross, kCenter, kCenter);
  EXPECT_EQ(check.allowed, LetterSet::All());
  EXPECT_FALSE(check.has_cross_word);
  EXPECT_EQ(check.score, 0);
}

TEST(CrossChecksTest, ConstraintsAroundAWord) {
  // CAT across at 8H-8J (row 7, cols 7-9).
  const Board board = MakeBoard({"8H CAT"});
  const CrossChecks checks = CrossChecks::ComputeAll(board, SmallLexicon());

  // Capping the word: only down moves see the horizontal cross-word.
  const CrossCheck& after = checks.For(Direction::kDown, 7, 10);
  EXPECT_EQ(after.allowed, Letters("S"));  // CATS
  EXPECT_TRUE(after.has_cross_word);
  EXPECT_EQ(after.score, 5);
  EXPECT_EQ(checks.For(Direction::kDown, 7, 6).allowed, Letters("S"));  // SCAT
  EXPECT_EQ(checks.For(Direction::kAcross, 7, 10), CrossCheck{});

  // Above and below A: the vertical word for across moves.
  EXPECT_EQ(checks.For(Direction::kAcross, 6, 8).allowed, Letters("BT"));
  EXPECT_EQ(checks.For(Direction::kAcross, 8, 8).allowed, Letters("ST"));
  EXPECT_EQ(checks.For(Direction::kAcross, 8, 8).score, 1);
  // Below C: no two-letter word starts with C.
  EXPECT_TRUE(checks.For(Direction::kAcross, 8, 7).allowed.Empty());
  EXPECT_TRUE(checks.For(Direction::kAcross, 8, 7).has_cross_word);

  // Occupied and distant squares.
  EXPECT_EQ(checks.For(Direction::kAcross, 7, 8), kOccupiedCrossCheck);
  EXPECT_EQ(checks.For(Direction::kDown, 7, 8), kOccupiedCrossCheck);
  EXPECT_EQ(checks.For(Direction::kDown, 0, 0), CrossCheck{});
}

TEST(CrossChecksTest, GapBetweenTwoTiles) {
  // C _ T: the hole must make CAT or CUT.
  const Board board = MakeBoard({"8G C", "8I T"});
  const CrossChecks checks = CrossChecks::ComputeAll(board, SmallLexicon());
  const CrossCheck& gap = checks.For(Direction::kDown, 7, 7);
  EXPECT_EQ(gap.allowed, Letters("AU"));
  EXPECT_EQ(gap.score, 4);
}

TEST(CrossChecksTest, ZeroPointCrossWordIsDistinctFromNone) {
  // A blank a above the square: cross-word "a?" exists but is worth 0.
  const Board board = MakeBoard({"8H a"});
  const CrossChecks checks = CrossChecks::ComputeAll(board, SmallLexicon());
  const CrossCheck& below = checks.For(Direction::kAcross, 8, 7);
  EXPECT_TRUE(below.has_cross_word);
  EXPECT_EQ(below.score, 0);
  EXPECT_EQ(below.allowed, Letters("ST"));
  EXPECT_FALSE(checks.For(Direction::kAcross, 10, 7).has_cross_word);
}

TEST(CrossChecksTest, BoardEdges) {
  const Board board = MakeBoard({"1A CAT", "O14 UT"});
  const CrossChecks checks = CrossChecks::ComputeAll(board, SmallLexicon());
  EXPECT_EQ(checks.For(Direction::kDown, 0, 3).allowed, Letters("S"));
  EXPECT_EQ(checks.For(Direction::kAcross, 1, 1).allowed, Letters("ST"));
  EXPECT_EQ(checks.For(Direction::kAcross, 12, 14).allowed, Letters("C"));
  EXPECT_EQ(checks.For(Direction::kDown, 14, 13).allowed, Letters("AU"));
}

TEST(CrossChecksTest, IncrementalUpdateHandlesPlacementAndRemoval) {
  Board board = MakeBoard({"8H CAT"});
  CrossChecks checks = CrossChecks::ComputeAll(board, SmallLexicon());
  const Move move = ParseOrDie("I7 B(A)");
  PlaceTiles(board, move);
  checks.Update(board, SmallLexicon(), move);
  EXPECT_EQ(checks, CrossChecks::ComputeAll(board, SmallLexicon()));
  RemoveTiles(board, move);
  checks.Update(board, SmallLexicon(), move);
  EXPECT_EQ(checks, CrossChecks::ComputeAll(board, SmallLexicon()));
}

class EnableCrossChecksTest : public ::testing::Test {
 protected:
  void SetUp() override {
    gaddag_ = EnableGaddag();
    if (gaddag_ == nullptr) GTEST_SKIP() << "ENABLE not found";
  }
  const Gaddag* gaddag_ = nullptr;
};

// After every move of random games, the incrementally maintained
// cross-checks equal a from-scratch recompute, both after placing the move
// and after taking it back off the board.
TEST_F(EnableCrossChecksTest, IncrementalMatchesFullRecompute) {
  const Gaddag& gaddag = *gaddag_;
  int checked = 0;
  for (std::uint64_t seed = 1; seed <= 2 * testing::kScale; ++seed) {
    testing::PlayRandomGame(
        gaddag, seed,
        [&](const Position& position, const Rack&,
            const std::vector<Move>& moves) {
          ASSERT_EQ(position.cross_checks(),
                    CrossChecks::ComputeAll(position.board(), gaddag));
          // Removal path: undo a random-ish subset via Update, not Restore.
          for (std::size_t i = 0; i < moves.size(); i += 97) {
            Board board = position.board();
            CrossChecks checks = position.cross_checks();
            PlaceTiles(board, moves[i]);
            checks.Update(board, gaddag, moves[i]);
            ASSERT_EQ(checks, CrossChecks::ComputeAll(board, gaddag))
                << moves[i].ToString();
            RemoveTiles(board, moves[i]);
            checks.Update(board, gaddag, moves[i]);
            ASSERT_EQ(checks, position.cross_checks()) << moves[i].ToString();
            ++checked;
          }
        });
  }
  EXPECT_GT(checked, 0);
}

// Apply/Undo of every generated move restores board and cross-checks
// exactly; a deep LIFO chain unwinds to the starting position too.
TEST_F(EnableCrossChecksTest, ApplyUndoRestoresExactly) {
  const Gaddag& gaddag = *gaddag_;
  MoveGenerator generator(gaddag);
  std::vector<Move> replies;
  for (std::uint64_t seed = 100; seed < 100 + testing::kScale; ++seed) {
    testing::PlayRandomGame(
        gaddag, seed,
        [&](const Position& position, const Rack& rack,
            const std::vector<Move>& moves) {
          Position scratch = position;
          for (std::size_t i = 0; i < moves.size(); i += 7) {
            const PositionUndo undo = scratch.Apply(moves[i]);
            scratch.Undo(moves[i], undo);
            ASSERT_EQ(scratch.board(), position.board());
            ASSERT_EQ(scratch.cross_checks(), position.cross_checks());
          }
          // Two-ply chain. The reply reuses the rack: tile accounting is
          // irrelevant to what is being checked here.
          if (moves.empty()) return;
          const Move& first = moves[moves.size() / 2];
          const PositionUndo undo1 = scratch.Apply(first);
          generator.Generate(scratch, rack, replies);
          if (!replies.empty()) {
            const Move& second = replies[replies.size() / 3];
            const PositionUndo undo2 = scratch.Apply(second);
            ASSERT_EQ(scratch.cross_checks(),
                      CrossChecks::ComputeAll(scratch.board(), gaddag));
            scratch.Undo(second, undo2);
          }
          scratch.Undo(first, undo1);
          ASSERT_EQ(scratch.board(), position.board());
          ASSERT_EQ(scratch.cross_checks(), position.cross_checks());
        });
  }
}

}  // namespace
}  // namespace scrabble
