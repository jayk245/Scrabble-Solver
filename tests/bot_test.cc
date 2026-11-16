#include "scrabble/bot.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "scrabble/gcg.h"
#include "scrabble/leave_evaluator.h"
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

TEST(BotTest, RejectsBadConfigurations) {
  const Gaddag gaddag = Gaddag::Build(std::vector<std::string>{"AB"});
  BotConfig simulation;
  simulation.simulation.enabled = true;
  EXPECT_NO_THROW(Bot(gaddag, simulation));
  simulation.simulation.plies = kMaxSimulationPlies + 1;
  EXPECT_THROW(Bot(gaddag, simulation), std::invalid_argument);
  simulation.simulation.plies = 2;
  simulation.simulation.threads = 0;
  EXPECT_THROW(Bot(gaddag, simulation), std::invalid_argument);
  BotConfig endgame;
  endgame.endgame.enabled = true;
  EXPECT_NO_THROW(Bot(gaddag, endgame));
  BotConfig no_leaves;
  no_leaves.leaves = nullptr;
  EXPECT_THROW(Bot(gaddag, no_leaves), std::invalid_argument);
}

TEST(BotTest, DefaultsToTheLearnedLeaves) {
  const BotConfig config;
  ASSERT_NE(config.leaves, nullptr);
  EXPECT_EQ(config.leaves, DefaultLeaves());  // one shared table
  EXPECT_EQ(config.leaves->name(), "learned");
  EXPECT_EQ(config.leaves->kind(), LeaveEvaluator::Kind::kTable);
  EXPECT_TRUE(config.leaves->has_model());
  EXPECT_EQ(config.leaves->size(), 29'163u);
  EXPECT_FALSE(config.simulation.enabled);
  EXPECT_FALSE(config.endgame.enabled);
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

// Bot-vs-bot games with two leave models: every chosen move is legal on
// its own terms (ValidateMove) and accepted by the referee.
TEST_F(EnableBotTest, AlwaysChoosesALegalMove) {
  Bot heuristic(*gaddag_, WithLeaves(LeaveEvaluator::Heuristic()));
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

// A requested move is simulated even when the static ranking would leave it
// out, so an analysis can compare it with the engine's choice.
TEST_F(EnableBotTest, SimulatesExtraCandidates) {
  BotConfig config;
  config.simulation.enabled = true;
  config.simulation.candidates = 2;
  config.simulation.iterations = 16;
  config.simulation.min_iterations = 16;
  config.simulation.threads = 1;
  Bot bot(*gaddag_, config);
  Game game(*gaddag_, 42);
  const PlayerView view = game.ViewForMover();
  std::vector<Move> ranked;
  bot.RankMoves(view, 50, ranked);
  ASSERT_GE(ranked.size(), 50u);
  const auto placement = std::find_if(
      ranked.rbegin(), ranked.rend(),
      [](const Move& move) { return move.type == MoveType::kPlace; });
  ASSERT_NE(placement, ranked.rend());
  const Move weak = *placement;
  Move illegal = weak;
  illegal.row = (illegal.row + 7) % kBoardSize;  // off the centre square

  const auto simulated = [&bot](const Move& wanted) {
    const SimulationResult* result = bot.last_simulation();
    if (result == nullptr) return false;
    return std::any_of(result->candidates.begin(), result->candidates.end(),
                       [&wanted](const CandidateStats& stats) {
                         Move move = stats.move;
                         move.equity = wanted.equity;
                         return move == wanted;
                       });
  };
  Rng rng(1);
  bot.ChooseMove(view, rng);
  EXPECT_FALSE(simulated(weak));

  bot.set_extra_candidates({weak, illegal});
  bot.ChooseMove(view, rng);
  EXPECT_TRUE(simulated(weak));
  EXPECT_FALSE(simulated(illegal));
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
