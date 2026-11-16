#include "scrabble/simulation.h"

#include <algorithm>
#include <barrier>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "scrabble/move_generator.h"
#include "scrabble/position.h"
#include "scrabble/static_evaluator.h"

namespace scrabble {
namespace {

std::int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Sample standard deviation from running sums of (shifted) values; 0 below
// two samples.
double StdDev(double sum, double sum_squares, std::int64_t n) {
  if (n < 2) return 0.0;
  const double count = static_cast<double>(n);
  const double mean = sum / count;
  // Clamped because rounding can push a near-zero variance negative.
  return std::sqrt(
      std::max(0.0, (sum_squares - count * mean * mean) / (count - 1.0)));
}

}  // namespace

bool CanSimulate(const PlayerView& view) {
  if (view.position == nullptr || view.bag_size < 0 ||
      view.opponent_rack_size < 0 || view.opponent_rack_size > kRackSize ||
      view.bag_size + view.opponent_rack_size != view.unseen_size ||
      view.unseen_size > kTotalTiles) {
    return false;
  }
  int total = 0;
  for (const std::uint8_t count : view.unseen) total += count;
  return total == view.unseen_size;
}

void DrawSample(const PlayerView& view, Rng& rng, SimulationSample& out) {
  assert(CanSimulate(view));
  int size = 0;
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    for (int n = view.unseen[letter]; n > 0; --n) {
      out.bag[static_cast<std::size_t>(size++)] = letter;
    }
  }
  // Fisher-Yates: every permutation of the unseen tiles is equally likely,
  // so the opponent's rack (the tail) is a uniform subset and the rest is a
  // uniform draw order.
  for (int i = 0; i + 1 < size; ++i) {
    const auto j = static_cast<std::size_t>(
        i + static_cast<int>(rng.Below(static_cast<std::uint32_t>(size - i))));
    std::swap(out.bag[static_cast<std::size_t>(i)], out.bag[j]);
  }
  out.bag_size = size - view.opponent_rack_size;
  out.opponent.Clear();
  for (int i = out.bag_size; i < size; ++i) {
    out.opponent.Add(out.bag[static_cast<std::size_t>(i)]);
  }
}

