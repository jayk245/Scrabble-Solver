#include "scrabble/simulation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "scrabble/bot.h"
#include "scrabble/game.h"
#include "scrabble/leave_evaluator.h"
#include "scrabble/move_validation.h"
#include "scrabble/position.h"
#include "test_util.h"

namespace scrabble {
namespace {

using testing::EnableGaddag;

// Rollouts are expensive under the sanitizers, so iteration counts follow
// kScale.
constexpr int kIterations = 16 * testing::kScale;

SimulationConfig Config(int iterations, int threads = 1) {
  SimulationConfig config;
  config.enabled = true;
  config.iterations = iterations;
  config.min_iterations = std::min(32, iterations);
  config.threads = threads;
  return config;
}

void ExpectSameResult(const SimulationResult& a, const SimulationResult& b) {
  ASSERT_EQ(a.candidates.size(), b.candidates.size());
  EXPECT_EQ(a.best, b.best);
  EXPECT_EQ(a.best_move, b.best_move);
  EXPECT_EQ(a.iterations, b.iterations);
  EXPECT_EQ(a.rollouts, b.rollouts);
  EXPECT_EQ(a.stop, b.stop);
  for (std::size_t i = 0; i < a.candidates.size(); ++i) {
    const CandidateStats& x = a.candidates[i];
    const CandidateStats& y = b.candidates[i];
    // Bitwise: the reduction order is fixed, so even rounding agrees.
    EXPECT_EQ(x.iterations, y.iterations) << i;
    EXPECT_EQ(x.mean, y.mean) << i;
    EXPECT_EQ(x.stddev, y.stddev) << i;
    EXPECT_EQ(x.diff_from_best, y.diff_from_best) << i;
    EXPECT_EQ(x.diff_std_error, y.diff_std_error) << i;
    EXPECT_EQ(x.pruned, y.pruned) << i;
  }
}

class SimulationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    gaddag_ = EnableGaddag();
    if (gaddag_ == nullptr) GTEST_SKIP() << "ENABLE not found";
  }

  // A game played by the static bot for `turns` moves from `seed`.
  Game MidGame(std::uint64_t seed, int turns) const {
    Game game(*gaddag_, seed);
    Bot bot(*gaddag_, BotConfig{});
    Rng rng(seed);
    for (int i = 0; i < turns && !game.IsOver(); ++i) {
      EXPECT_TRUE(game.Play(bot.ChooseMove(game.ViewForMover(), rng)).ok());
    }
    return game;
  }

  // The top `n` static moves for the mover.
  std::vector<Move> TopMoves(const PlayerView& view, std::size_t n) const {
    Bot bot(*gaddag_, BotConfig{});
    std::vector<Move> moves;
    bot.RankMoves(view, n, moves);
    return moves;
  }

  const Gaddag* gaddag_ = nullptr;
};

TEST(SimulationConfigTest, RejectsOutOfRangeSettings) {
  const Gaddag gaddag = Gaddag::Build(std::vector<std::string>{"AB"});
  const auto leaves = DefaultLeaves();
  EXPECT_NO_THROW(Simulator(gaddag, leaves, Config(10)));
  EXPECT_THROW(Simulator(gaddag, nullptr, Config(10)), std::invalid_argument);
  SimulationConfig config = Config(10);
  config.candidates = 0;
  EXPECT_THROW(Simulator(gaddag, leaves, config), std::invalid_argument);
  config = Config(0);
  EXPECT_THROW(Simulator(gaddag, leaves, config), std::invalid_argument);
  config = Config(10);
  config.plies = -1;
  EXPECT_THROW(Simulator(gaddag, leaves, config), std::invalid_argument);
  config.plies = kMaxSimulationPlies + 1;
  EXPECT_THROW(Simulator(gaddag, leaves, config), std::invalid_argument);
  config = Config(10);
  config.min_iterations = 1;
  EXPECT_THROW(Simulator(gaddag, leaves, config), std::invalid_argument);
}

TEST(SimulationSeedTest, DistinctPerIterationAndStream) {
  EXPECT_EQ(SimulationSeed(7, 3), SimulationSeed(7, 3));
  EXPECT_NE(SimulationSeed(7, 3), SimulationSeed(7, 4));
  EXPECT_NE(SimulationSeed(7, 3), SimulationSeed(8, 3));
  EXPECT_NE(SimulationSeed(7, 3, 1), SimulationSeed(7, 3, 2));
  EXPECT_NE(SimulationSeed(7, 3, 0), SimulationSeed(7, 3, 1));
}

