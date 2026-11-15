#include "scrabble/bot.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "scrabble/gcg.h"
#include "scrabble/move_validation.h"
#include "test_util.h"

namespace scrabble {
namespace {

using testing::EnableGaddag;
using testing::RackOrDie;

BotConfig WithLeaves(LeaveEvaluator leaves) {
  BotConfig config;
  config.leaves = std::make_shared<const LeaveEvaluator>(std::move(leaves));
  return config;
}

TEST(BotTest, RejectsUnimplementedStagesAndMissingLeaves) {
  const Gaddag gaddag = Gaddag::Build(std::vector<std::string>{"AB"});
  BotConfig simulation;
  simulation.simulation.enabled = true;
  EXPECT_THROW(Bot(gaddag, simulation), std::invalid_argument);
  BotConfig endgame;
  endgame.endgame.enabled = true;
  EXPECT_THROW(Bot(gaddag, endgame), std::invalid_argument);
  BotConfig no_leaves;
  no_leaves.leaves = nullptr;
  EXPECT_THROW(Bot(gaddag, no_leaves), std::invalid_argument);
}

TEST(BotTest, PassesWhenNothingElseIsLegal) {
  const Gaddag gaddag = Gaddag::Build(std::vector<std::string>{"AB"});
  const Position position(gaddag);
  PlayerView view;
  view.position = &position;
  view.rack = RackOrDie("QQ");
  view.bag_size = 6;  // too few to exchange
  Bot bot(gaddag, BotConfig{});
  Rng rng(1);
  EXPECT_EQ(bot.ChooseMove(view, rng).type, MoveType::kPass);
}

TEST(BotTest, ExchangesAwayABadRack) {
  const Gaddag gaddag = Gaddag::Build(std::vector<std::string>{"QI"});
  const Position position(gaddag);
  PlayerView view;
  view.position = &position;
  view.rack = RackOrDie("QIUUUVV");
  view.bag_size = 50;
  Bot bot(gaddag, BotConfig{});
  Rng rng(1);
  const Move move = bot.ChooseMove(view, rng);
  // QI scores 22 but keeps UUUVV, which is worth far less than nothing.
  ASSERT_EQ(move.type, MoveType::kExchange) << move.ToString();
  EXPECT_EQ(LeaveAfter(view.rack, move), Rack());
}

class EnableBotTest : public ::testing::Test {
 protected:
  void SetUp() override {
    gaddag_ = EnableGaddag();
    if (gaddag_ == nullptr) GTEST_SKIP() << "ENABLE not found";
  }
  const Gaddag* gaddag_ = nullptr;
};

// Checks the precomputed leave index against direct evaluation for every
// move, both with tiles left in the bag and with the bag empty.
TEST_F(EnableBotTest, EquityIsScorePlusLeave) {
  const auto leaves =
      std::make_shared<const LeaveEvaluator>(LeaveEvaluator::Heuristic());
  BotConfig config;
  config.leaves = leaves;
  Bot bot(*gaddag_, config);
  std::vector<Move> ranked;
  int positions = 0;
  for (std::uint64_t seed = 1; seed <= 2 * testing::kScale; ++seed) {
    Game game(*gaddag_, seed);
    Rng rng(seed);
    while (!game.IsOver()) {
      const PlayerView view = game.ViewForMover();
      bot.RankMoves(view, 1'000'000, ranked);
      ASSERT_FALSE(ranked.empty());
      int unseen_value = 0;
      for (Letter l = 0; l < kAlphabetSize; ++l) {
        unseen_value += view.unseen[l] * kTileValues[l];
      }
      for (std::size_t i = 0; i < ranked.size(); ++i) {
        const Move& move = ranked[i];
        const Rack leave = LeaveAfter(view.rack, move);
        double expected;
        if (view.bag_size > 0) {
          expected = leaves->Value(leave);
        } else {
          expected = leave.IsEmpty() ? 2.0 * unseen_value
                                     : -2.0 * leave.Score();
        }
        ASSERT_DOUBLE_EQ(move.equity, move.score + expected)
            << move.ToString() << " rack " << view.rack.ToString();
        if (i > 0) ASSERT_GE(ranked[i - 1].equity, move.equity);
      }
      ASSERT_TRUE(game.Play(bot.ChooseMove(view, rng)).ok());
      ++positions;
    }
  }
  EXPECT_GT(positions, 20);
}

// Bot-vs-bot games with both leave models: every chosen move is legal on
// its own terms (ValidateMove) and accepted by the referee.
TEST_F(EnableBotTest, AlwaysChoosesALegalMove) {
  Bot heuristic(*gaddag_, BotConfig{});
  Bot zero(*gaddag_, WithLeaves(LeaveEvaluator::Zero()));
  Rng rng(3);
  for (std::uint64_t seed = 100; seed < 100 + 3 * testing::kScale; ++seed) {
    Game game(*gaddag_, seed);
    while (!game.IsOver()) {
      const PlayerView view = game.ViewForMover();
      Bot& bot = game.to_move() == 0 ? heuristic : zero;
      const Move move = bot.ChooseMove(view, rng);
      Move checked = move;
      ASSERT_EQ(ValidateMove(game.board(), *gaddag_, view.rack, checked),
                MoveError::kOk)
          << move.ToString();
      if (move.type == MoveType::kExchange) ASSERT_TRUE(view.CanExchange());
      const PlayResult result = game.Play(move);
      ASSERT_TRUE(result.ok()) << move.ToString() << ": "
                               << PlayErrorMessage(result);
    }
    EXPECT_GT(game.turn(), 5);
  }
}

// With zero leaves and tiles in the bag, the bot simply maximises score.
TEST_F(EnableBotTest, ZeroLeavesMaximiseScore) {
  Bot bot(*gaddag_, WithLeaves(LeaveEvaluator::Zero()));
  MoveGenerator generator(*gaddag_);
  std::vector<Move> moves;
  Rng rng(1);
  Game game(*gaddag_, 42);
  while (!game.IsOver() && game.bag().Size() > 0) {
    const PlayerView view = game.ViewForMover();
    generator.Generate(game.position(), view.rack, moves);
    int best = 0;
    for (const Move& m : moves) best = std::max(best, m.score);
    const Move move = bot.ChooseMove(view, rng);
    EXPECT_EQ(move.score, best);
    ASSERT_TRUE(game.Play(move).ok());
  }
}

TEST_F(EnableBotTest, GamesAreDeterministicPerSeed) {
  const auto play = [this](std::uint64_t seed) {
    Bot bot(*gaddag_, BotConfig{});
    Rng rng(seed);
    Game game(*gaddag_, seed);
    while (!game.IsOver()) {
      EXPECT_TRUE(game.Play(bot.ChooseMove(game.ViewForMover(), rng)).ok());
    }
    GcgInfo info;
    info.players = {GcgPlayer{"p1", ""}, GcgPlayer{"p2", ""}};
    return ToGcg(game, info);
  };
  EXPECT_EQ(play(11), play(11));
  EXPECT_NE(play(11), play(12));
}

}  // namespace
}  // namespace scrabble
