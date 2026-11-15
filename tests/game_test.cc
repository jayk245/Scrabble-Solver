#include "scrabble/game.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "scrabble/move_generator.h"
#include "test_util.h"

namespace scrabble {
namespace {

using testing::EnableGaddag;
using testing::ParseOrDie;
using testing::RackOrDie;

Bag BagOf(std::string_view tiles) {
  Bag bag;
  for (char c : tiles) bag.Return(*CharToLetter(c));
  return bag;
}

Gaddag SmallLexicon() {
  return Gaddag::Build(std::vector<std::string>{"EEEEEEE", "ABE", "AB"});
}

GameSetup MakeSetup(std::string_view rack0, std::string_view rack1,
                std::string_view bag) {
  GameSetup setup;
  setup.racks = {RackOrDie(std::string(rack0)), RackOrDie(std::string(rack1))};
  setup.bag = BagOf(bag);
  return setup;
}

void ExpectSameState(const Game& a, const Game& b) {
  EXPECT_EQ(a.board(), b.board());
  for (int p = 0; p < kNumPlayers; ++p) {
    EXPECT_EQ(a.rack(p), b.rack(p));
    EXPECT_EQ(a.score(p), b.score(p));
  }
  EXPECT_EQ(a.bag().Size(), b.bag().Size());
  for (Letter l = 0; l < kAlphabetSize; ++l) {
    EXPECT_EQ(a.bag().Count(l), b.bag().Count(l));
  }
  EXPECT_EQ(a.turn(), b.turn());
  EXPECT_EQ(a.to_move(), b.to_move());
  EXPECT_EQ(a.scoreless_turns(), b.scoreless_turns());
  EXPECT_EQ(a.status(), b.status());
}

TEST(GameTest, NewGameDealsTwoRacks) {
  const Gaddag gaddag = SmallLexicon();
  const Game game(gaddag, 7);
  EXPECT_EQ(game.rack(0).Size(), kRackSize);
  EXPECT_EQ(game.rack(1).Size(), kRackSize);
  EXPECT_EQ(game.bag().Size(), kTotalTiles - 2 * kRackSize);
  EXPECT_EQ(game.to_move(), 0);
  EXPECT_EQ(game.turn(), 0);
  EXPECT_FALSE(game.IsOver());
}

TEST(GameTest, ResetMatchesFreshGame) {
  const Gaddag gaddag = SmallLexicon();
  Game game(gaddag, 1);
  ASSERT_TRUE(game.Play(Move::Pass()).ok());
  game.Reset(9);
  ExpectSameState(game, Game(gaddag, 9));
  EXPECT_TRUE(game.history().empty());
}

TEST(GameTest, ExchangeNeedsSevenTilesInBag) {
  const Gaddag gaddag = SmallLexicon();
  const Move exchange = ParseOrDie("-VV");

  Game short_bag(gaddag, MakeSetup("VVVVVVV", "AB", "EEEEEE"), 1);
  const Game before = short_bag;
  EXPECT_EQ(short_bag.Play(exchange).error, PlayError::kExchangeNotAllowed);
  ExpectSameState(short_bag, before);

  Game full_bag(gaddag, MakeSetup("VVVVVVV", "AB", "EEEEEEE"), 1);
  ASSERT_TRUE(full_bag.Play(exchange).ok());
  EXPECT_EQ(full_bag.rack(0), RackOrDie("EEVVVVV"));
  EXPECT_EQ(full_bag.bag().Size(), 7);
  EXPECT_EQ(full_bag.bag().Count(*CharToLetter('V')), 2);
  EXPECT_EQ(full_bag.scoreless_turns(), 1);
  const MoveRecord& record = full_bag.history().back();
  EXPECT_EQ(record.drawn, RackOrDie("EE"));
  EXPECT_EQ(record.rack_before, RackOrDie("VVVVVVV"));
  EXPECT_EQ(record.score, 0);
}

TEST(GameTest, IllegalMovesLeaveStateUnchanged) {
  const Gaddag gaddag = SmallLexicon();
  Game game(gaddag, MakeSetup("ABEEEEE", "AB", "VVVVVVVV"), 1);
  const Game before = game;
  const struct {
    const char* move;
    MoveError error;
  } kCases[] = {
      {"8H BA", MoveError::kInvalidWord},
      {"8H ABV", MoveError::kNotInRack},
      {"1A AB", MoveError::kFirstMoveOffCenter},
      {"-VV", MoveError::kNotInRack},
  };
  for (const auto& c : kCases) {
    const PlayResult result = game.Play(ParseOrDie(c.move));
    EXPECT_EQ(result.error, PlayError::kIllegalMove) << c.move;
    EXPECT_EQ(result.move_error, c.error) << c.move;
    EXPECT_FALSE(PlayErrorMessage(result).empty());
    ExpectSameState(game, before);
  }
  EXPECT_TRUE(game.history().empty());
}

TEST(GameTest, PlacementScoresAndDraws) {
  const Gaddag gaddag = SmallLexicon();
  Game game(gaddag, MakeSetup("ABEEEEE", "AB", "VVVVVVVV"), 1);
  // A caller-supplied score is ignored; ValidateMove() rescores.
  Move move = ParseOrDie("8H AB");
  move.score = 1000;
  ASSERT_TRUE(game.Play(move).ok());
  EXPECT_EQ(game.score(0), 8);  // (1 + 3) x 2 on the centre star
  EXPECT_EQ(game.rack(0), RackOrDie("EEEEEVV"));
  EXPECT_EQ(game.bag().Size(), 6);
  EXPECT_EQ(game.to_move(), 1);
  EXPECT_EQ(game.scoreless_turns(), 0);
  const MoveRecord& record = game.history().back();
  EXPECT_EQ(record.cumulative, 8);
  EXPECT_EQ(record.bag_before, 8);
  EXPECT_EQ(record.drawn, RackOrDie("VV"));
}

TEST(GameTest, SixScorelessTurnsEndTheGame) {
  for (const EndRules rules : {EndRules::kNaspa, EndRules::kWespa}) {
    const Gaddag gaddag = SmallLexicon();
    Game game(gaddag, MakeSetup("VVVVVVV", "AB", ""), 1, {.end_rules = rules});
    for (int turn = 0; turn < kScorelessTurnsToEnd - 1; ++turn) {
      ASSERT_TRUE(game.Play(Move::Pass()).ok());
    }
    EXPECT_FALSE(game.IsOver());
    ASSERT_TRUE(game.Play(Move::Pass()).ok());
    EXPECT_EQ(game.status(), GameStatus::kScorelessTurns);
    // Both rule sets: each player loses their own rack value.
    EXPECT_EQ(game.end_adjustment(0), -28);
    EXPECT_EQ(game.end_adjustment(1), -4);
    EXPECT_EQ(game.score(0), -28);
    EXPECT_EQ(game.score(1), -4);
    EXPECT_EQ(game.Play(Move::Pass()).error, PlayError::kGameOver);
  }
}

TEST(GameTest, ZeroPointPlaysAndExchangesAreScoreless) {
  const Gaddag gaddag = SmallLexicon();
  Game game(gaddag, MakeSetup("??", "AB", "VVVVVVVVVVVVVVVVVV"), 1);
  ASSERT_TRUE(game.Play(ParseOrDie("8H ab")).ok());
  EXPECT_EQ(game.history().back().score, 0);
  EXPECT_EQ(game.scoreless_turns(), 1);
  ASSERT_TRUE(game.Play(ParseOrDie("-AB")).ok());
  EXPECT_EQ(game.scoreless_turns(), 2);
  for (int turn = 2; turn < kScorelessTurnsToEnd; ++turn) {
    EXPECT_FALSE(game.IsOver());
    ASSERT_TRUE(game.Play(Move::Pass()).ok());
  }
  EXPECT_EQ(game.status(), GameStatus::kScorelessTurns);
}

TEST(GameTest, ScoringPlayResetsScorelessCount) {
  const Gaddag gaddag = SmallLexicon();
  Game game(gaddag, MakeSetup("VVVVVVV", "ABEEEEE", "VVVVVVVV"), 1);
  for (int turn = 0; turn < 5; ++turn) {
    ASSERT_TRUE(game.Play(Move::Pass()).ok());
  }
  ASSERT_TRUE(game.Play(ParseOrDie("8H AB")).ok());
  EXPECT_EQ(game.scoreless_turns(), 0);
  EXPECT_FALSE(game.IsOver());
}

// alice exchanges, bob passes, alice bingos to empty the bag, bob goes out
// holding AB while alice is stuck with VVVVVVV (28 points).
Game PlayGoingOutGame(const Gaddag& gaddag, EndRules rules) {
  Game game(gaddag, MakeSetup("VVVVVVV", "AB", "EEEEEEE"), 3,
            {.end_rules = rules});
  for (const char* move : {"-VVVVVVV", "-", "8B EEEEEEE", "C6 AB(E)"}) {
    const PlayResult result = game.Play(ParseOrDie(move));
    EXPECT_TRUE(result.ok()) << move << ": " << PlayErrorMessage(result);
  }
  return game;
}

TEST(GameTest, GoingOutNaspaDoublesOpponentRack) {
  const Gaddag gaddag = SmallLexicon();
  const Game game = PlayGoingOutGame(gaddag, EndRules::kNaspa);
  EXPECT_EQ(game.status(), GameStatus::kWentOut);
  EXPECT_EQ(game.went_out(), 1);
  EXPECT_EQ(game.end_adjustment(1), 56);
  EXPECT_EQ(game.end_adjustment(0), 0);
  EXPECT_EQ(game.score(0), 66);
  EXPECT_EQ(game.score(1), 8 + 56);
}

TEST(GameTest, GoingOutWespaTransfersOpponentRack) {
  const Gaddag gaddag = SmallLexicon();
  const Game game = PlayGoingOutGame(gaddag, EndRules::kWespa);
  EXPECT_EQ(game.status(), GameStatus::kWentOut);
  EXPECT_EQ(game.end_adjustment(1), 28);
  EXPECT_EQ(game.end_adjustment(0), -28);
  EXPECT_EQ(game.score(0), 66 - 28);
  EXPECT_EQ(game.score(1), 8 + 28);
  // Same spread swing as NASPA.
  EXPECT_EQ(game.spread(1),
            PlayGoingOutGame(gaddag, EndRules::kNaspa).spread(1));
}

TEST(GameTest, ViewHidesOpponentRack) {
  const Gaddag gaddag = SmallLexicon();
  const Game game(gaddag, MakeSetup("VVVVVVV", "AB", "EEEEEEE"), 1);
  const PlayerView view = game.View(1);
  EXPECT_EQ(view.rack, RackOrDie("AB"));
  EXPECT_EQ(view.opponent_rack_size, 7);
  EXPECT_EQ(view.bag_size, 7);
  EXPECT_EQ(view.unseen_size, 14);
  EXPECT_EQ(view.unseen[*CharToLetter('V')], 7);
  EXPECT_EQ(view.unseen[*CharToLetter('E')], 7);
  EXPECT_TRUE(view.CanExchange());
  EXPECT_EQ(view.position, &game.position());
}

class EnableGameTest : public ::testing::Test {
 protected:
  void SetUp() override {
    gaddag_ = EnableGaddag();
    if (gaddag_ == nullptr) GTEST_SKIP() << "ENABLE not found";
  }
  const Gaddag* gaddag_ = nullptr;
};

// Board + racks + bag always hold exactly the standard 100 tiles, and each
// player's view of the unseen tiles matches what tile tracking deduces.
TEST_F(EnableGameTest, TilesAreConservedInRandomGames) {
  MoveGenerator generator(*gaddag_);
  std::vector<Move> moves;
  int checked = 0;
  for (std::uint64_t seed = 1; seed <= 4 * testing::kScale; ++seed) {
    Rng rng(seed * 7919);
    Game game(*gaddag_, seed,
              {.end_rules = seed % 2 ? EndRules::kNaspa : EndRules::kWespa});
    while (true) {
      std::array<int, kAlphabetSize> counts{};
      for (int row = 0; row < kBoardSize; ++row) {
        for (int col = 0; col < kBoardSize; ++col) {
          const Square& sq = game.board().At(row, col);
          if (!sq.IsEmpty()) ++counts[sq.is_blank ? kBlank : sq.letter];
        }
      }
      for (Letter l = 0; l < kAlphabetSize; ++l) {
        counts[l] += game.rack(0).Count(l) + game.rack(1).Count(l) +
                     game.bag().Count(l);
      }
      ASSERT_EQ(counts, kTileCounts) << "seed " << seed << " turn "
                                     << game.turn();
      for (int p = 0; p < kNumPlayers; ++p) {
        const PlayerView view = game.View(p);
        ASSERT_EQ(view.unseen, UnseenTiles(game.board(), game.rack(p)));
      }
      ++checked;
      if (game.IsOver()) break;

      const PlayerView view = game.ViewForMover();
      generator.Generate(game.position(), view.rack, moves,
                         {.include_exchanges = view.CanExchange(),
                          .include_pass = true});
      std::size_t placements = 0;
      while (moves[placements].type == MoveType::kPlace) ++placements;
      // Mostly placements, so games reach the end of the bag, with enough
      // exchanges and passes mixed in to exercise those paths.
      const bool place = placements > 0 && rng.Below(10) < 8;
      const auto count =
          static_cast<std::uint32_t>(place ? placements : moves.size());
      const PlayResult result = game.Play(moves[rng.Below(count)]);
      ASSERT_TRUE(result.ok()) << PlayErrorMessage(result);
    }
  }
  EXPECT_GT(checked, 40 * testing::kScale);
}

}  // namespace
}  // namespace scrabble