// The sampled opponent rack plus the sampled bag is always exactly the
// unseen pool, with the opponent's rack of the right size.
TEST_F(SimulationTest, SamplesConserveTheUnseenTiles) {
  Game game(*gaddag_, 3);
  Bot bot(*gaddag_, BotConfig{});
  Rng rng(3);
  Rng sampler(99);
  SimulationSample sample;
  int checked = 0;
  while (!game.IsOver()) {
    const PlayerView view = game.ViewForMover();
    ASSERT_TRUE(CanSimulate(view));
    for (int i = 0; i < 20; ++i) {
      DrawSample(view, sampler, sample);
      ASSERT_EQ(sample.opponent.Size(), view.opponent_rack_size);
      ASSERT_EQ(sample.bag_size, view.bag_size);
      std::array<int, kAlphabetSize> counts{};
      for (Letter l = 0; l < kAlphabetSize; ++l) {
        counts[l] = sample.opponent.Count(l);
      }
      for (int t = 0; t < sample.bag_size; ++t) {
        ++counts[sample.bag[static_cast<std::size_t>(t)]];
      }
      for (Letter l = 0; l < kAlphabetSize; ++l) {
        ASSERT_EQ(counts[l], view.unseen[l]) << LetterToChar(l);
      }
      ++checked;
    }
    ASSERT_TRUE(game.Play(bot.ChooseMove(view, rng)).ok());
  }
  EXPECT_GT(checked, 300);

  PlayerView bad = game.ViewForMover();
  bad.unseen_size += 1;
  EXPECT_FALSE(CanSimulate(bad));
}

// Every tile is equally likely to land on the opponent's rack.
TEST_F(SimulationTest, OpponentRacksAreUniform) {
  const Game game(*gaddag_, 1);
  const PlayerView view = game.ViewForMover();
  Rng rng(5);
  SimulationSample sample;
  constexpr int kSamples = 20'000;
  std::array<int, kAlphabetSize> held{};
  for (int i = 0; i < kSamples; ++i) {
    DrawSample(view, rng, sample);
    for (Letter l = 0; l < kAlphabetSize; ++l) held[l] += sample.opponent.Count(l);
  }
  for (Letter l = 0; l < kAlphabetSize; ++l) {
    if (view.unseen[l] == 0) continue;
    const double p = 7.0 / view.unseen_size;
    const double expected = kSamples * view.unseen[l] * p;
    // Binomial-ish spread; five standard deviations.
    EXPECT_NEAR(held[l], expected, 5.0 * std::sqrt(expected) + 1.0)
        << LetterToChar(l);
  }
}

TEST_F(SimulationTest, SingleCandidateIsReturnedWithoutRollouts) {
  const Game game = MidGame(4, 6);
  const PlayerView view = game.ViewForMover();
  const std::vector<Move> moves = TopMoves(view, 5);
  ASSERT_GE(moves.size(), 2u);
  Simulator simulator(*gaddag_, DefaultLeaves(), Config(kIterations));
  const std::vector<Move> one = {moves[1]};
  const SimulationResult& result = simulator.Run(view, one, 1);
  EXPECT_EQ(result.best, 0);
  EXPECT_EQ(result.best_move, moves[1]);
  EXPECT_EQ(result.rollouts, 0);
  EXPECT_EQ(result.stop, SimulationStop::kSingleCandidate);

  // A bot simulating one candidate plays its static choice.
  BotConfig config;
  config.simulation = Config(kIterations);
  config.simulation.candidates = 1;
  Bot simulating(*gaddag_, config);
  Bot static_bot(*gaddag_, BotConfig{});
  Rng rng(1);
  const Move expected = static_bot.ChooseMove(view, rng);
  const Move chosen = simulating.ChooseMove(view, rng);
  // The top scorer is added as a second candidate unless it is the best
  // static move; either way the static choice must be a candidate.
  ASSERT_NE(simulating.last_simulation(), nullptr);
  const SimulationResult& sim = *simulating.last_simulation();
  EXPECT_EQ(sim.candidates[0].move, expected);
  if (sim.candidates.size() == 1) EXPECT_EQ(chosen, expected);
}

