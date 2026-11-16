#include "scrabble/endgame.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "scrabble/bot.h"
#include "scrabble/game.h"
#include "scrabble/move_generator.h"
#include "scrabble/move_validation.h"
#include "test_util.h"

namespace scrabble {
namespace {

using testing::EnableGaddag;
using testing::MakeBoard;
using testing::RackOrDie;

// The oracle: full minimax over Game itself (no pruning, no table), so
// every rule (going out, scoreless turns, end adjustments) comes from the
// referee rather than from the solver's own terminal logic. Returns the
// final spread for the player to move, or nullopt once `budget` nodes have
// been spent.
std::optional<int> BruteForce(const Game& game, MoveGenerator& generator,
                              std::int64_t& budget) {
  if (game.IsOver()) return game.spread(game.to_move());
  if (--budget < 0) return std::nullopt;
  std::vector<Move> moves;
  generator.Generate(game.position(), game.rack(game.to_move()), moves,
                     {.include_exchanges = false, .include_pass = true});
  int best = -1'000'000;
  for (const Move& move : moves) {
    Game child = game;
    EXPECT_TRUE(child.Play(move).ok()) << move.ToString();
    const std::optional<int> value = BruteForce(child, generator, budget);
    if (!value) return std::nullopt;
    // The child's mover is the opponent; spreads are antisymmetric.
    best = std::max(best, -*value);
  }
  return best;
}

EndgameState StateOf(const Game& game) {
  EndgameState state;
  state.racks = {game.rack(0), game.rack(1)};
  state.to_move = game.to_move();
  state.spread = game.spread(game.to_move());
  state.scoreless_turns = game.scoreless_turns();
  state.end_rules = game.config().end_rules;
  return state;
}

EndgameResult SolveGame(EndgameSolver& solver, const Game& game,
                        const EndgameLimits& limits = {}) {
  Position position = game.position();
  return solver.Solve(position, StateOf(game), limits);
}

Game MakeEndgame(const Gaddag& gaddag, const Board& board,
                 const std::string& rack0, const std::string& rack1,
                 EndRules rules = EndRules::kNaspa) {
  GameSetup setup;
  setup.board = board;
  setup.racks = {RackOrDie(rack0), RackOrDie(rack1)};
  return Game(gaddag, setup, /*seed=*/1, GameConfig{.end_rules = rules});
}

// Plays `pv` on a copy of `game`; returns the mover's final spread, or
// nullopt if the line is illegal or does not end the game.
std::optional<int> PlayOut(const Game& game, const std::vector<Move>& pv) {
  Game copy = game;
  for (const Move& move : pv) {
    if (copy.IsOver() || !copy.Play(move).ok()) return std::nullopt;
  }
  if (!copy.IsOver()) return std::nullopt;
  return copy.spread(game.to_move());
}

// Boards and full racks at the moment the bag empties, from seeded static
// bot self-play: realistic endgame material.
struct SelfPlayEndgame {
  Board board;
  std::array<Rack, kNumPlayers> racks;
};

const std::vector<SelfPlayEndgame>& SelfPlayEndgames(const Gaddag& gaddag) {
  static const std::vector<SelfPlayEndgame> endgames = [&gaddag] {
    std::vector<SelfPlayEndgame> found;
    Bot bot(gaddag, BotConfig{});
    Rng rng(1);
    for (std::uint64_t seed = 1; found.size() < 8; ++seed) {
      Game game(gaddag, seed);
      while (!game.IsOver() && game.bag().Size() > 0) {
        EXPECT_TRUE(game.Play(bot.ChooseMove(game.ViewForMover(), rng)).ok());
      }
      if (game.IsOver()) continue;
      found.push_back({game.board(), {game.rack(0), game.rack(1)}});
    }
    return found;
  }();
  return endgames;
}

// A random sub-rack of `rack` with between 1 and `max_tiles` tiles.
Rack Truncate(const Rack& rack, int max_tiles, Rng& rng) {
  std::vector<Letter> tiles;
  for (Letter l = 0; l < kAlphabetSize; ++l) {
    for (int n = 0; n < rack.Count(l); ++n) tiles.push_back(l);
  }
  for (std::size_t i = tiles.size(); i > 1; --i) {
    std::swap(tiles[i - 1],
              tiles[rng.Below(static_cast<std::uint32_t>(i))]);
  }
  const int keep = 1 + static_cast<int>(rng.Below(static_cast<std::uint32_t>(
                           std::min<int>(max_tiles,
                                         static_cast<int>(tiles.size())))));
  Rack out;
  for (int i = 0; i < keep; ++i) out.Add(tiles[static_cast<std::size_t>(i)]);
  return out;
}

// A random endgame from self-play material: truncated racks, either end
// rule set, and up to three opening passes so the scoreless counter and the
// side to move vary.
Game RandomEndgame(const Gaddag& gaddag, int max_tiles, Rng& rng) {
  const auto& endgames = SelfPlayEndgames(gaddag);
  const SelfPlayEndgame& source =
      endgames[rng.Below(static_cast<std::uint32_t>(endgames.size()))];
  GameSetup setup;
  setup.board = source.board;
  setup.racks = {Truncate(source.racks[0], max_tiles, rng),
                 Truncate(source.racks[1], max_tiles, rng)};
  const EndRules rules =
      rng.Below(2) == 0 ? EndRules::kNaspa : EndRules::kWespa;
  Game game(gaddag, setup, 1, GameConfig{.end_rules = rules});
  const int passes = static_cast<int>(rng.Below(4));
  for (int i = 0; i < passes; ++i) EXPECT_TRUE(game.Play(Move::Pass()).ok());
  return game;
}

class EndgameTest : public ::testing::Test {
 protected:
  void SetUp() override {
    gaddag_ = EnableGaddag();
    if (gaddag_ == nullptr) GTEST_SKIP() << "ENABLE not found";
  }
  const Gaddag* gaddag_ = nullptr;
};

TEST_F(EndgameTest, MatchesBruteForceOnSmallSelfPlayEndgames) {
  EndgameSolver solver(*gaddag_);
  MoveGenerator generator(*gaddag_);
  Rng rng(2024);
  int compared = 0;
  for (int i = 0; i < 8 * testing::kScale; ++i) {
    const Game game = RandomEndgame(*gaddag_, i % 4 == 3 ? 3 : 2, rng);
    std::int64_t budget = 3'000 * testing::kScale;
    const std::optional<int> expected = BruteForce(game, generator, budget);
    if (!expected) continue;  // too big for the oracle
    const EndgameResult result = SolveGame(solver, game);
    ASSERT_TRUE(result.complete);
    ASSERT_EQ(result.value, *expected)
        << "racks " << game.rack(0).ToString() << " / "
        << game.rack(1).ToString() << " to move " << game.to_move() << "\n"
        << game.board().ToString();
    ++compared;
  }
  EXPECT_GE(compared, 4 * testing::kScale);
}

// Small crafted lexicon and boards let the oracle handle three-tile racks
// on both sides.
TEST(EndgameCraftedTest, MatchesBruteForceWithThreeTileRacks) {
  const Gaddag gaddag = Gaddag::Build(std::vector<std::string>{
      "AB", "BA", "ABA", "BAA", "AA", "AE", "EA", "BE", "BEE", "ABE",
      "BAE", "QI", "ZA", "ZEA"});
  const std::vector<std::string> tiles = {"A", "B", "E", "Q", "I", "Z", "?"};
  EndgameSolver solver(gaddag);
  MoveGenerator generator(gaddag);
  Rng rng(7);
  int compared = 0;
  for (int i = 0; i < 8 * testing::kScale; ++i) {
    std::string racks[2];
    for (std::string& rack : racks) {
      const int size = 1 + static_cast<int>(rng.Below(3));
      for (int t = 0; t < size; ++t) {
        rack += tiles[rng.Below(static_cast<std::uint32_t>(tiles.size()))];
      }
    }
    const Board board =
        rng.Below(2) == 0 ? MakeBoard({"8G ABA", "G8 (A)B"})
                          : MakeBoard({"8H BEE", "J6 BA(E)", "9F AB"});
    const EndRules rules =
        rng.Below(2) == 0 ? EndRules::kNaspa : EndRules::kWespa;
    const Game game = MakeEndgame(gaddag, board, racks[0], racks[1], rules);
    std::int64_t budget = 5'000 * testing::kScale;
    const std::optional<int> expected = BruteForce(game, generator, budget);
    if (!expected) continue;
    const EndgameResult result = SolveGame(solver, game);
    ASSERT_TRUE(result.complete);
    ASSERT_EQ(result.value, *expected) << racks[0] << " / " << racks[1];
    ++compared;
  }
  EXPECT_GE(compared, 4 * testing::kScale);
}

TEST_F(EndgameTest, TableAndOrderingDoNotChangeValues) {
  EndgameSolver full(*gaddag_);
  EndgameSolver no_table(*gaddag_, {.tt_size_mb = 0});
  EndgameSolver no_order(*gaddag_, {.order_moves = false});
  EndgameSolver bare(*gaddag_, {.tt_size_mb = 0, .order_moves = false});
  Rng rng(99);
  std::int64_t nodes_full = 0;
  std::int64_t nodes_bare = 0;
  for (int i = 0; i < 4 * testing::kScale; ++i) {
    const Game game = RandomEndgame(*gaddag_, 3, rng);
    const EndgameResult a = SolveGame(full, game);
    const EndgameResult b = SolveGame(no_table, game);
    const EndgameResult c = SolveGame(no_order, game);
    const EndgameResult d = SolveGame(bare, game);
    ASSERT_TRUE(a.complete && b.complete && c.complete && d.complete);
    EXPECT_EQ(a.value, b.value);
    EXPECT_EQ(a.value, c.value);
    EXPECT_EQ(a.value, d.value);
    nodes_full += a.nodes;
    nodes_bare += d.nodes;
  }
  EXPECT_LT(nodes_full, nodes_bare);
}

// The principal variation, played through the referee, scores exactly the
// solver's value; so does letting the solver choose every move for both
// sides.
TEST_F(EndgameTest, ValuesMatchWhatGameScores) {
  EndgameSolver solver(*gaddag_);
  Rng rng(5);
  for (int i = 0; i < 6 * testing::kScale; ++i) {
    const Game start = RandomEndgame(*gaddag_, 4, rng);
    const EndgameResult result = SolveGame(solver, start);
    ASSERT_TRUE(result.complete);
    EXPECT_EQ(PlayOut(start, result.pv), result.value);

    Game game = start;
    while (!game.IsOver()) {
      const EndgameResult step = SolveGame(solver, game);
      ASSERT_TRUE(game.Play(step.best_move).ok());
    }
    EXPECT_EQ(game.spread(start.to_move()), result.value);
  }
}

TEST_F(EndgameTest, SearchRestoresThePosition) {
  EndgameSolver solver(*gaddag_);
  Rng rng(11);
  for (int i = 0; i < 3; ++i) {
    const Game game = RandomEndgame(*gaddag_, 4, rng);
    Position position = game.position();
    solver.Solve(position, StateOf(game));
    EXPECT_EQ(position.board(), game.board());
    EXPECT_EQ(position.cross_checks(), game.position().cross_checks());
    // Also when a limit interrupts the search mid-tree.
    solver.Solve(position, StateOf(game), {.max_nodes = 500});
    EXPECT_EQ(position.board(), game.board());
    EXPECT_EQ(position.cross_checks(), game.position().cross_checks());
  }
}

TEST_F(EndgameTest, IncrementalHashMatchesFullRecompute) {
  const Zobrist& zobrist = Zobrist::Instance();
  MoveGenerator generator(*gaddag_);
  std::vector<Move> moves;
  Rng rng(3);
  for (const SelfPlayEndgame& source : SelfPlayEndgames(*gaddag_)) {
    Position position(*gaddag_, source.board);
    std::array<Rack, kNumPlayers> racks = source.racks;
    int side = 0;
    int scoreless = 0;
    std::uint64_t hash = zobrist.Hash(position.board(), racks, side, 0);
    while (!racks[0].IsEmpty() && !racks[1].IsEmpty()) {
      Rack& rack = racks[static_cast<std::size_t>(side)];
      generator.Generate(position, rack, moves, {.include_pass = true});
      // Passes are generated last; pick one now and then.
      const Move move =
          rng.Below(4) == 0
              ? moves.back()
              : moves[rng.Below(static_cast<std::uint32_t>(moves.size()))];
      const int next = move.score == 0 ? scoreless + 1 : 0;
      if (next >= kScorelessTurnsToEnd) break;
      const Rack after = LeaveAfter(rack, move);
      hash = zobrist.Update(hash, move, side, rack, after, scoreless, next);
      position.Apply(move);
      rack = after;
      scoreless = next;
      side = 1 - side;
      ASSERT_EQ(hash, zobrist.Hash(position.board(), racks, side, scoreless));
    }
  }
  // Side to move and the counter are part of the key.
  const SelfPlayEndgame& any = SelfPlayEndgames(*gaddag_).front();
  EXPECT_NE(zobrist.Hash(any.board, any.racks, 0, 0),
            zobrist.Hash(any.board, any.racks, 1, 0));
  EXPECT_NE(zobrist.Hash(any.board, any.racks, 0, 0),
            zobrist.Hash(any.board, any.racks, 0, 1));
}

TEST_F(EndgameTest, DeterministicRegardlessOfHistory) {
  EndgameSolver solver(*gaddag_);
  Rng rng(17);
  const Game game = RandomEndgame(*gaddag_, 4, rng);
  const Game other = RandomEndgame(*gaddag_, 4, rng);
  for (const EndgameLimits limits :
       {EndgameLimits{}, EndgameLimits{.max_nodes = 2'000}}) {
    const EndgameResult first = SolveGame(solver, game, limits);
    SolveGame(solver, other, limits);
    const EndgameResult second = SolveGame(solver, game, limits);
    EndgameSolver fresh(*gaddag_);
    const EndgameResult third = SolveGame(fresh, game, limits);
    for (const EndgameResult* r : {&second, &third}) {
      EXPECT_EQ(r->value, first.value);
      EXPECT_EQ(r->best_move, first.best_move);
      EXPECT_EQ(r->pv, first.pv);
      EXPECT_EQ(r->nodes, first.nodes);
      EXPECT_EQ(r->depth, first.depth);
      EXPECT_EQ(r->complete, first.complete);
    }
  }
}

TEST_F(EndgameTest, NodeLimitReturnsBestSoFar) {
  EndgameSolver solver(*gaddag_);
  const SelfPlayEndgame& source = SelfPlayEndgames(*gaddag_).front();
  GameSetup setup;
  setup.board = source.board;
  setup.racks = source.racks;  // full racks: far beyond 300 nodes
  const Game game(*gaddag_, setup, 1);
  const EndgameResult result = SolveGame(solver, game, {.max_nodes = 300});
  EXPECT_FALSE(result.complete);
  EXPECT_GE(result.depth, 1);
  Move checked = result.best_move;
  EXPECT_EQ(ValidateMove(game.board(), *gaddag_, game.rack(0), checked),
            MoveError::kOk);
  ASSERT_FALSE(result.pv.empty());
  EXPECT_EQ(result.pv.front().ToString(), result.best_move.ToString());
}

// Crafted rules cases on a tiny lexicon. Board: AB across at H8-I8.
class EndgameRulesTest : public ::testing::Test {
 protected:
  EndgameRulesTest()
      : gaddag_(Gaddag::Build(
            std::vector<std::string>{"AB", "ABA", "ABAB"})),
        board_(MakeBoard({"8H AB"})) {}

  const Gaddag gaddag_;
  const Board board_;
};

// A on the rack goes out for 5 (AB(A) across or A over B down); QZ left
// on the opponent's rack is worth 20, so the swing is 40 under both rule
// sets, although the two rule sets split it differently.
TEST_F(EndgameRulesTest, GoingOutBonusUnderBothRuleSets) {
  EndgameSolver solver(gaddag_);
  for (const EndRules rules : {EndRules::kNaspa, EndRules::kWespa}) {
    const Game game = MakeEndgame(gaddag_, board_, "A", "QZ", rules);
    const EndgameResult result = SolveGame(solver, game);
    ASSERT_TRUE(result.complete);
    EXPECT_EQ(result.value, 5 + 40);
    Game played = game;
    ASSERT_TRUE(played.Play(result.best_move).ok());
    ASSERT_EQ(played.status(), GameStatus::kWentOut);
    EXPECT_EQ(played.spread(0), result.value);
    EXPECT_EQ(played.score(1), rules == EndRules::kNaspa ? 0 : -20);
  }
}

// Holding QZ, the mover has no placement and must pass; the opponent then
// goes out.
TEST_F(EndgameRulesTest, ForcedPass) {
  EndgameSolver solver(gaddag_);
  const Game game = MakeEndgame(gaddag_, board_, "QZ", "A");
  const EndgameResult result = SolveGame(solver, game);
  ASSERT_TRUE(result.complete);
  EXPECT_EQ(result.best_move.type, MoveType::kPass);
  EXPECT_EQ(result.value, -(5 + 40));
  EXPECT_EQ(PlayOut(game, result.pv), result.value);
}

// Neither side can play: the game ends after six passes and each player
// loses their own rack (Q = 10, V = 4). The counter already standing at 3
// shortens the line, not the value.
TEST_F(EndgameRulesTest, BothSidesStuck) {
  EndgameSolver solver(gaddag_);
  Game game = MakeEndgame(gaddag_, board_, "Q", "V");
  EndgameResult result = SolveGame(solver, game);
  ASSERT_TRUE(result.complete);
  EXPECT_EQ(result.value, 4 - 10);
  EXPECT_EQ(result.pv.size(), 6u);
  EXPECT_EQ(PlayOut(game, result.pv), result.value);

  for (int i = 0; i < 3; ++i) ASSERT_TRUE(game.Play(Move::Pass()).ok());
  result = SolveGame(solver, game);  // player 1 (V) to move, 3 scoreless
  ASSERT_TRUE(result.complete);
  EXPECT_EQ(result.value, 10 - 4);
  EXPECT_EQ(result.pv.size(), 3u);
  EXPECT_EQ(PlayOut(game, result.pv), result.value);
}

// The classic stuck-Q endgame: the opponent can never play the Q, so the
// mover should not go out at once (ABAB for 8, +20 = 28) but play off one
// tile at a time while the opponent passes: AB(A) for 5, then (ABA)B for 8
// and out, +20 = 33.
TEST_F(EndgameRulesTest, StuckQLetsTheMoverPlayOffSlowly) {
  EndgameSolver solver(gaddag_);
  const Game game = MakeEndgame(gaddag_, board_, "AB", "Q");
  const EndgameResult result = SolveGame(solver, game);
  ASSERT_TRUE(result.complete);
  MoveGenerator generator(gaddag_);
  std::int64_t budget = 1'000'000;
  EXPECT_EQ(BruteForce(game, generator, budget), result.value);
  EXPECT_GE(result.value, 33);
  EXPECT_EQ(result.best_move.TilesUsed(), 1);
  EXPECT_EQ(PlayOut(game, result.pv), result.value);

  // The static rule would go out at once; the endgame bot does not.
  BotConfig config;
  config.endgame.enabled = true;
  Bot endgame_bot(gaddag_, config);
  Bot static_bot(gaddag_, BotConfig{});
  Rng rng(1);
  const PlayerView view = game.ViewForMover();
  EXPECT_EQ(endgame_bot.ChooseMove(view, rng).TilesUsed(), 1);
  EXPECT_EQ(static_bot.ChooseMove(view, rng).TilesUsed(), 2);
}

// The bot rebuilds the opponent's rack from the view and plays legal moves
// throughout real games.
TEST_F(EndgameTest, BotPlaysLegalEndgames) {
  BotConfig config;
  config.endgame.enabled = true;
  config.endgame.node_limit = 500 * testing::kScale;
  Bot bot(*gaddag_, config);
  Rng rng(1);
  for (std::uint64_t seed = 1; seed <= static_cast<std::uint64_t>(
                                           testing::kScale);
       ++seed) {
    Game game(*gaddag_, seed);
    while (!game.IsOver()) {
      const PlayerView view = game.ViewForMover();
      const Move move = bot.ChooseMove(view, rng);
      if (view.bag_size == 0) {
        EXPECT_NE(move.type, MoveType::kExchange);
        // Every view Game produces has a reconstructible opponent rack, so
        // the solver decides every empty-bag move and reports its search.
        const EndgameResult* result = bot.last_endgame();
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->best_move, move);
        EXPECT_GE(result->depth, 1);
      } else {
        EXPECT_EQ(bot.last_endgame(), nullptr);
      }
      ASSERT_TRUE(game.Play(move).ok()) << move.ToString();
    }
  }
}

}  // namespace
}  // namespace scrabble
