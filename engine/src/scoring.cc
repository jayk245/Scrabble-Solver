#include "scrabble/scoring.h"

#include <cstddef>

namespace scrabble {
namespace {

int FaceValue(const Square& square) {
  return square.is_blank ? 0 : kTileValues[square.letter];
}

}  // namespace

int Score(const Board& board, const Move& move) {
  if (move.type != MoveType::kPlace) return 0;

  const int dr = move.direction == Direction::kDown ? 1 : 0;
  const int dc = 1 - dr;
  // Perpendicular step for cross-words.
  const int pr = dc;
  const int pc = dr;

  int main_sum = 0;
  int main_multiplier = 1;
  int cross_total = 0;
  int placed = 0;
  for (int i = 0; i < move.num_tiles; ++i) {
    const int row = move.row + i * dr;
    const int col = move.col + i * dc;
    const MoveTile tile = move.tiles[static_cast<std::size_t>(i)];
    if (tile.is_play_through()) {
      // Premiums under existing tiles were consumed when they were played.
      main_sum += FaceValue(board.At(row, col));
      continue;
    }
    ++placed;
    const Premium premium = PremiumAt(row, col);
    const int value =
        (tile.is_blank() ? 0 : kTileValues[tile.letter()]) *
        LetterMultiplier(premium);
    const int word_multiplier = WordMultiplier(premium);
    main_sum += value;
    main_multiplier *= word_multiplier;

    int cross_sum = 0;
    bool has_cross = false;
    for (const int sign : {-1, 1}) {
      for (int r = row + sign * pr, c = col + sign * pc;
           InBounds(r, c) && !board.At(r, c).IsEmpty();
           r += sign * pr, c += sign * pc) {
        cross_sum += FaceValue(board.At(r, c));
        has_cross = true;
      }
    }
    if (has_cross) cross_total += (cross_sum + value) * word_multiplier;
  }

  // A one-square span is not a word along this direction; its only word (if
  // any) is the cross-word already counted.
  const int main_score = move.num_tiles >= 2 ? main_sum * main_multiplier : 0;
  return main_score + cross_total + (placed == kRackSize ? kBingoBonus : 0);
}

}  // namespace scrabble
