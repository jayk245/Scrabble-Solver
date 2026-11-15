#include "scrabble/move_generator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "scrabble/move_validation.h"
#include "scrabble/rng.h"
#include "scrabble/scoring.h"
#include "test_util.h"

namespace scrabble {
namespace {

using testing::EnableGaddag;
using testing::MakeBoard;
using testing::RackOrDie;
using testing::ReferenceMoves;
using testing::SameMoves;

std::vector<Move> Generate(const Gaddag& gaddag, const Board& board,
                           const std::string& rack,
                           const MoveGenOptions& options = {}) {
  MoveGenerator generator(gaddag);
  std::vector<Move> moves;
  generator.Generate(Position(gaddag, board), RackOrDie(rack), moves, options);
  return moves;
}

std::set<std::string> Notations(const std::vector<Move>& moves) {
  std::set<std::string> out;
  for (const Move& move : moves) out.insert(move.ToString());
  return out;
}

::testing::AssertionResult MatchesReference(const Gaddag& gaddag,
                                            const Board& board,
                                            const std::string& rack,
                                            bool prune = true) {
  return SameMoves(Generate(gaddag, board, rack),
                   ReferenceMoves(board, gaddag, RackOrDie(rack), prune));
}

TEST(MoveGeneratorTest, FirstMoveCoversCentreBothWays) {
  const Gaddag gaddag = Gaddag::Build(std::vector<std::string>{"AB", "BA"});
  const std::vector<Move> moves = Generate(gaddag, Board(), "AB");
  EXPECT_EQ(Notations(moves),
            (std::set<std::string>{"8G AB", "8H AB", "8G BA", "8H BA",
                                   "H7 AB", "H8 AB", "H7 BA", "H8 BA"}));
  for (const Move& move : moves) EXPECT_EQ(move.score, 8) << move.ToString();
}

TEST(MoveGeneratorTest, EmptyRackOrNoWordsGivesNoPlacements) {
  const Gaddag gaddag = Gaddag::Build(std::vector<std::string>{"AB"});
  EXPECT_TRUE(Generate(gaddag, Board(), "").empty());
  EXPECT_TRUE(Generate(gaddag, Board(), "CDE").empty());
  EXPECT_TRUE(Generate(Gaddag(), MakeBoard({"8H AB"}), "AB?").empty());
}

TEST(MoveGeneratorTest, SingleTilesAreEmittedOnce) {
  const Gaddag gaddag = Gaddag::Build(std::vector<std::string>{"AA"});
  // A lone A at the centre: single tiles beside it are across moves, above
  // and below it down moves.
  EXPECT_EQ(Notations(Generate(gaddag, MakeBoard({"8H A"}), "A")),
            (std::set<std::string>{"8G A(A)", "8H (A)A", "H7 A(A)",
                                   "H8 (A)A"}));

  // 9H touches the A above and the A to its right: one move, reported
  // across, scoring both words.
  const Board board = MakeBoard({"8H A", "9I A"});
  const std::vector<Move> moves = Generate(gaddag, board, "A");
  const std::set<std::string> notations = Notations(moves);
  EXPECT_EQ(notations.count("9H A(A)"), 1u);
  EXPECT_EQ(notations.count("H8 (A)A"), 0u);
  for (const Move& move : moves) {
    if (move.ToString() == "9H A(A)") {
      EXPECT_EQ(move.score, 4);  // AA across (2) + AA down (2)
    }
  }
  EXPECT_TRUE(MatchesReference(gaddag, board, "A", /*prune=*/false));
}

TEST(MoveGeneratorTest, BlankTriesEveryAllowedLetter) {
  const Gaddag gaddag =
      Gaddag::Build(std::vector<std::string>{"AB", "AD", "AX", "BA"});
  const std::vector<Move> moves = Generate(gaddag, MakeBoard({"8H A"}), "?");
  EXPECT_EQ(Notations(moves),
            (std::set<std::string>{"8H (A)b", "8H (A)d", "8H (A)x", "8G b(A)",
                                   "H8 (A)b", "H8 (A)d", "H8 (A)x",
                                   "H7 b(A)"}));
  for (const Move& move : moves) EXPECT_EQ(move.score, 1) << move.ToString();
}

TEST(MoveGeneratorTest, PlaysThroughAndAlongEdges) {
  const Gaddag gaddag = Gaddag::Build(std::vector<std::string>{
      "CAT", "CATS", "SCAT", "AT", "TA", "AS", "ACT", "TACT", "TACTS"});
  const Board board = MakeBoard({"1A CAT", "O13 CAT", "8H ACT"});
  for (const char* rack : {"S", "ST", "TAS", "STAT", "?S", "T?"}) {
    EXPECT_TRUE(MatchesReference(gaddag, board, rack, /*prune=*/false))
        << rack;
  }
}

TEST(MoveGeneratorTest, ExchangesAndPass) {
  const Gaddag gaddag = Gaddag::Build(std::vector<std::string>{"AB"});
  const MoveGenOptions options{.include_exchanges = true, .include_pass = true};
  const std::vector<Move> moves = Generate(gaddag, Board(), "AAB", options);
  EXPECT_EQ(Notations(moves),
            (std::set<std::string>{"8G AB", "8H AB", "H7 AB", "H8 AB", "-A",
                                   "-AA", "-B", "-AB", "-AAB", "-"}));
  EXPECT_EQ(moves.back(), Move::Pass());
  EXPECT_EQ(std::count(moves.begin(), moves.end(), Move::Exchange(RackOrDie(
                                                       "BAA"))),
            1);

  // Seven distinct tiles: 2^7 - 1 exchanges.
  const std::vector<Move> all =
      Generate(gaddag, Board(), "CDEFGH?", {.include_exchanges = true});
  EXPECT_EQ(all.size(), 127u);
}

TEST(MoveGeneratorTest, ReusesOutputBuffer) {
  const Gaddag gaddag = Gaddag::Build(std::vector<std::string>{"AB", "BA"});
  MoveGenerator generator(gaddag);
  const Position position(gaddag);
  std::vector<Move> moves;
  generator.Generate(position, RackOrDie("AB"), moves);
  const Move* data = moves.data();
  const std::size_t capacity = moves.capacity();
  generator.Generate(position, RackOrDie("A"), moves);
  EXPECT_TRUE(moves.empty());
  generator.Generate(position, RackOrDie("AB"), moves);
  EXPECT_EQ(moves.data(), data);
  EXPECT_EQ(moves.capacity(), capacity);
}

// Random games on a small-alphabet lexicon dense with short words, checked
// against the brute-force reference at every position. Small alphabets make
// play-through, blanks and crowded cross-checks very common.
TEST(MoveGeneratorTest, MatchesReferenceOnRandomSmallLexiconGames) {
  std::mt19937 word_rng(99);
  std::uniform_int_distribution<int> length(2, 6);
  std::uniform_int_distribution<int> letter(0, 4);
  std::vector<std::string> words;
  for (int i = 0; i < 300; ++i) {
    std::string w(static_cast<std::size_t>(length(word_rng)), 'A');
    for (char& c : w) c = static_cast<char>('A' + letter(word_rng));
    words.push_back(w);
  }
  const Gaddag gaddag = Gaddag::Build(words);
  MoveGenerator generator(gaddag);
  std::vector<Move> moves;
  int positions = 0;
  for (std::uint64_t seed = 1; seed <= 3 * testing::kScale; ++seed) {
    Rng rng(seed);
    Position position(gaddag);
    for (int turn = 0; turn < 25; ++turn) {
      Rack rack;
      const int size = 1 + static_cast<int>(rng.Below(kRackSize));
      for (int i = 0; i < size; ++i) {
        // Roughly one tile in twelve is a blank.
        const std::uint32_t r = rng.Below(60);
        rack.Add(r < 5 ? kBlank : static_cast<Letter>(r % 5));
      }
      generator.Generate(position, rack, moves);
      const bool exhaustive = rack.Size() <= 4 && !rack.Contains(kBlank);
      ASSERT_TRUE(SameMoves(moves, ReferenceMoves(position.board(), gaddag,
                                                  rack, !exhaustive)))
          << "seed " << seed << " rack " << rack.ToString() << "\n"
          << position.board().ToString();
      ++positions;
      if (moves.empty()) continue;
      const auto pick = rng.Below(static_cast<std::uint32_t>(moves.size()));
      position.Apply(moves[pick]);
    }
  }
  EXPECT_EQ(positions, 75 * testing::kScale);
}

class EnableMoveGeneratorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    gaddag_ = EnableGaddag();
    if (gaddag_ == nullptr) GTEST_SKIP() << "ENABLE not found";
  }
  const Gaddag* gaddag_ = nullptr;
};

