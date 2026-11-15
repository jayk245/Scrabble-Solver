// A board together with its cross-checks, plus make/unmake of placements.
//
// This is the unit that simulation and endgame search copy and mutate: it is
// trivially copyable (~4 KB, no heap), and Apply()/Undo() touch only the
// squares a move can affect, so the cross-checks never need a full rebuild
// during search.

#ifndef SCRABBLE_POSITION_H_
#define SCRABBLE_POSITION_H_

#include "scrabble/board.h"
#include "scrabble/cross_checks.h"
#include "scrabble/gaddag.h"
#include "scrabble/move.h"

namespace scrabble {

// Puts the rack tiles of a placement on the board, skipping play-through
// squares. Precondition: every non-play-through square is empty and in
// bounds (as guaranteed by ValidateMove()).
void PlaceTiles(Board& board, const Move& move);

// Exact inverse of PlaceTiles(): removes the move's rack tiles.
void RemoveTiles(Board& board, const Move& move);

// Everything Undo() needs to revert one Apply().
struct PositionUndo {
  CrossCheckUndo cross_checks;
};

// Invariant: cross_checks() == CrossChecks::ComputeAll(board(), lexicon()).
class Position {
 public:
  // An empty board. `gaddag` must outlive the Position.
  explicit Position(const Gaddag& gaddag) : gaddag_(&gaddag) {}

  // An arbitrary board; computes its cross-checks from scratch.
  Position(const Gaddag& gaddag, const Board& board)
      : gaddag_(&gaddag),
        board_(board),
        cross_checks_(CrossChecks::ComputeAll(board, gaddag)) {}

  const Board& board() const { return board_; }
  const CrossChecks& cross_checks() const { return cross_checks_; }
  const Gaddag& lexicon() const { return *gaddag_; }

  // Plays a placement; passes and exchanges do not change the board and are
  // accepted as no-ops. Returns the record Undo() needs. Precondition: the
  // move is legal on this board (play-through letters need not be resolved).
  PositionUndo Apply(const Move& move);

  // Reverts the Apply() that returned `undo`. Moves must be undone in LIFO
  // order. Restores board and cross-checks bit for bit.
  void Undo(const Move& move, const PositionUndo& undo);

 private:
  const Gaddag* gaddag_;  // pointer, not reference, to keep Position copyable
  Board board_;
  CrossChecks cross_checks_;
};

}  // namespace scrabble

#endif  // SCRABBLE_POSITION_H_