std::uint64_t SimulationSeed(std::uint64_t seed, std::int64_t iteration,
                             std::uint64_t stream) {
  std::uint64_t z = seed +
                    0x9E3779B97F4A7C15ULL *
                        (static_cast<std::uint64_t>(iteration) + 1) +
                    0xD1B54A32D192ED03ULL * stream;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// Rollout buffers for one thread. The Position is copied from the view once
// per Run() and then only changed by Apply()/Undo() pairs, so it equals the
// view's position again after every rollout.
class Simulator::Worker {
 public:
  Worker(const Gaddag& gaddag, const LeaveEvaluator& leaves)
      : position_(gaddag), generator_(gaddag), evaluator_(leaves) {
    // Comfortably above typical move counts; Generate() grows it further
    // if a rack with blanks needs more, after which it stays allocated.
    moves_.reserve(4096);
  }

  void Reset(const Position& position) { position_ = position; }

  // The value of `candidate` against the future drawn from `sample_seed`,
  // from the view's player's perspective (see simulation.h).
  double Rollout(const PlayerView& view, const Move& candidate,
                 std::uint64_t sample_seed, int plies) {
    Rng rng(sample_seed);
    DrawSample(view, rng, sample_);
    racks_ = {view.rack, sample_.opponent};
    std::copy_n(sample_.bag.begin(), sample_.bag_size, bag_.begin());
    bag_top_ = 0;
    bag_end_ = sample_.bag_size;
    spread_ = 0;
    scoreless_turns_ = view.scoreless_turns;
    leave_terms_ = {0.0, 0.0};
    applied_ = 0;

    evaluator_.Prepare(racks_[0], view.bag_size == 0, racks_[1].Score());
    bool over = Play(0, candidate, evaluator_.LeaveTerm(candidate), rng);
    for (int ply = 1; ply <= plies && !over; ++ply) {
      const int player = ply % 2;
      double leave_term = 0.0;
      const Move& move = ChooseStatic(player, leave_term);
      over = Play(player, move, leave_term, rng);
    }
    double value = spread_;
    if (!over) value += leave_terms_[0] - leave_terms_[1];

    while (applied_ > 0) {
      --applied_;
      const auto i = static_cast<std::size_t>(applied_);
      position_.Undo(applied_moves_[i], undo_[i]);
    }
    return value;
  }

 private:
  // Room for the sampled bag plus tiles returned by exchanges, of which
  // there can be at most one per move of the rollout.
  static constexpr int kBagCapacity =
      kTotalTiles + kRackSize * (kMaxSimulationPlies + 1);

  int BagSize() const { return bag_end_ - bag_top_; }

  // The static policy: the mover's best static-equity move, first in
  // generation order on ties, exactly as Bot::ChooseMove() ranks.
  const Move& ChooseStatic(int player, double& leave_term) {
    const auto p = static_cast<std::size_t>(player);
    generator_.Generate(
        position_, racks_[p], moves_,
        {.include_exchanges = BagSize() >= kMinBagForExchange,
         .include_pass = true});
    evaluator_.Prepare(racks_[p], BagSize() == 0, racks_[1 - p].Score());
    // Never empty: the pass is always generated.
    std::size_t best = 0;
    double best_equity = evaluator_.Equity(moves_[0]);
    for (std::size_t i = 1; i < moves_.size(); ++i) {
      const double equity = evaluator_.Equity(moves_[i]);
      if (equity > best_equity) {
        best = i;
        best_equity = equity;
      }
    }
    leave_term = best_equity - moves_[best].score;
    return moves_[best];
  }

  // Plays `move` for `player` (0 = the view's player): board, rack, draws,
  // spread and the scoreless-turn count. Returns true if it ends the game,
  // after adding the end-of-game rack adjustment to the spread.
  bool Play(int player, const Move& move, double leave_term, Rng& rng) {
    const auto p = static_cast<std::size_t>(player);
    const int sign = player == 0 ? 1 : -1;
    if (move.type == MoveType::kPlace) {
      const auto i = static_cast<std::size_t>(applied_++);
      undo_[i] = position_.Apply(move);
      applied_moves_[i] = move;
    }
    Rack& rack = racks_[p];
    rack = LeaveAfter(rack, move);
    spread_ += sign * move.score;
    if (move.type != MoveType::kPass) {
      while (rack.Size() < kRackSize && bag_top_ < bag_end_) {
        rack.Add(bag_[static_cast<std::size_t>(bag_top_++)]);
      }
    }
    if (move.type == MoveType::kExchange) {
      for (std::size_t i = 0; i < move.num_tiles; ++i) {
        ReturnToBag(move.tiles[i].letter(), rng);
      }
    }
    scoreless_turns_ = move.score == 0 ? scoreless_turns_ + 1 : 0;
    if (move.type == MoveType::kPlace && rack.IsEmpty()) {
      // Going out swings the spread by twice the opponent's rack under
      // both EndRules.
      spread_ += sign * 2 * racks_[1 - p].Score();
      return true;
    }
    if (scoreless_turns_ >= kScorelessTurnsToEnd) {
      spread_ += racks_[1].Score() - racks_[0].Score();
      return true;
    }
    leave_terms_[p] = leave_term;
    return false;
  }

  // Inserts `letter` at a uniformly random position among the undrawn
  // tiles, so the remaining draw order stays a uniform permutation.
  void ReturnToBag(Letter letter, Rng& rng) {
    assert(bag_end_ < kBagCapacity);
    bag_[static_cast<std::size_t>(bag_end_++)] = letter;
    const auto j = static_cast<std::size_t>(
        bag_top_ +
        static_cast<int>(rng.Below(static_cast<std::uint32_t>(BagSize()))));
    std::swap(bag_[static_cast<std::size_t>(bag_end_ - 1)], bag_[j]);
  }

  Position position_;
  MoveGenerator generator_;
  StaticEvaluator evaluator_;
  std::vector<Move> moves_;
  SimulationSample sample_;
  std::array<Rack, 2> racks_{};
  std::array<Letter, kBagCapacity> bag_{};
  int bag_top_ = 0;
  int bag_end_ = 0;
  int spread_ = 0;
  int scoreless_turns_ = 0;
  std::array<double, 2> leave_terms_{};
  // Placements applied to position_, for LIFO undo.
  std::array<Move, kMaxSimulationPlies + 1> applied_moves_{};
  std::array<PositionUndo, kMaxSimulationPlies + 1> undo_{};
  int applied_ = 0;
};

Simulator::Simulator(const Gaddag& gaddag,
                     std::shared_ptr<const LeaveEvaluator> leaves,
                     const SimulationConfig& config)
    : gaddag_(&gaddag), leaves_(std::move(leaves)), config_(config) {
  if (leaves_ == nullptr) {
    throw std::invalid_argument("Simulator: leaves must not be null");
  }
  if (config_.candidates < 1 || config_.iterations < 1 ||
      config_.threads < 1 || config_.plies < 0 ||
      config_.plies > kMaxSimulationPlies || config_.min_iterations < 2) {
    throw std::invalid_argument("Simulator: configuration out of range");
  }
  workers_.reserve(static_cast<std::size_t>(config_.threads));
  for (int i = 0; i < config_.threads; ++i) {
    workers_.push_back(std::make_unique<Worker>(*gaddag_, *leaves_));
  }
}

Simulator::~Simulator() = default;

const SimulationResult& Simulator::Run(const PlayerView& view,
                                       std::span<const Move> candidates,
                                       std::uint64_t seed) {
  assert(CanSimulate(view));
  assert(!candidates.empty());
  start_ns_ = NowNs();
  view_ = &view;
  seed_ = seed;
  candidates_.assign(candidates.begin(), candidates.end());
  const std::size_t n = candidates_.size();
  active_.resize(n);
  for (std::size_t i = 0; i < n; ++i) active_[i] = static_cast<int>(i);
  count_.assign(n, 0);
  shift_.assign(n, 0.0);
  sum_.assign(n, 0.0);
  sum_squares_.assign(n, 0.0);
  pair_shift_.assign(n * n, 0.0);
  pair_sum_.assign(n * n, 0.0);
  pair_squares_.assign(n * n, 0.0);
  pruned_.assign(n, false);
  round_values_.assign(n * kSimulationRoundIterations, 0.0);
  iterations_done_ = 0;
  done_ = false;
  stop_ = SimulationStop::kSingleCandidate;
  int threads_used = 1;

  if (n > 1) {
    StartRound();
    // No more threads than one round can keep busy.
    const int threads = std::min(
        config_.threads,
        static_cast<int>(n) * kSimulationRoundIterations);
    threads_used = threads;
    for (int t = 0; t < threads; ++t) {
      workers_[static_cast<std::size_t>(t)]->Reset(*view.position);
    }
    if (threads == 1) {
      while (!done_) {
        RunItems(*workers_[0]);
        FinishRound();
      }
    } else {
      // The completion step runs once per round, on one thread, after every
      // thread has finished its share and before any starts the next.
      std::barrier sync(threads, [this]() noexcept { FinishRound(); });
      const auto loop = [this, &sync](Worker& worker) {
        do {
          RunItems(worker);
          sync.arrive_and_wait();
        } while (!done_);
      };
      std::vector<std::thread> pool;
      pool.reserve(static_cast<std::size_t>(threads - 1));
      try {
        for (int t = 1; t < threads; ++t) {
          pool.emplace_back(loop,
                            std::ref(*workers_[static_cast<std::size_t>(t)]));
        }
      } catch (...) {
        // Could not start every thread: release the barrier slots of the
        // missing ones so the started threads finish, then report.
        for (std::size_t t = pool.size() + 1;
             t < static_cast<std::size_t>(threads); ++t) {
          sync.arrive_and_drop();
        }
        loop(*workers_[0]);
        for (std::thread& thread : pool) thread.join();
        throw;
      }
      loop(*workers_[0]);
      for (std::thread& thread : pool) thread.join();
    }
  }

  BuildResult();
  result_.threads = threads_used;
  result_.seconds = static_cast<double>(NowNs() - start_ns_) * 1e-9;
  view_ = nullptr;
  return result_;
}

void Simulator::StartRound() {
  round_start_ = iterations_done_;
  round_size_ = std::min(kSimulationRoundIterations,
                         config_.iterations - iterations_done_);
  round_items_ = round_size_ * static_cast<int>(active_.size());
  next_item_.store(0, std::memory_order_relaxed);
}

void Simulator::RunItems(Worker& worker) {
  const int active = static_cast<int>(active_.size());
  const std::size_t n = candidates_.size();
  for (int item = next_item_.fetch_add(1, std::memory_order_relaxed);
       item < round_items_;
       item = next_item_.fetch_add(1, std::memory_order_relaxed)) {
    const int r = item / active;
    const int candidate = active_[static_cast<std::size_t>(item % active)];
    const std::int64_t iteration = round_start_ + r;
    // With CRN every candidate replays iteration i's future; without, each
    // candidate gets its own stream.
    const std::uint64_t sample_seed =
        config_.common_random_numbers
            ? SimulationSeed(seed_, iteration)
            : SimulationSeed(seed_, iteration,
                             static_cast<std::uint64_t>(candidate) + 1);
    // Each item writes its own slot; the barrier (or the single thread)
    // orders these writes before FinishRound() reads them.
    round_values_[static_cast<std::size_t>(r) * n +
                  static_cast<std::size_t>(candidate)] =
        worker.Rollout(*view_, candidates_[static_cast<std::size_t>(candidate)],
                       sample_seed, config_.plies);
  }
}

void Simulator::FinishRound() noexcept {
  const std::size_t n = candidates_.size();
  // Fixed order (iteration, then candidate index) so the floating-point
  // sums, and hence every decision below, do not depend on scheduling.
  for (int r = 0; r < round_size_; ++r) {
    const double* values = &round_values_[static_cast<std::size_t>(r) * n];
    for (std::size_t a = 0; a < active_.size(); ++a) {
      const auto i = static_cast<std::size_t>(active_[a]);
      const double x = values[i];
      if (count_[i]++ == 0) shift_[i] = x;
      const double shifted = x - shift_[i];
      sum_[i] += shifted;
      sum_squares_[i] += shifted * shifted;
      for (std::size_t b = a + 1; b < active_.size(); ++b) {
        const auto j = static_cast<std::size_t>(active_[b]);
        // active_ is kept in increasing order, so i < j, and both have
        // been active for every iteration so far.
        const std::size_t pair = i * n + j;
        const double d = x - values[j];
        if (iterations_done_ + r == 0) pair_shift_[pair] = d;
        const double shifted_d = d - pair_shift_[pair];
        pair_sum_[pair] += shifted_d;
        pair_squares_[pair] += shifted_d * shifted_d;
      }
    }
  }
  iterations_done_ += round_size_;

  if (config_.prune_z > 0.0 && iterations_done_ >= config_.min_iterations) {
    Prune();
  }
  if (active_.size() == 1) {
    stop_ = SimulationStop::kSingleCandidate;
    done_ = true;
  } else if (iterations_done_ >= config_.iterations) {
    stop_ = SimulationStop::kIterations;
    done_ = true;
  } else if (config_.time_limit_ms > 0 &&
             NowNs() - start_ns_ >= config_.time_limit_ms * 1'000'000) {
    stop_ = SimulationStop::kTime;
    done_ = true;
  } else {
    StartRound();
  }
}

void Simulator::Prune() {
  const std::size_t n = candidates_.size();
  // Every active candidate has completed exactly iterations_done_ rollouts.
  const double count = iterations_done_;
  std::size_t leader = static_cast<std::size_t>(active_[0]);
  for (const int c : active_) {
    if (Mean(static_cast<std::size_t>(c)) > Mean(leader)) {
      leader = static_cast<std::size_t>(c);
    }
  }
  const auto kept = std::remove_if(
      active_.begin(), active_.end(), [&](int c) {
        const auto i = static_cast<std::size_t>(c);
        if (i == leader) return false;
        // Paired difference leader - candidate; the variance of a
        // difference does not depend on its sign.
        const std::size_t pair = std::min(i, leader) * n + std::max(i, leader);
        const double sign = leader < i ? 1.0 : -1.0;
        const double mean =
            sign * (pair_shift_[pair] + pair_sum_[pair] / count);
        const double se =
            StdDev(pair_sum_[pair], pair_squares_[pair], iterations_done_) /
            std::sqrt(count);
        const bool drop = mean - config_.prune_z * se > 0.0;
        if (drop) pruned_[i] = true;
        return drop;
      });
  active_.erase(kept, active_.end());
}

void Simulator::BuildResult() {
  const std::size_t n = candidates_.size();
  result_.candidates.resize(n);
  // The best survivor; ties go to the earlier (statically better)
  // candidate. With no rollouts (one candidate) that is candidate 0.
  std::size_t best = static_cast<std::size_t>(active_[0]);
  for (const int c : active_) {
    if (Mean(static_cast<std::size_t>(c)) > Mean(best)) {
      best = static_cast<std::size_t>(c);
    }
  }
  result_.rollouts = 0;
  for (std::size_t i = 0; i < n; ++i) {
    CandidateStats& stats = result_.candidates[i];
    stats.move = candidates_[i];
    stats.iterations = static_cast<int>(count_[i]);
    const double count =
        static_cast<double>(std::max<std::int64_t>(count_[i], 1));
    // Without rollouts the best estimate is the static one.
    stats.mean = count_[i] > 0 ? Mean(i) : candidates_[i].equity;
    stats.stddev = StdDev(sum_[i], sum_squares_[i], count_[i]);
    stats.std_error = count_[i] > 0 ? stats.stddev / std::sqrt(count) : 0.0;
    stats.pruned = pruned_[i];
    stats.diff_from_best = 0.0;
    stats.diff_std_error = 0.0;
    result_.rollouts += count_[i];
    if (i == best) continue;
    // Both were active for the first min(count) iterations.
    const std::int64_t common = std::min(count_[i], count_[best]);
    if (common == 0) continue;
    const std::size_t pair = std::min(i, best) * n + std::max(i, best);
    const double sign = best < i ? 1.0 : -1.0;
    stats.diff_from_best =
        sign * (pair_shift_[pair] +
                pair_sum_[pair] / static_cast<double>(common));
    stats.diff_std_error = StdDev(pair_sum_[pair], pair_squares_[pair],
                                  common) /
                           std::sqrt(static_cast<double>(common));
  }
  result_.best = static_cast<int>(best);
  result_.best_move = candidates_[best];
  result_.best_move.equity = result_.candidates[best].mean;
  result_.iterations = iterations_done_;
  result_.stop = stop_;
}

}  // namespace scrabble