TEST_F(EnableMoveGeneratorTest, MatchesReferenceOnEmptyBoard) {
  for (const char* rack : {"AEINRST", "QUIZJAX", "RETAIN?", "A??", "E"}) {
    EXPECT_TRUE(MatchesReference(*gaddag_, Board(), rack)) << rack;
  }
}

TEST_F(EnableMoveGeneratorTest, MatchesReferenceOnCraftedBoards) {
  const std::vector<Board> boards = {
      // Words on every edge and corner plus a centre word.
      MakeBoard({"1A CAT", "O1 DOG", "A13 FOX", "15L BEE", "8G QUIET",
                 "J3 HAZE"}),
      // Blanks on the board, and parallel words for many cross-checks.
      MakeBoard({"8H HeLLo", "9G ADZE", "H5 PUs(H)"}),
      // A long word to play through and hook.
      MakeBoard({"8A RETAIN", "8G ERS", "D8 (A)rEA"}),
  };
  for (std::size_t b = 0; b < boards.size(); ++b) {
    for (const char* rack :
         {"AEINRST", "ETAOIN?", "QXZJKVW", "S", "EEDDSS?", "UNSOLVE"}) {
      EXPECT_TRUE(MatchesReference(*gaddag_, boards[b], rack))
          << "board " << b << " rack " << rack << "\n"
          << boards[b].ToString();
    }
  }
}

