// Move scoring.
//
// Score() deliberately works from the board alone, square by square, without
// cross-checks or move-generation state, so it serves as an independent
// oracle for the move generator's incremental scoring.

#ifndef SCRABBLE_SCORING_H_
#define SCRABBLE_SCORING_H_

#include "scrabble/board.h"
#include "scrabble/move.h"

namespace scrabble {

constexpr int LetterMultiplier(Premium premium) {
  switch (premium) {
    case Premium::kDoubleLetter: return 2;
    case Premium::kTripleLetter: return 3;
    default: return 1;
  }
}

constexpr int WordMultiplier(Premium premium) {
  switch (premium) {
    case Premium::kDoubleWord: return 2;
    case Premium::kTripleWord: return 3;
    default: return 1;
  }
}

// Points for `move` played on `board`, which must not yet contain the move's
// tiles. Counts the main word (if at least two letters long) and every
// perpendicular word formed by a newly placed tile, applying premiums only
// under new tiles, scoring blanks as zero, and adding kBingoBonus when all
// kRackSize tiles are used. Pass and exchange score 0.
//
// Precondition: the move's span covers its whole main word (no tile adjoins
// either end) and its placed squares are empty. Words are not checked
// against a lexicon; play-through letters are read from the board, so '.'
// placeholders need not be resolved.
int Score(const Board& board, const Move& move);

}  // namespace scrabble

#endif  // SCRABBLE_SCORING_H_
