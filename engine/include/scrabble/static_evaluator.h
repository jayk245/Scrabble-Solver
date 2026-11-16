// Static equity of moves from one rack: score plus a leave term.
//
// Shared by the Bot's top-level ranking and by the static policy that plays
// the plies of a simulation rollout, so both rank moves identically.
//
// Leave term:
//   * Tiles left in the bag: the LeaveEvaluator's value of the tiles kept.
//   * Bag empty: nothing more is drawn, so the term becomes the end-of-game
//     rack swing instead: +2x the opponent's rack value for going out,
//     otherwise -2x the face value of the tiles kept (the opponent will
//     likely go out next). The 2x holds for both EndRules, since they differ
//     only in who carries the swing.

#ifndef SCRABBLE_STATIC_EVALUATOR_H_
#define SCRABBLE_STATIC_EVALUATOR_H_

#include <array>
#include <cstddef>

#include "scrabble/leave_evaluator.h"
#include "scrabble/move.h"
#include "scrabble/rack.h"
#include "scrabble/tile.h"

namespace scrabble {

// Caches the leave term of every sub-multiset of one rack, so a move's
// equity costs a few index subtractions instead of a hash lookup. Prepare()
// once per rack, then query any number of moves made from that rack.
// Allocation-free; not thread-safe (give each thread its own).
class StaticEvaluator {
 public:
  // `leaves` must outlive the evaluator.
  explicit StaticEvaluator(const LeaveEvaluator& leaves) : leaves_(&leaves) {}

  // Fills the cache for moves made from `rack`. With `bag_empty`, uses the
  // end-of-game rule, where `opponent_rack_value` is the face value of the
  // opponent's tiles (with the bag empty, every unseen tile).
  void Prepare(const Rack& rack, bool bag_empty, int opponent_rack_value);

  // The leave term of `move`. Precondition: Prepare() was last called with
  // a rack holding every tile the move uses.
  double LeaveTerm(const Move& move) const;

  double Equity(const Move& move) const { return move.score + LeaveTerm(move); }

 private:
  // Maps every sub-multiset of the rack to a dense index. With per-letter
  // counts c[l], letter l gets stride prod(c[k] + 1 for distinct letters k
  // before l), and a leave keeping n[l] copies has index
  // sum(n[l] * stride[l]). A 7-tile rack has at most 2^7 = 128
  // sub-multisets (seven distinct tiles).
  static constexpr int kMaxLeaves = 1 << kRackSize;

  const LeaveEvaluator* leaves_;
  std::array<int, kAlphabetSize> stride_{};
  int full_rack_index_ = 0;
  std::array<double, kMaxLeaves> values_{};
};

}  // namespace scrabble

#endif  // SCRABBLE_STATIC_EVALUATOR_H_
