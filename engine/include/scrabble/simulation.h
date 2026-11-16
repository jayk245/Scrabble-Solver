// Monte Carlo simulation of candidate moves (the bot's third stage).
//
// Static equity (score + leave value) judges a move by its immediate
// result. Simulation instead plays each candidate forward against sampled
// futures and averages what happens, which captures board effects the leave
// table cannot: openings handed to the opponent, hooks, setups for the next
// turn and how the tiles kept combine with the tiles likely to be drawn.
//
// One iteration, for every surviving candidate:
//   1. Sample a future from what the player cannot see: the opponent's rack
//      is a uniformly random subset of PlayerView::unseen (bag plus the
//      opponent's rack) of the opponent's rack size, and the remaining
//      unseen tiles, shuffled, are the bag in draw order.
//   2. Play the candidate, draw replacements from the sampled bag, then
//      play `plies` further moves, alternating opponent and player, each the
//      best static-equity move for the mover's actual rack (the same
//      StaticEvaluator and leave table as the bot).
//   3. Score the rollout from the player's point of view:
//        (player's points - opponent's points over the candidate and the
//         simulated plies)
//        + the leave term of the player's last move
//        - the leave term of the opponent's last move (0 if it never moved)
//      Leave values estimate the future points a player's kept tiles are
//      worth. Each player's last leave is still on their rack, unplayed, so
//      both are counted: ignoring the opponent's would favour candidates
//      that give the opponent a weak score but a strong leave. With
//      plies = 0 the value is exactly the candidate's static equity.
//      If the game ends inside the rollout (a player goes out with the bag
//      empty, or six consecutive scoreless turns), the exact end-of-game
//      rack adjustment replaces both leave terms.
//
// Near the end of the bag: a rollout does not stop when its bag runs out.
// It plays its remaining plies with the static policy's empty-bag rule
// (going out is worth twice the opponent's rack; kept tiles cost twice
// their face value), which is crude but keeps every candidate's rollout the
// same length. Stopping early would score bag-emptying candidates without
// the opponent's reply and so systematically favour them. Once the real
// bag is empty the bot uses its endgame solver instead.
//
// Variance reduction: with common random numbers (CRN, the default) every
// candidate in an iteration sees the same opponent rack and bag order, so
// the luck of the draw cancels in comparisons between candidates.
//
// Pruning: iterations run in rounds of kSimulationRoundIterations. After
// min_iterations, a candidate is dropped once the leader (highest mean) is
// better by more than prune_z standard errors of their paired difference
// (the per-iteration difference of their values, which CRN makes far less
// noisy than either value). The run stops when one candidate is left, the
// iteration budget is spent, or the time limit passes.
//
// Determinism: iteration i is seeded from (decision seed, i) alone and the
// per-iteration values are reduced in iteration order after each round, so
// the result is bit-identical for any thread count, unless a time limit
// cuts the run short.

#ifndef SCRABBLE_SIMULATION_H_
#define SCRABBLE_SIMULATION_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "scrabble/gaddag.h"
#include "scrabble/game.h"
#include "scrabble/leave_evaluator.h"
#include "scrabble/move.h"
#include "scrabble/rack.h"
#include "scrabble/rng.h"
#include "scrabble/tile.h"

