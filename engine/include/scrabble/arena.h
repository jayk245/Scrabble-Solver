// Self-play arena: many games between two bot configurations, in parallel.
//
// The arena is how bot changes are measured (A/B testing configurations of
// the one Bot) and how training data for learned leave values is produced.
//
// Determinism: game i depends only on (ArenaConfig::seed, i). Each worker
// thread owns its Game, both Bots and an Rng, and reseeds them per game, so
// results, the JSONL log and GCG files are byte-identical for any thread
// count. Config A sits in seat 0 (moves first) in even-numbered games and
// in seat 1 in odd-numbered games.
//
// Training log (JSONL, one object per move, games in index order and moves
// in play order). Every field is always present:
//
//   game         int     game index, 0-based
//   seed         int     the arena seed (with `game`, identifies the game)
//   turn         int     move index within the game, 0-based
//   player       int     seat of the mover; seat 0 moved first
//   bot          string  "A" or "B": which configuration moved
//   rack         string  mover's rack before the move, canonical
//                        (A-Z sorted, '?' = blank, last)
//   move         string  notation, e.g. "8D RETAINS", "H8 (A)X" (parenthesised
//                        letters were already on the board; lowercase =
//                        blank), "-ABC" (exchange) or "-" (pass)
//   type         string  "place", "exchange" or "pass"
//   score        int     points scored by the move
//   equity       number  the mover's static estimate (score + leave term)
//   leave        string  tiles kept, canonical; "" when all were played
//   bag          int     tiles in the bag before the move
//   spread       int     mover's score minus opponent's, before the move
//   final_spread int     mover's final score minus opponent's, after
//                        end-of-game adjustments
//
// The leave model in M6 regresses (final_spread - spread - score), the
// points that followed this move, on `leave`, using `bag` to weight or
// bucket by game phase.

#ifndef SCRABBLE_ARENA_H_
#define SCRABBLE_ARENA_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <ostream>
#include <span>
#include <vector>

#include "scrabble/bot.h"
#include "scrabble/gaddag.h"
#include "scrabble/game.h"

namespace scrabble {

struct ArenaConfig {
  int games = 100;
  // Worker threads; values < 1 mean one. More threads than games is fine.
  int threads = 1;
  std::uint64_t seed = 1;
  GameConfig game;
  BotConfig bot_a;
  BotConfig bot_b;
  // When non-empty, every game is written to <gcg_dir>/game_NNNNNN.gcg.
  // The directory is created if needed.
  std::filesystem::path gcg_dir;
  // When non-null, receives the JSONL training log (schema above). Written
  // under a lock, in game order, as games complete.
  std::ostream* log = nullptr;
};

// The outcome of one game, from configuration A's point of view.
struct ArenaGame {
  int index = 0;
  int a_seat = 0;                   // 0 if A moved first
  std::array<int, 2> score{};       // final scores: [0] = A, [1] = B
  std::array<int, 2> bingos{};      // seven-tile plays: [0] = A, [1] = B
  int turns = 0;
  GameStatus end = GameStatus::kInProgress;

  int spread() const { return score[0] - score[1]; }
  bool operator==(const ArenaGame&) const = default;
};

// Aggregate statistics for configuration A versus B. Confidence intervals
// are 95% normal approximations (1.96 standard errors).
struct ArenaSummary {
  int games = 0;
  int wins = 0;  // for A
  int losses = 0;
  int ties = 0;
  double win_rate = 0.0;     // (wins + ties / 2) / games
  double win_rate_ci = 0.0;  // half-width
  double mean_score_a = 0.0;
  double mean_score_a_ci = 0.0;
  double mean_score_b = 0.0;
  double mean_score_b_ci = 0.0;
  double mean_spread = 0.0;  // A minus B
  double mean_spread_ci = 0.0;
  double bingos_per_game_a = 0.0;
  double bingos_per_game_b = 0.0;
  double turns_per_game = 0.0;
};

struct ArenaResult {
  std::vector<ArenaGame> games;  // indexed by game number
  ArenaSummary summary;
  double seconds = 0.0;  // wall time of the run
};

// Seed for game `index` of a run seeded with `seed`. Splitmix-style mixing,
// so neighbouring indices get unrelated streams.
std::uint64_t ArenaGameSeed(std::uint64_t seed, int index);

// Plays config.games games and summarises them. Throws whatever a worker
// throws (e.g. std::runtime_error if a GCG file cannot be written, or
// std::logic_error if a bot returns an illegal move) after all workers stop.
ArenaResult RunArena(const Gaddag& gaddag, const ArenaConfig& config);

ArenaSummary Summarize(std::span<const ArenaGame> games);

}  // namespace scrabble

#endif  // SCRABBLE_ARENA_H_
