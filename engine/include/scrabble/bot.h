// The Scrabble bot.
//
// There is exactly one bot. It grows in strength by enabling stages through
// BotConfig rather than by adding bot classes, so any two strengths can be
// A/B tested in the arena by changing configuration alone:
//
//   1. Generate every legal move, including exchanges (when the bag allows)
//      and the pass.
//   2. Static evaluation: equity = score + leave term (StaticEvaluator,
//      static_evaluator.h), with leave values from the configured
//      LeaveEvaluator; by default the learned table (DefaultLeaves()). Once
//      the bag is empty the leave term becomes the end-of-game rack swing.
//      With the later stages off, the best static move is played.
//   3. Simulation (BotConfig::simulation, simulation.h): while the bag has
//      tiles, re-rank the top static candidates (plus the highest-scoring
//      move) by Monte Carlo rollouts against futures sampled from the
//      unseen tiles, and play the best mean.
//   4. Endgame (BotConfig::endgame): with the bag empty the opponent's rack
//      is exactly PlayerView::unseen, and ChooseMove() plays the move an
//      EndgameSolver search finds best instead.

#ifndef SCRABBLE_BOT_H_
#define SCRABBLE_BOT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "scrabble/endgame.h"
#include "scrabble/game.h"
#include "scrabble/gaddag.h"
#include "scrabble/leave_evaluator.h"
#include "scrabble/move.h"
#include "scrabble/move_generator.h"
#include "scrabble/position.h"
#include "scrabble/rack.h"
#include "scrabble/rng.h"
#include "scrabble/simulation.h"
#include "scrabble/static_evaluator.h"

namespace scrabble {

// Endgame search once the bag is empty (see endgame.h). Both racks are
// known then, so the remaining game is searched exactly when the limits
// allow, and to the deepest completed depth otherwise. With a time limit the
// chosen move can depend on machine load; the node limit alone keeps
// ChooseMove() deterministic, which is why it is the default.
struct EndgameConfig {
  bool enabled = false;
  std::int64_t time_limit_ms = 0;     // 0 = no time limit
  std::int64_t node_limit = 2'000'000;  // 0 = no node limit
  std::size_t tt_size_mb = 16;        // per Bot
};

struct BotConfig {
  // Label used in logs, GCG files and reports.
  std::string name = "bot";
  // Shared so many bots (e.g. one per arena thread) can read one loaded
  // table. Must not be null. Defaults to the learned table.
  std::shared_ptr<const LeaveEvaluator> leaves = DefaultLeaves();
  SimulationConfig simulation;
  EndgameConfig endgame;
};

// Holds move-generation scratch buffers (and, with the later stages on,
// simulation workers and a transposition table), so a Bot is used by one
// thread at a time; give each thread its own. With only the static stage,
// ChooseMove() performs no heap allocation once its buffers have grown to
// the largest move list seen.
class Bot {
 public:
  // `gaddag` must outlive the bot. Throws std::invalid_argument if `config`
  // has no leave evaluator or an out-of-range simulation configuration.
  Bot(const Gaddag& gaddag, BotConfig config);

  // Returns the best move for the player described by `view`, with
  // `equity` set. Always legal for the view's rack and board, and never an
  // exchange unless view.CanExchange(). Ties go to the first move in
  // generation order, so the result is deterministic.
  //
  // With the simulation stage on and tiles in the bag, the move is the
  // simulation's choice and `equity` its mean rollout value; one draw from
  // `rng` seeds the decision, so equal rng states give equal moves for any
  // simulation thread count (unless a time limit is set). The view must
  // satisfy CanSimulate(), or the static choice is played.
  //
  // With the endgame stage on and the bag empty, the move is the solver's
  // and `equity` is its value: the expected final spread for this player.
  // The view must then be one Game could produce: `unseen` is taken to be
  // the opponent's rack (a view whose unseen tiles cannot form a rack falls
  // back to static evaluation).
  Move ChooseMove(const PlayerView& view, Rng& rng);

  // Fills `out` with every legal move, equity set, best first, truncated to
  // `max_moves`. For analysis front ends; ChooseMove() does not sort. Always
  // the static ranking, even in the endgame: the solver proves only the best
  // move's value, not an order of the rest.
  void RankMoves(const PlayerView& view, std::size_t max_moves,
                 std::vector<Move>& out);

  // The simulation behind the last ChooseMove(), or null if that call did
  // not simulate. Valid until the next ChooseMove().
  const SimulationResult* last_simulation() const { return last_simulation_; }

  const BotConfig& config() const { return config_; }

 private:
  // The solver's move for an empty-bag view, or nullopt if the opponent's
  // rack cannot be reconstructed from the view.
  std::optional<Move> SolveEndgame(const PlayerView& view);

  // Fills moves_ with every legal move and its static equity.
  void EvaluateStatic(const PlayerView& view);
  // Fills candidates_ from moves_ (after EvaluateStatic()): the top
  // config_.simulation.candidates by equity, ties in generation order, plus
  // the highest-scoring move if it is not among them.
  void SelectCandidates();

  BotConfig config_;
  MoveGenerator generator_;
  StaticEvaluator evaluator_;
  std::vector<Move> moves_;

  // Simulation stage; null when disabled.
  std::unique_ptr<Simulator> simulator_;
  std::vector<std::size_t> order_;
  std::vector<Move> candidates_;
  const SimulationResult* last_simulation_ = nullptr;  // owned by simulator_

  // Endgame stage; null when disabled. The solver mutates a Position, so
  // the view's board is copied once per decision into endgame_position_.
  std::unique_ptr<EndgameSolver> endgame_;
  std::optional<Position> endgame_position_;
};

}  // namespace scrabble

#endif  // SCRABBLE_BOT_H_
