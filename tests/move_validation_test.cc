#include "scrabble/move_validation.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "test_util.h"

namespace scrabble {
namespace {

using testing::EnableGaddag;
using testing::MakeBoard;
using testing::ParseOrDie;
using testing::RackOrDie;

const Gaddag& Lexicon() {
  static const Gaddag gaddag = Gaddag::Build(std::vector<std::string>{
      "HELLO", "HELLOS", "HE", "EH", "LO", "OH", "AX", "EX", "HA", "QI",
      "AT", "TA", "XI"});
  return gaddag;
}

MoveError Check(const Board& board, const std::string& rack, Move& move) {
  return ValidateMove(board, Lexicon(), RackOrDie(rack), move);
}

MoveError Check(const Board& board, const std::string& rack,
                const std::string& notation) {
  Move move = ParseOrDie(notation);
  return Check(board, rack, move);
}

TEST(ValidateMoveTest, FirstMove) {
  const Board empty;
  Move move = ParseOrDie("8H HELLO");
  EXPECT_EQ(Check(empty, "HELLOXY", move), MoveError::kOk);
  EXPECT_EQ(move.score, 18);
  EXPECT_EQ(Check(empty, "HELLO", "H4 HELLO"), MoveError::kOk);
  EXPECT_EQ(Check(empty, "HELLO", "9H HELLO"), MoveError::kFirstMoveOffCenter);
  EXPECT_EQ(Check(empty, "HELLO", "8C HELLO"), MoveError::kFirstMoveOffCenter);
  EXPECT_EQ(Check(empty, "H", "8H H"), MoveError::kWordTooShort);
}

TEST(ValidateMoveTest, RackAccounting) {
  const Board empty;
  EXPECT_EQ(Check(empty, "HELO", "8H HELLO"), MoveError::kNotInRack);
  EXPECT_EQ(Check(empty, "HLLO?", "8H HeLLO"), MoveError::kOk);
  // A designated blank needs a '?' even if the real letter is on the rack.
  EXPECT_EQ(Check(empty, "HELLO", "8H HeLLO"), MoveError::kNotInRack);
}

TEST(ValidateMoveTest, Geometry) {
  const Board board = MakeBoard({"8H HELLO"});
  EXPECT_EQ(Check(board, "H", "8H H"), MoveError::kSquareOccupied);
  EXPECT_EQ(Check(board, "X", "H8 (A)X"), MoveError::kPlayThroughMismatch);
  EXPECT_EQ(Check(board, "X", "9H (A)X"), MoveError::kPlayThroughEmpty);
  EXPECT_EQ(Check(board, "S", "8M S"), MoveError::kIncompleteWord);
  EXPECT_EQ(Check(board, "S", "8G S"), MoveError::kIncompleteWord);
  EXPECT_EQ(Check(board, "S", "8H (HELLO)S"), MoveError::kOk);
  EXPECT_EQ(Check(board, "HE", "1A HE"), MoveError::kNotConnected);

  Move off_board = Move::Place(7, 12, Direction::kAcross);
  for (char c : std::string("HELLO")) {
    off_board.AddTile(MoveTile::Placed(static_cast<Letter>(c - 'A')));
  }
  EXPECT_EQ(Check(Board(), "HELLO", off_board), MoveError::kOutOfBounds);

  Move empty_move = Move::Place(7, 7, Direction::kAcross);
  EXPECT_EQ(Check(Board(), "A", empty_move), MoveError::kEmptyMove);

  Move only_board = Move::Place(7, 7, Direction::kAcross);
  for (int i = 0; i < 5; ++i) {
    only_board.AddTile(MoveTile::PlayThrough(MoveTile::kUnknownLetter));
  }
  EXPECT_EQ(Check(board, "A", only_board), MoveError::kNoRackTiles);

  Move eight = Move::Place(0, 0, Direction::kAcross);
  for (int i = 0; i < 8; ++i) eight.AddTile(MoveTile::Placed(0));
  EXPECT_EQ(Check(Board(), "AAAAAAA", eight), MoveError::kTooManyTiles);

  Move bad = Move::Place(7, 7, Direction::kAcross);
  bad.AddTile(MoveTile::Placed(kBlank));
  bad.AddTile(MoveTile::Placed(0));
  EXPECT_EQ(Check(Board(), "A?", bad), MoveError::kBadTile);
}

TEST(ValidateMoveTest, Words) {
  const Board board = MakeBoard({"8H HELLO"});
  EXPECT_EQ(Check(Board(), "HELLO", "8H HLLEO"), MoveError::kInvalidWord);
  // EH is fine across, but E under E makes EE down.
  EXPECT_EQ(Check(board, "EH", "9I EH"), MoveError::kInvalidCrossWord);
  Move ax = ParseOrDie("9H AX");
  EXPECT_EQ(Check(board, "AX", ax), MoveError::kOk);
  EXPECT_EQ(ax.score, 39);
}

TEST(ValidateMoveTest, SingleTileNeedsAWordInItsDirection) {
  const Board board = MakeBoard({"8H HELLO"});
  // A under H forms only HA, vertically.
  EXPECT_EQ(Check(board, "A", "9H A"), MoveError::kWordTooShort);
  Move down = ParseOrDie("H8 (H)A");
  EXPECT_EQ(Check(board, "A", down), MoveError::kOk);
  EXPECT_EQ(down.score, 5);
}

TEST(ValidateMoveTest, ResolvesPlaceholdersFromBoard) {
  const Board board = MakeBoard({"8H HeLLO"});
  Move move = ParseOrDie("H8 .A");
  ASSERT_EQ(Check(board, "A", move), MoveError::kOk);
  EXPECT_EQ(move, [] {
    Move expected = ParseOrDie("H8 (H)A");
    expected.score = 5;
    return expected;
  }());
  // The play-through tile also picks up the board's blank flag.
  Move through_blank = ParseOrDie("I8 .X");
  ASSERT_EQ(Check(board, "X", through_blank), MoveError::kOk);
  EXPECT_EQ(through_blank.ToString(), "I8 (e)X");
  EXPECT_EQ(through_blank.score, 16);  // e scores 0; X on the I9 DL
}

TEST(ValidateMoveTest, FailureLeavesMoveUntouched) {
  const Board board = MakeBoard({"8H HELLO"});
  Move move = ParseOrDie("H8 .X");  // HX is not a word
  const Move before = move;
  EXPECT_EQ(Check(board, "X", move), MoveError::kInvalidWord);
  EXPECT_EQ(move, before);
}

TEST(ValidateMoveTest, ExchangeAndPass) {
  const Board board;
  EXPECT_EQ(Check(board, "ABCD", "-ABC"), MoveError::kOk);
  EXPECT_EQ(Check(board, "AB", "-ABC"), MoveError::kNotInRack);
  EXPECT_EQ(Check(board, "A?", "-??"), MoveError::kNotInRack);
  EXPECT_EQ(Check(board, "", "-"), MoveError::kOk);
}

TEST(ValidateMoveTest, EveryErrorHasAMessage) {
  for (int e = 0; e <= static_cast<int>(MoveError::kInvalidCrossWord); ++e) {
    const std::string_view message =
        MoveErrorMessage(static_cast<MoveError>(e));
    EXPECT_FALSE(message.empty());
    EXPECT_NE(message, "unknown error");
  }
}

// Generated moves survive a trip through notation and back through
// validation, both verbatim and with play-through letters replaced by '.'.
TEST(EnableValidateMoveTest, NotationRoundTripsThroughValidation) {
  const Gaddag* gaddag = EnableGaddag();
  if (gaddag == nullptr) GTEST_SKIP() << "ENABLE not found";
  int checked = 0;
  for (std::uint64_t seed = 7; seed < 7 + testing::kScale; ++seed) {
    testing::PlayRandomGame(
        *gaddag, seed,
        [&](const Position& position, const Rack& rack,
            const std::vector<Move>& moves) {
          for (std::size_t i = 0; i < moves.size(); i += 13) {
            const Move& generated = moves[i];
            const std::string text = generated.ToString();
            Move parsed = ParseOrDie(text);
            ASSERT_EQ(ValidateMove(position.board(), *gaddag, rack, parsed),
                      MoveError::kOk)
                << text;
            ASSERT_EQ(parsed, generated) << text;

            Move dotted = generated;
            for (std::size_t t = 0; t < dotted.num_tiles; ++t) {
              if (dotted.tiles[t].is_play_through()) {
                dotted.tiles[t] =
                    MoveTile::PlayThrough(MoveTile::kUnknownLetter);
              }
            }
            const std::optional<Move> reparsed =
                Move::Parse(dotted.ToString());
            ASSERT_TRUE(reparsed.has_value()) << dotted.ToString();
            Move resolved = *reparsed;
            ASSERT_EQ(ValidateMove(position.board(), *gaddag, rack, resolved),
                      MoveError::kOk)
                << dotted.ToString();
            ASSERT_EQ(resolved, generated) << dotted.ToString();
            ++checked;
          }
        });
  }
  EXPECT_GT(checked, 100);
}

}  // namespace
}  // namespace scrabble
