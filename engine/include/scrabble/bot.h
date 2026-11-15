// The Scrabble bot.
//
// There is exactly one bot. It grows in strength by enabling stages through
// BotConfig rather than by adding bot classes, so any two strengths can be
// A/B tested in the arena by changing configuration alone:
//
//   1. Generate every legal move, including exchanges (when the bag allows)
//      and the pass.
//   2. Static evaluation: equity = score + leave value, from the configured
//      LeaveEvaluator (heuristic now, learned values from M6). Once the bag
//      is empty nothing more is drawn, so leave values no longer apply and
//      the leave term becomes the end-of-game rack swing instead: +2x the
//      opponent's rack value for going out, otherwise -2x the face value of
//      the tiles kept (the opponent will likely go out next). The 2x holds
//      for both EndRules, since they differ only in who carries the swing.
//   3. Simulation (M7, BotConfig::simulation): re-rank the top static
//      candidates by Monte Carlo playouts against racks sampled from the
//      unseen tiles.
//   4. Endgame (M8, BotConfig::endgame): with the bag empty, replace the
//      above with an exact search.
//
// Stages 3 and 4 are configuration hooks only; enabling them before they
// exist is rejected at construction rather than silently ignored.

#ifndef SCRABBLE_BOT_H_
#define SCRABBLE_BOT_H_

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "scrabble/game.h"
#include "scrabble/gaddag.h"
#include "scrabble/leave_evaluator.h"
#include "scrabble/move.h"
#include "scrabble/move_generator.h"
#include "scrabble/rack.h"
#include "scrabble/rng.h"

namespace scrabble {

// Monte Carlo simulation of the top static candidates (milestone 7). Each
// iteration samples an opponent rack and future draws from
// PlayerView::unseen, plays `plies` further moves with the static
// evaluator, and scores the candidate by its average resulting spread plus
// the final leave. Not yet implemented: `enabled` must be false.
struct SimulationConfig {
  bool enabled = false;
  int candidates = 10;    // top static moves to simulate
  int iterations = 1000;  // playouts per candidate (upper bound)
  int plies = 2;          // moves played after the candidate
};

// Exact endgame search once the bag is empty (milestone 8). Both racks are
// known then (the opponent's is PlayerView::unseen), so the position is a
// perfect-information game. Not yet implemented: `enabled` must be false.
struct EndgameConfig {
  bool enabled = false;
  int max_plies = 8;  // search depth limit
};

struct BotConfig {
  // Label used in logs, GCG files and reports.
  std::string name = "bot";
  // Shared so many bots (e.g. one per arena thread) can read one loaded
  // table. Must not be null.
  std::shared_ptr<const LeaveEvaluator> leaves =
      std::make_shared<const LeaveEvaluator>(LeaveEvaluator::Heuristic());
  SimulationConfig simulation;
  EndgameConfig endgame;
};

// Holds move-generation scratch buffers, so a Bot is used by one thread at
// a time; give each thread its own (they are cheap). ChooseMove() performs
// no heap allocation once its buffers have grown to the largest move list
// seen.
class Bot {
 public:
  // `gaddag` must outlive the bot. Throws std::invalid_argument if `config`
  // has no leave evaluator or enables a stage that is not implemented.
  Bot(const Gaddag& gaddag, BotConfig config);

  // Returns the best move for the player described by `view`, with
  // `equity` set. Always legal for the view's rack and board, and never an
  // exchange unless view.CanExchange(). Ties go to the first move in
  // generation order, so the result is deterministic. `rng` is reserved for
  // simulation and currently unused.
  Move ChooseMove(const PlayerView& view, Rng& rng);

  // Fills `out` with every legal move, equity set, best first, truncated to
  // `max_moves`. For analysis front ends; ChooseMove() does not sort.
  void RankMoves(const PlayerView& view, std::size_t max_moves,
                 std::vector<Move>& out);

  const BotConfig& config() const { return config_; }

 private:
  // Maps every sub-multiset of the current rack to a dense index so leave
  // values are computed once per distinct leave per turn, not once per
  // move. With per-letter counts c[l], letter l gets stride
  // prod(c[k] + 1 for distinct letters k before l), and a leave keeping
  // n[l] copies has index sum(n[l] * stride[l]). A 7-tile rack has at most
  // 2^7 = 128 sub-multisets (seven distinct tiles).
  static constexpr int kMaxLeaves = 1 << kRackSize;

  // Fills moves_ with every legal move and its static equity.
  void EvaluateStatic(const PlayerView& view);
  void PrepareLeaves(const PlayerView& view);
  double LeaveTerm(const Move& move) const;

  BotConfig config_;
  MoveGenerator generator_;
  std::vector<Move> moves_;
  std::array<int, kAlphabetSize> leave_stride_{};
  int full_rack_index_ = 0;
  std::array<double, kMaxLeaves> leave_values_{};
};

}  // namespace scrabble

#endif  // SCRABBLE_BOT_H_
