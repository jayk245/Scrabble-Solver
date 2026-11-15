#include "scrabble/move_validation.h"

#include <array>
#include <cstddef>
#include <span>

#include "scrabble/scoring.h"

namespace scrabble {
namespace {

bool Occupied(const Board& board, int row, int col) {
  return InBounds(row, col) && !board.At(row, col).IsEmpty();
}

MoveError ValidateExchange(const Rack& rack, const Move& move) {
  if (move.num_tiles == 0) return MoveError::kEmptyMove;
  if (move.num_tiles > kRackSize) return MoveError::kTooManyTiles;
  Rack needed;
  for (int i = 0; i < move.num_tiles; ++i) {
    const MoveTile tile = move.tiles[static_cast<std::size_t>(i)];
    if (tile.letter() >= kAlphabetSize || tile.is_play_through()) {
      return MoveError::kBadTile;
    }
    needed.Add(tile.letter());
  }
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    if (needed.Count(letter) > rack.Count(letter)) return MoveError::kNotInRack;
  }
  return MoveError::kOk;
}

// Validates `move` (a placement) and canonicalises `resolved`, a copy of it.
MoveError ValidatePlacement(const Board& board, const Gaddag& gaddag,
                            const Rack& rack, Move& resolved) {
  const int n = resolved.num_tiles;
  if (n == 0) return MoveError::kEmptyMove;
  const int dr = resolved.direction == Direction::kDown ? 1 : 0;
  const int dc = 1 - dr;
  const int row0 = resolved.row;
  const int col0 = resolved.col;
  if (n > kMaxMoveTiles || !InBounds(row0, col0) ||
      !InBounds(row0 + (n - 1) * dr, col0 + (n - 1) * dc)) {
    return MoveError::kOutOfBounds;
  }

  // Geometry and play-through resolution.
  std::array<Letter, kMaxMoveTiles> word{};
  Rack needed;
  int placed = 0;
  for (int i = 0; i < n; ++i) {
    const int row = row0 + i * dr;
    const int col = col0 + i * dc;
    MoveTile& tile = resolved.tiles[static_cast<std::size_t>(i)];
    const Square& square = board.At(row, col);
    if (tile.is_play_through()) {
      if (square.IsEmpty()) return MoveError::kPlayThroughEmpty;
      if (tile.letter() != MoveTile::kUnknownLetter &&
          tile.letter() != square.letter) {
        return MoveError::kPlayThroughMismatch;
      }
      tile = MoveTile::PlayThrough(square.letter, square.is_blank);
    } else {
      if (tile.letter() >= kNumLetters) return MoveError::kBadTile;
      if (!square.IsEmpty()) return MoveError::kSquareOccupied;
      if (++placed > kRackSize) return MoveError::kTooManyTiles;
      needed.Add(tile.is_blank() ? kBlank : tile.letter());
    }
    word[static_cast<std::size_t>(i)] = tile.letter();
  }
  if (Occupied(board, row0 - dr, col0 - dc) ||
      Occupied(board, row0 + n * dr, col0 + n * dc)) {
    return MoveError::kIncompleteWord;
  }
  if (placed == 0) return MoveError::kNoRackTiles;
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    if (needed.Count(letter) > rack.Count(letter)) return MoveError::kNotInRack;
  }
  if (n < 2) return MoveError::kWordTooShort;

  // Connectivity: the first move covers the centre; later moves play through
  // a tile or place one next to an existing tile.
  if (board.IsEmpty()) {
    const bool covers_center =
        dr == 0 ? (row0 == kCenter && col0 <= kCenter && kCenter < col0 + n)
                : (col0 == kCenter && row0 <= kCenter && kCenter < row0 + n);
    if (!covers_center) return MoveError::kFirstMoveOffCenter;
  } else {
    bool connected = placed < n;  // any play-through square
    for (int i = 0; i < n && !connected; ++i) {
      const int row = row0 + i * dr;
      const int col = col0 + i * dc;
      connected = Occupied(board, row - dc, col - dr) ||
                  Occupied(board, row + dc, col + dr);
    }
    if (!connected) return MoveError::kNotConnected;
  }

  if (!gaddag.ContainsLetters(
          std::span<const Letter>(word.data(), static_cast<std::size_t>(n)))) {
    return MoveError::kInvalidWord;
  }

  // Cross-words through each newly placed tile.
  const int pr = dc;
  const int pc = dr;
  for (int i = 0; i < n; ++i) {
    const MoveTile tile = resolved.tiles[static_cast<std::size_t>(i)];
    if (tile.is_play_through()) continue;
    const int row = row0 + i * dr;
    const int col = col0 + i * dc;
    int begin = 0;
    while (Occupied(board, row + (begin - 1) * pr, col + (begin - 1) * pc)) {
      --begin;
    }
    int end = 0;
    while (Occupied(board, row + (end + 1) * pr, col + (end + 1) * pc)) ++end;
    if (begin == 0 && end == 0) continue;
    std::array<Letter, kBoardSize> cross{};
    std::size_t length = 0;
    for (int k = begin; k <= end; ++k) {
      cross[length++] = k == 0 ? tile.letter()
                               : board.At(row + k * pr, col + k * pc).letter;
    }
    if (!gaddag.ContainsLetters(
            std::span<const Letter>(cross.data(), length))) {
      return MoveError::kInvalidCrossWord;
    }
  }
  return MoveError::kOk;
}

}  // namespace

std::string_view MoveErrorMessage(MoveError error) {
  switch (error) {
    case MoveError::kOk: return "ok";
    case MoveError::kEmptyMove: return "move places no tiles";
    case MoveError::kOutOfBounds: return "move extends off the board";
    case MoveError::kBadTile: return "malformed tile";
    case MoveError::kSquareOccupied: return "square is already occupied";
    case MoveError::kPlayThroughEmpty:
      return "play-through square is empty";
    case MoveError::kPlayThroughMismatch:
      return "play-through letter does not match the board";
    case MoveError::kIncompleteWord:
      return "word continues past the end of the move";
    case MoveError::kNoRackTiles: return "move uses no tiles from the rack";
    case MoveError::kTooManyTiles: return "move uses too many tiles";
    case MoveError::kNotInRack: return "rack does not hold the tiles";
    case MoveError::kWordTooShort: return "word must have at least 2 letters";
    case MoveError::kFirstMoveOffCenter:
      return "first move must cover the centre square";
    case MoveError::kNotConnected:
      return "move does not connect to existing tiles";
    case MoveError::kInvalidWord: return "word is not in the lexicon";
    case MoveError::kInvalidCrossWord:
      return "cross-word is not in the lexicon";
  }
  return "unknown error";
}

MoveError ValidateMove(const Board& board, const Gaddag& gaddag,
                       const Rack& rack, Move& move) {
  switch (move.type) {
    case MoveType::kPass:
      return MoveError::kOk;
    case MoveType::kExchange:
      return ValidateExchange(rack, move);
    case MoveType::kPlace:
      break;
  }
  Move resolved = move;
  const MoveError error = ValidatePlacement(board, gaddag, rack, resolved);
  if (error != MoveError::kOk) return error;
  resolved.score = Score(board, resolved);
  move = resolved;
  return MoveError::kOk;
}

}  // namespace scrabble