TEST_F(EnableMoveGeneratorTest, MatchesReferenceInRandomGames) {
  int positions = 0;
  for (std::uint64_t seed = 500; seed < 500 + testing::kScale / 2 + 1;
       ++seed) {
    int turn = 0;
    testing::PlayRandomGame(
        *gaddag_, seed,
        [&](const Position& position, const Rack& rack,
            const std::vector<Move>& moves) {
          // Debug builds sample every third position to stay fast.
          if (turn++ % (testing::kScale > 1 ? 1 : 3) != 0) return;
          ASSERT_TRUE(SameMoves(
              moves, ReferenceMoves(position.board(), *gaddag_, rack)))
              << "seed " << seed << " rack " << rack.ToString() << "\n"
              << position.board().ToString();
          ++positions;
        });
  }
  EXPECT_GT(positions, 5);
}

// Every generated move is legal per ValidateMove(), scores exactly what
// Score() says, and appears once.
TEST_F(EnableMoveGeneratorTest, FuzzGeneratedMovesAreLegalAndUnique) {
  std::size_t total = 0;
  for (std::uint64_t seed = 1000; seed < 1000 + 5 * testing::kScale; ++seed) {
    testing::PlayRandomGame(
        *gaddag_, seed,
        [&](const Position& position, const Rack& rack,
            const std::vector<Move>& moves) {
          std::vector<Move> sorted = moves;
          for (const Move& move : moves) {
            Move checked = move;
            ASSERT_EQ(ValidateMove(position.board(), *gaddag_, rack, checked),
                      MoveError::kOk)
                << move.ToString() << "\n" << position.board().ToString();
            ASSERT_EQ(checked, move) << move.ToString();
            ASSERT_EQ(Score(position.board(), move), move.score)
                << move.ToString();
            ASSERT_EQ(move.equity, 0.0);
          }
          std::sort(sorted.begin(), sorted.end(),
                    [](const Move& a, const Move& b) {
                      return a.ToString() < b.ToString();
                    });
          ASSERT_EQ(std::adjacent_find(sorted.begin(), sorted.end()),
                    sorted.end());
          total += moves.size();
        });
  }
  EXPECT_GT(total, 1000u);
}

}  // namespace
}  // namespace scrabble