// With no plies after the candidate a rollout is the candidate's static
// equity, whatever the sample.
TEST_F(SimulationTest, ZeroPliesReproducesStaticEquity) {
  const Game game = MidGame(5, 8);
  const PlayerView view = game.ViewForMover();
  const std::vector<Move> moves = TopMoves(view, 8);
  SimulationConfig config = Config(kIterations);
  config.plies = 0;
  config.prune_z = 0.0;
  Simulator simulator(*gaddag_, DefaultLeaves(), config);
  const SimulationResult& result = simulator.Run(view, moves, 3);
  for (std::size_t i = 0; i < moves.size(); ++i) {
    EXPECT_NEAR(result.candidates[i].mean, moves[i].equity, 1e-9) << i;
    EXPECT_NEAR(result.candidates[i].stddev, 0.0, 1e-6) << i;
  }
  EXPECT_EQ(result.best, 0);
}

// Bit-identical results for equal seeds, for a reused simulator, and for
// any thread count.
TEST_F(SimulationTest, DeterministicAndIndependentOfThreads) {
  const Game game = MidGame(6, 7);
  const PlayerView view = game.ViewForMover();
  const std::vector<Move> moves = TopMoves(view, 6);
  Simulator one(*gaddag_, DefaultLeaves(), Config(2 * kIterations, 1));
  Simulator four(*gaddag_, DefaultLeaves(), Config(2 * kIterations, 4));
  const SimulationResult first = one.Run(view, moves, 42);
  ExpectSameResult(first, one.Run(view, moves, 42));
  const SimulationResult threaded = four.Run(view, moves, 42);
  EXPECT_EQ(threaded.threads, 4);
  ExpectSameResult(first, threaded);
  const SimulationResult other = one.Run(view, moves, 43);
  EXPECT_NE(first.candidates[0].mean, other.candidates[0].mean);
}

// Rollouts run on per-thread copies: the view's position is bit-identical
// afterwards.
TEST_F(SimulationTest, NeverMutatesTheView) {
  const Game game = MidGame(7, 9);
  const PlayerView view = game.ViewForMover();
  const Position before = game.position();
  const PlayerView view_before = view;
  const std::vector<Move> moves = TopMoves(view, 4);
  Simulator simulator(*gaddag_, DefaultLeaves(), Config(kIterations, 2));
  simulator.Run(view, moves, 1);
  EXPECT_EQ(game.position().board(), before.board());
  EXPECT_EQ(game.position().cross_checks(), before.cross_checks());
  EXPECT_EQ(view.rack, view_before.rack);
  EXPECT_EQ(view.unseen, view_before.unseen);
  EXPECT_EQ(view.position, view_before.position);
}

// CRN does not change what is estimated, only how precisely: the mean
// difference between two candidates agrees with independent sampling, and
// its variance is lower.
TEST_F(SimulationTest, CommonRandomNumbersReduceVariance) {
  const Game game = MidGame(8, 6);
  const PlayerView view = game.ViewForMover();
  const std::vector<Move> moves = TopMoves(view, 2);
  ASSERT_EQ(moves.size(), 2u);
  SimulationConfig config = Config(4 * kIterations);
  config.prune_z = 0.0;
  Simulator crn(*gaddag_, DefaultLeaves(), config);
  config.common_random_numbers = false;
  Simulator independent(*gaddag_, DefaultLeaves(), config);
  const SimulationResult with = crn.Run(view, moves, 11);
  const SimulationResult without = independent.Run(view, moves, 11);
  // Candidate 0 minus candidate 1, whichever came out best.
  const auto difference = [](const SimulationResult& result) {
    const CandidateStats& other =
        result.candidates[1 - static_cast<std::size_t>(result.best)];
    return result.best == 0 ? other.diff_from_best : -other.diff_from_best;
  };
  const CandidateStats& a =
      with.candidates[1 - static_cast<std::size_t>(with.best)];
  const CandidateStats& b =
      without.candidates[1 - static_cast<std::size_t>(without.best)];
  const double combined = std::hypot(a.diff_std_error, b.diff_std_error);
  EXPECT_NEAR(difference(with), difference(without), 4.0 * combined + 1e-9);
  // Pairing also leaves each candidate's own mean unbiased.
  EXPECT_NEAR(with.candidates[0].mean, without.candidates[0].mean,
              4.0 * std::hypot(with.candidates[0].std_error,
                               without.candidates[0].std_error));
  EXPECT_LT(a.diff_std_error, b.diff_std_error);
}

