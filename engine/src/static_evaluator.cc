#include "scrabble/static_evaluator.h"

#include <cassert>
#include <cstddef>

namespace scrabble {

void StaticEvaluator::Prepare(const Rack& rack, bool bag_empty,
                              int opponent_rack_value) {
  int combinations = 1;
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    const int count = rack.Count(letter);
    stride_[letter] = count > 0 ? combinations : 0;
    combinations *= count + 1;
  }
  assert(combinations <= kMaxLeaves);
  full_rack_index_ = combinations - 1;

  for (int index = 0; index < combinations; ++index) {
    Rack leave;
    for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
      const int stride = stride_[letter];
      if (stride == 0) continue;
      const int keep = (index / stride) % (rack.Count(letter) + 1);
      for (int n = 0; n < keep; ++n) leave.Add(letter);
    }
    double value;
    if (!bag_empty) {
      value = leaves_->Value(leave);
    } else if (leave.IsEmpty()) {
      value = 2.0 * opponent_rack_value;
    } else {
      value = -2.0 * leave.Score();
    }
    values_[static_cast<std::size_t>(index)] = value;
  }
}

double StaticEvaluator::LeaveTerm(const Move& move) const {
  int index = full_rack_index_;
  for (std::size_t i = 0; i < move.num_tiles; ++i) {
    const MoveTile tile = move.tiles[i];
    if (move.type == MoveType::kPlace) {
      if (tile.is_play_through()) continue;
      index -= stride_[tile.is_blank() ? kBlank : tile.letter()];
    } else {
      index -= stride_[tile.letter()];
    }
  }
  assert(index >= 0);
  return values_[static_cast<std::size_t>(index)];
}

}  // namespace scrabble
