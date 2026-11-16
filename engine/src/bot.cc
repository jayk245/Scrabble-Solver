#include "scrabble/bot.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>

namespace scrabble {
namespace {

// Validates before the evaluator is bound to the table, so a null table is
// reported instead of dereferenced.
const LeaveEvaluator& CheckedLeaves(const BotConfig& config) {
  if (config.leaves == nullptr) {
    throw std::invalid_argument("BotConfig::leaves must not be null");
  }
  return *config.leaves;
}

}  // namespace

Bot::Bot(const Gaddag& gaddag, BotConfig config)
    : config_(std::move(config)),
      generator_(gaddag),
      evaluator_(CheckedLeaves(config_)) {
  if (config_.simulation.enabled) {
    simulator_ =
        std::make_unique<Simulator>(gaddag, config_.leaves, config_.simulation);
  }
  if (config_.endgame.enabled) {
    endgame_ = std::make_unique<EndgameSolver>(
        gaddag, EndgameOptions{.tt_size_mb = config_.endgame.tt_size_mb});
  }
}

Move Bot::ChooseMove(const PlayerView& view, Rng& rng) {
  last_simulation_ = nullptr;
  last_endgame_.reset();
  if (endgame_ != nullptr && view.bag_size == 0) {
    if (const std::optional<Move> move = SolveEndgame(view)) return *move;
  }
  EvaluateStatic(view);
  // Never empty: the pass is always generated.
  assert(!moves_.empty());
  // With an empty bag rollouts would be deterministic and shallow, so the
  // static rule (or the endgame stage above) decides instead.
  if (simulator_ != nullptr && view.bag_size > 0 && CanSimulate(view)) {
    SelectCandidates();
    last_simulation_ = &simulator_->Run(view, candidates_, rng());
    return last_simulation_->best_move;
  }
  // max_element keeps the first of equal elements, which makes ties
  // resolve by generation order.
  return *std::max_element(
      moves_.begin(), moves_.end(),
      [](const Move& a, const Move& b) { return a.equity < b.equity; });
}

void Bot::RankMoves(const PlayerView& view, std::size_t max_moves,
                    std::vector<Move>& out) {
  EvaluateStatic(view);
  const std::size_t keep = std::min(max_moves, moves_.size());
  // Unlike ChooseMove(), tie order here only affects display, so a
  // non-stable partial sort is fine.
  std::partial_sort(
      moves_.begin(), moves_.begin() + static_cast<std::ptrdiff_t>(keep),
      moves_.end(),
      [](const Move& a, const Move& b) { return a.equity > b.equity; });
  out.assign(moves_.begin(),
             moves_.begin() + static_cast<std::ptrdiff_t>(keep));
}

std::optional<Move> Bot::SolveEndgame(const PlayerView& view) {
  assert(view.position != nullptr);
  // With the bag empty every unseen tile is on the opponent's rack.
  if (view.unseen_size < 1 || view.unseen_size > kRackSize ||
      view.rack.IsEmpty()) {
    return std::nullopt;
  }
  EndgameState state;
  Rack& opponent = state.racks[static_cast<std::size_t>(1 - view.player)];
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    for (int n = 0; n < view.unseen[letter]; ++n) {
      if (opponent.Size() == kRackSize) return std::nullopt;
      opponent.Add(letter);
    }
  }
  if (opponent.IsEmpty()) return std::nullopt;
  state.racks[static_cast<std::size_t>(view.player)] = view.rack;
  state.to_move = view.player;
  state.spread = view.score - view.opponent_score;
  state.scoreless_turns = view.scoreless_turns;
  state.end_rules = view.end_rules;
  endgame_position_.emplace(*view.position);
  last_endgame_ =
      endgame_->Solve(*endgame_position_, state,
                      {.max_nodes = config_.endgame.node_limit,
                       .max_time_ms = config_.endgame.time_limit_ms});
  return last_endgame_->best_move;
}

void Bot::EvaluateStatic(const PlayerView& view) {
  assert(view.position != nullptr);
  generator_.Generate(*view.position, view.rack, moves_,
                      {.include_exchanges = view.CanExchange(),
                       .include_pass = true});
  int unseen_value = 0;
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    unseen_value += view.unseen[letter] * kTileValues[letter];
  }
  // With the bag empty the unseen tiles are the opponent's rack.
  evaluator_.Prepare(view.rack, view.bag_size == 0, unseen_value);
  for (Move& move : moves_) move.equity = evaluator_.Equity(move);
}

void Bot::SelectCandidates() {
  const std::size_t keep = std::min(
      moves_.size(),
      static_cast<std::size_t>(config_.simulation.candidates));
  order_.resize(moves_.size());
  std::iota(order_.begin(), order_.end(), std::size_t{0});
  // Index as the tie-break makes the selection independent of the sort
  // algorithm, and so deterministic.
  std::partial_sort(
      order_.begin(), order_.begin() + static_cast<std::ptrdiff_t>(keep),
      order_.end(), [this](std::size_t a, std::size_t b) {
        if (moves_[a].equity != moves_[b].equity) {
          return moves_[a].equity > moves_[b].equity;
        }
        return a < b;
      });
  candidates_.clear();
  for (std::size_t i = 0; i < keep; ++i) {
    candidates_.push_back(moves_[order_[i]]);
  }
  // Static equity can undervalue a big score (its leave may be judged too
  // harshly), so the top scorer always gets a chance in simulation.
  std::size_t top_scorer = 0;
  for (std::size_t i = 1; i < moves_.size(); ++i) {
    if (moves_[i].score > moves_[top_scorer].score) top_scorer = i;
  }
  const bool included =
      std::find(order_.begin(), order_.begin() + static_cast<std::ptrdiff_t>(keep),
                top_scorer) != order_.begin() + static_cast<std::ptrdiff_t>(keep);
  if (!included) candidates_.push_back(moves_[top_scorer]);

  // Equity is whatever the evaluator last wrote, so it takes no part in
  // matching a requested move to a generated one.
  const auto same_play = [](Move a, Move b) {
    a.equity = 0.0;
    b.equity = 0.0;
    return a == b;
  };
  for (const Move& extra : extra_candidates_) {
    const auto matches = [&](const Move& move) {
      return same_play(move, extra);
    };
    if (std::any_of(candidates_.begin(), candidates_.end(), matches)) continue;
    const auto legal = std::find_if(moves_.begin(), moves_.end(), matches);
    if (legal != moves_.end()) candidates_.push_back(*legal);
  }
}

}  // namespace scrabble