// A clearly best move survives pruning, is chosen, and pruning saves
// rollouts on the clearly worse ones.
TEST_F(SimulationTest, PruningKeepsTheClearBest) {
  const Game game = MidGame(9, 5);
  const PlayerView view = game.ViewForMover();
  std::vector<Move> ranked = TopMoves(view, 100'000);
  ASSERT_GT(ranked.size(), 6u);
  // The static best against the four worst moves (pass and exchanges of
  // the best tiles, or two-point plays): tens of points apart.
  std::vector<Move> candidates = {ranked.front()};
  candidates.insert(candidates.end(), ranked.end() - 4, ranked.end());
  ASSERT_GT(candidates[0].equity - candidates[1].equity, 15.0);

  SimulationConfig config = Config(4 * kIterations);
  config.min_iterations = 16;
  Simulator pruning(*gaddag_, DefaultLeaves(), config);
  const SimulationResult& result = pruning.Run(view, candidates, 5);
  EXPECT_EQ(result.best, 0);
  EXPECT_FALSE(result.candidates[0].pruned);
  EXPECT_EQ(result.candidates[0].iterations, result.iterations);
  int pruned = 0;
  for (const CandidateStats& stats : result.candidates) pruned += stats.pruned;
  EXPECT_EQ(pruned, 4);
  EXPECT_EQ(result.stop, SimulationStop::kSingleCandidate);
  EXPECT_LT(result.rollouts, 4 * kIterations * 5);
  for (std::size_t i = 1; i < candidates.size(); ++i) {
    EXPECT_GT(result.candidates[i].diff_from_best, 0.0) << i;
  }
}

// The time limit stops the run between rounds.
TEST_F(SimulationTest, TimeLimitStopsTheRun) {
  const Game game = MidGame(10, 6);
  const PlayerView view = game.ViewForMover();
  const std::vector<Move> moves = TopMoves(view, 3);
  SimulationConfig config = Config(1'000'000);
  config.prune_z = 0.0;
  config.time_limit_ms = 1;
  Simulator simulator(*gaddag_, DefaultLeaves(), config);
  const SimulationResult& result = simulator.Run(view, moves, 1);
  EXPECT_EQ(result.stop, SimulationStop::kTime);
  EXPECT_LT(result.iterations, 1'000'000);
  EXPECT_EQ(result.iterations % kSimulationRoundIterations, 0);
}

// Full games with simulation (and the endgame solver) on for one side:
// every move is legal, and equal seeds replay identically.
TEST_F(SimulationTest, BotPlaysLegalGames) {
  BotConfig config;
  config.simulation = Config(kIterations / 2);
  config.simulation.candidates = 4;
  config.simulation.min_iterations = 8;
  config.endgame.enabled = true;
  config.endgame.node_limit = 500 * testing::kScale;
  const auto play = [&](std::uint64_t seed) {
    Bot simulating(*gaddag_, config);
    Bot static_bot(*gaddag_, BotConfig{});
    Rng rng(seed);
    Game game(*gaddag_, seed);
    int simulated = 0;
    while (!game.IsOver()) {
      const PlayerView view = game.ViewForMover();
      Bot& bot = game.to_move() == 0 ? simulating : static_bot;
      const Move move = bot.ChooseMove(view, rng);
      Move checked = move;
      EXPECT_EQ(ValidateMove(game.board(), *gaddag_, view.rack, checked),
                MoveError::kOk)
          << move.ToString();
      if (move.type == MoveType::kExchange) EXPECT_TRUE(view.CanExchange());
      if (&bot == &simulating && bot.last_simulation() != nullptr) {
        ++simulated;
        EXPECT_EQ(bot.last_simulation()->best_move, move);
      }
      const PlayResult result = game.Play(move);
      EXPECT_TRUE(result.ok()) << move.ToString();
      if (!result.ok()) break;
    }
    EXPECT_GT(simulated, 3);
    return game.history().size() * 1000u +
           static_cast<std::size_t>(game.score(0));
  };
  for (std::uint64_t seed = 1;
       seed <= static_cast<std::uint64_t>((testing::kScale + 4) / 5); ++seed) {
    EXPECT_EQ(play(seed), play(seed));
  }
}

}  // namespace
}  // namespace scrabble
