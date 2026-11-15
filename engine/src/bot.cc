#include "scrabble/bot.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace scrabble {

Bot::Bot(const Gaddag& gaddag, BotConfig config)
    : config_(std::move(config)), generator_(gaddag) {
  if (config_.leaves == nullptr) {
    throw std::invalid_argument("BotConfig::leaves must not be null");
  }
  if (config_.simulation.enabled) {
    throw std::invalid_argument("simulation is not implemented yet");
  }
  if (config_.endgame.enabled) {
    throw std::invalid_argument("endgame search is not implemented yet");
  }
}

Move Bot::ChooseMove(const PlayerView& view, Rng& /*rng*/) {
  EvaluateStatic(view);
  // Never empty: the pass is always generated.
  assert(!moves_.empty());
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

void Bot::EvaluateStatic(const PlayerView& view) {
  assert(view.position != nullptr);
  generator_.Generate(*view.position, view.rack, moves_,
                      {.include_exchanges = view.CanExchange(),
                       .include_pass = true});
  PrepareLeaves(view);
  for (Move& move : moves_) move.equity = move.score + LeaveTerm(move);
}

void Bot::PrepareLeaves(const PlayerView& view) {
  const Rack& rack = view.rack;
  int combinations = 1;
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    const int count = rack.Count(letter);
    leave_stride_[letter] = count > 0 ? combinations : 0;
    combinations *= count + 1;
  }
  assert(combinations <= kMaxLeaves);
  full_rack_index_ = combinations - 1;

  int unseen_value = 0;
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    unseen_value += view.unseen[letter] * kTileValues[letter];
  }
  const bool bag_empty = view.bag_size == 0;

  for (int index = 0; index < combinations; ++index) {
    Rack leave;
    for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
      const int stride = leave_stride_[letter];
      if (stride == 0) continue;
      const int keep = (index / stride) % (rack.Count(letter) + 1);
      for (int n = 0; n < keep; ++n) leave.Add(letter);
    }
    double value;
    if (!bag_empty) {
      value = config_.leaves->Value(leave);
    } else if (leave.IsEmpty()) {
      value = 2.0 * unseen_value;
    } else {
      value = -2.0 * leave.Score();
    }
    leave_values_[static_cast<std::size_t>(index)] = value;
  }
}

double Bot::LeaveTerm(const Move& move) const {
  int index = full_rack_index_;
  for (std::size_t i = 0; i < move.num_tiles; ++i) {
    const MoveTile tile = move.tiles[i];
    if (move.type == MoveType::kPlace) {
      if (tile.is_play_through()) continue;
      index -= leave_stride_[tile.is_blank() ? kBlank : tile.letter()];
    } else {
      index -= leave_stride_[tile.letter()];
    }
  }
  return leave_values_[static_cast<std::size_t>(index)];
}

}  // namespace scrabble
