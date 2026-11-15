#include "scrabble/position.h"

#include <cassert>
#include <cstddef>

namespace scrabble {

void PlaceTiles(Board& board, const Move& move) {
  assert(move.type == MoveType::kPlace);
  const int dr = move.direction == Direction::kDown ? 1 : 0;
  const int dc = 1 - dr;
  for (int i = 0; i < move.num_tiles; ++i) {
    const MoveTile tile = move.tiles[static_cast<std::size_t>(i)];
    if (tile.is_play_through()) continue;
    board.Place(move.row + i * dr, move.col + i * dc, tile.letter(),
                tile.is_blank());
  }
}

void RemoveTiles(Board& board, const Move& move) {
  assert(move.type == MoveType::kPlace);
  const int dr = move.direction == Direction::kDown ? 1 : 0;
  const int dc = 1 - dr;
  for (int i = 0; i < move.num_tiles; ++i) {
    if (move.tiles[static_cast<std::size_t>(i)].is_play_through()) continue;
    board.Remove(move.row + i * dr, move.col + i * dc);
  }
}

PositionUndo Position::Apply(const Move& move) {
  PositionUndo undo;
  if (move.type != MoveType::kPlace) return undo;
  PlaceTiles(board_, move);
  cross_checks_.Update(board_, *gaddag_, move, &undo.cross_checks);
  return undo;
}

void Position::Undo(const Move& move, const PositionUndo& undo) {
  if (move.type != MoveType::kPlace) return;
  // Restoring the snapshot is cheaper than recomputing, and exact because
  // the saved entries were computed for the pre-move board.
  cross_checks_.Restore(undo.cross_checks);
  RemoveTiles(board_, move);
}

}  // namespace scrabble
