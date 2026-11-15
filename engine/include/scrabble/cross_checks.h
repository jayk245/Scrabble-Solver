// Per-square cross-checks: which letters may be placed on an empty square
// without forming an invalid perpendicular word, and what that word scores.
//
// Cross-checks are a pure function of (board, lexicon). They are kept as a
// separate value type rather than folded into Board so that (a) scoring and
// validation, which do not need them, stay lexicon-free, and (b) tests can
// compare an incrementally maintained instance against ComputeAll() with a
// plain operator==. Position (position.h) bundles a Board with its
// CrossChecks and keeps the two in sync.

#ifndef SCRABBLE_CROSS_CHECKS_H_
#define SCRABBLE_CROSS_CHECKS_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "scrabble/board.h"
#include "scrabble/gaddag.h"
#include "scrabble/letter_set.h"
#include "scrabble/move.h"

namespace scrabble {

constexpr Direction Perpendicular(Direction direction) {
  return direction == Direction::kAcross ? Direction::kDown
                                         : Direction::kAcross;
}

// The constraint a move along some direction meets at one square. For an
// across move this describes the vertical word through the square; for a
// down move, the horizontal one.
struct CrossCheck {
  // Letters that complete a valid perpendicular word. All() when there is no
  // perpendicular neighbour; empty for occupied squares.
  LetterSet allowed = LetterSet::All();
  // Face value of the perpendicular neighbours (blanks count zero), i.e. the
  // cross-word score before the new tile and multipliers are added.
  std::int16_t score = 0;
  // Distinguishes "no cross-word" from "a cross-word worth 0 points" (e.g.
  // a neighbouring blank). Only true for empty squares.
  bool has_cross_word = false;

  bool operator==(const CrossCheck&) const = default;
};

// The value stored for occupied squares: nothing may be placed there.
inline constexpr CrossCheck kOccupiedCrossCheck = {
    .allowed = LetterSet(), .score = 0, .has_cross_word = false};

// Undo log for CrossChecks::Update(): the entries it overwrote, in order.
// Restoring them in reverse is exact even if a square appears twice.
struct CrossCheckUndo {
  // Each placed tile touches its own square (both directions) and the two
  // squares capping its perpendicular run; the main line adds two caps.
  static constexpr int kMaxEntries = 4 * kRackSize + 2;

  struct Entry {
    std::uint8_t square;
    Direction direction;
    CrossCheck value;
  };

  std::array<Entry, kMaxEntries> entries;
  int count = 0;
};

class CrossChecks {
 public:
  // Cross-checks for an empty board: everything allowed everywhere.
  CrossChecks() = default;

  // Full recompute from scratch. O(squares); the reference for Update().
  static CrossChecks ComputeAll(const Board& board, const Gaddag& gaddag);

  // The constraint for a move along `direction` at square (row, col).
  const CrossCheck& For(Direction direction, int row, int col) const {
    return For(direction, SquareIndex(row, col));
  }
  const CrossCheck& For(Direction direction, std::size_t square) const {
    return checks_[static_cast<std::size_t>(direction)][square];
  }

  // Refreshes exactly the entries a placement can affect: the placed
  // squares, the first empty square beyond each end of every perpendicular
  // run through a placed tile, and the first empty square beyond each end of
  // the main line. Call after `move`'s tiles have been placed on `board`, or
  // after they have been removed again; either way the result equals
  // ComputeAll(board). Pass `undo` to record overwritten entries for
  // Restore(). Precondition: `move` is a placement within the board.
  void Update(const Board& board, const Gaddag& gaddag, const Move& move,
              CrossCheckUndo* undo = nullptr);

  // Reverts the entries recorded by Update(), bit for bit.
  void Restore(const CrossCheckUndo& undo);

  // The cross-check for one square, computed directly from the board.
  static CrossCheck Compute(const Board& board, const Gaddag& gaddag,
                            Direction direction, int row, int col);

  bool operator==(const CrossChecks&) const = default;

 private:
  void Set(const Board& board, const Gaddag& gaddag, Direction direction,
           int row, int col, CrossCheckUndo* undo);

  // [direction][square]. Default member initialisers give the empty-board
  // state without running ComputeAll().
  std::array<std::array<CrossCheck, kNumSquares>, 2> checks_{};
};

}  // namespace scrabble

#endif  // SCRABBLE_CROSS_CHECKS_H_