namespace scrabble {

// Longest supported rollout after the candidate.
inline constexpr int kMaxSimulationPlies = 8;

// Iterations per round: the granularity of pruning and stopping checks.
// Fixed, not derived from the thread count, so that results are too.
inline constexpr int kSimulationRoundIterations = 16;

struct SimulationConfig {
  // Bot stage switch (see bot.h); ignored by Simulator itself.
  bool enabled = false;
  // The bot simulates this many of its top static moves, plus the
  // highest-scoring move if it is not among them.
  int candidates = 15;
  // Iteration budget: every surviving candidate is rolled out once per
  // iteration.
  int iterations = 1000;
  // Moves played after the candidate: 2 = the opponent's reply and the
  // player's next move.
  int plies = 2;
  // Iterations before any candidate may be pruned.
  int min_iterations = 64;
  // Pruning threshold in standard errors of the paired difference; <= 0
  // disables pruning.
  double prune_z = 2.0;
  // Common random numbers across candidates. Off only to measure what it
  // buys.
  bool common_random_numbers = true;
  // Worker threads inside one decision. Use 1 where decisions already run
  // in parallel (the arena), more for interactive analysis.
  int threads = 1;
  // Wall-clock limit per decision, checked between rounds; 0 = none. A
  // limit makes the result depend on machine speed and load, so the
  // deterministic iteration budget is the default.
  std::int64_t time_limit_ms = 0;
};

// Statistics for one candidate. Plain data, so front ends can serialise it
// field by field.
struct CandidateStats {
  Move move;                   // equity = its static equity
  int iterations = 0;          // rollouts completed
  double mean = 0.0;           // mean rollout value (spread points)
  double stddev = 0.0;         // sample standard deviation of the values
  double std_error = 0.0;      // stddev / sqrt(iterations)
  // Mean and standard error of (chosen move's value - this value) over the
  // iterations both completed; 0 for the chosen move itself. With CRN this
  // is the precise comparison, much tighter than the two std_errors.
  double diff_from_best = 0.0;
  double diff_std_error = 0.0;
  bool pruned = false;         // dropped before the run ended
};

enum class SimulationStop : std::uint8_t {
  kSingleCandidate,  // only one candidate (given, or left after pruning)
  kIterations,       // iteration budget spent
  kTime,             // time limit reached
};

struct SimulationResult {
  std::vector<CandidateStats> candidates;  // in the order given to Run()
  int best = 0;                            // index into `candidates`
  Move best_move;                          // equity = its mean value
  int iterations = 0;                      // iterations completed
  std::int64_t rollouts = 0;               // over all candidates
  double seconds = 0.0;
  int threads = 1;
  SimulationStop stop = SimulationStop::kSingleCandidate;
};

// A sampled future: the opponent's rack and the bag in draw order
// (bag[0] is drawn first). The opponent's rack plus bag[0, bag_size) is
// exactly the view's unseen multiset.
struct SimulationSample {
  Rack opponent;
  std::array<Letter, kTotalTiles> bag{};
  int bag_size = 0;
};

// True if `view` describes a state Simulator can sample: its unseen counts
// sum to unseen_size == bag_size + opponent_rack_size, the opponent's rack
// size is within 0..kRackSize, and a known_opponent_rack, if set, has that
// size and is drawn from the unseen tiles.
bool CanSimulate(const PlayerView& view);

// Draws a uniformly random SimulationSample for `view`: with a
// known_opponent_rack only the bag order is random. Precondition:
// CanSimulate(view).
void DrawSample(const PlayerView& view, Rng& rng, SimulationSample& out);

// Seed of iteration `iteration` of a decision seeded with `seed` (and, with
// CRN off, of one candidate: pass its index + 1 as `stream`).
std::uint64_t SimulationSeed(std::uint64_t seed, std::int64_t iteration,
                             std::uint64_t stream = 0);

// Runs simulations; reusable across decisions, and holds one set of
// rollout buffers (Position, MoveGenerator, move list) per thread, so a
// rollout performs no heap allocation and no Position copy. Not
// thread-safe itself: one Run() at a time.
class Simulator {
 public:
  // `gaddag` must outlive the simulator. Throws std::invalid_argument if
  // `leaves` is null or `config` is out of range (candidates, iterations or
  // threads < 1, plies outside 0..kMaxSimulationPlies, min_iterations < 2).
  Simulator(const Gaddag& gaddag, std::shared_ptr<const LeaveEvaluator> leaves,
            const SimulationConfig& config);
  ~Simulator();

  Simulator(const Simulator&) = delete;
  Simulator& operator=(const Simulator&) = delete;

  // Simulates `candidates` (each legal for the view) from `view` and returns
  // the result, valid until the next call. Preconditions: CanSimulate(view)
  // and !candidates.empty(). `view` and its position are only read: each
  // thread rolls out on its own copy. With a single candidate no rollouts
  // are run.
  const SimulationResult& Run(const PlayerView& view,
                              std::span<const Move> candidates,
                              std::uint64_t seed);

  const SimulationConfig& config() const { return config_; }

 private:
  class Worker;

  // Barrier completion step between rounds (single-threaded): folds the
  // round's values into the statistics, prunes, and sets up the next round
  // or marks the run done.
  void FinishRound() noexcept;
  void StartRound();
  void Prune();
  // Claims and runs work items (one rollout each) of the current round
  // until none are left.
  void RunItems(Worker& worker);
  void BuildResult();
  // Mean value of candidate `i` so far; requires count_[i] > 0.
  double Mean(std::size_t i) const {
    return shift_[i] + sum_[i] / static_cast<double>(count_[i]);
  }

  const Gaddag* gaddag_;
  std::shared_ptr<const LeaveEvaluator> leaves_;
  SimulationConfig config_;
  std::vector<std::unique_ptr<Worker>> workers_;

  // State of the current Run(); sized once per Run(), never per iteration.
  const PlayerView* view_ = nullptr;
  std::uint64_t seed_ = 0;
  std::vector<Move> candidates_;
  std::vector<int> active_;            // surviving candidate indices
  // Running sums are of values minus a shift (the first value seen), so
  // the variance does not suffer catastrophic cancellation when the spread
  // of the values is small next to their size.
  std::vector<std::int64_t> count_;    // per candidate
  std::vector<double> shift_;          // per candidate
  std::vector<double> sum_;            // per candidate
  std::vector<double> sum_squares_;    // per candidate
  // Paired differences x[a] - x[b] for a < b, over the iterations both
  // were active: [a * n + b].
  std::vector<double> pair_shift_;
  std::vector<double> pair_sum_;
  std::vector<double> pair_squares_;
  std::vector<bool> pruned_;
  // Values of the current round: [round iteration * n + candidate].
  std::vector<double> round_values_;
  int iterations_done_ = 0;
  int round_start_ = 0;
  int round_size_ = 0;
  int round_items_ = 0;
  bool done_ = false;
  SimulationStop stop_ = SimulationStop::kSingleCandidate;
  std::int64_t start_ns_ = 0;
  SimulationResult result_;
  // Work-item counter for the current round, shared by all threads. On its
  // own cache line so claiming work does not contend with the fields above.
  alignas(64) std::atomic<int> next_item_{0};
};

}  // namespace scrabble

#endif  // SCRABBLE_SIMULATION_H_
