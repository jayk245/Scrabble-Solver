// Move legality checking for human input and as a test oracle.
//
// ValidateMove() works from the board and lexicon alone, without
// cross-checks or move-generation state, so it independently verifies the
// move generator's output.

#ifndef SCRABBLE_MOVE_VALIDATION_H_
#define SCRABBLE_MOVE_VALIDATION_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "scrabble/board.h"
#include "scrabble/gaddag.h"
#include "scrabble/move.h"
#include "scrabble/rack.h"

namespace scrabble {

// Why ValidateMove() rejected a move. kOk means legal.
enum class MoveError : std::uint8_t {
  kOk,
  kEmptyMove,             // placement with no squares
  kOutOfBounds,           // start or end square off the board
  kBadTile,               // malformed tile (e.g. a blank code in a placement)
  kSquareOccupied,        // a rack tile is placed on an occupied square
  kPlayThroughEmpty,      // a play-through square is empty on the board
  kPlayThroughMismatch,   // a play-through letter disagrees with the board
  kIncompleteWord,        // a tile adjoins the start or end of the span
  kNoRackTiles,           // only play-through tiles
  kTooManyTiles,          // more than kRackSize rack tiles
  kNotInRack,             // the rack lacks a required tile
  kWordTooShort,          // main word has a single letter
  kFirstMoveOffCenter,    // first placement misses the centre square
  kNotConnected,          // placement touches no existing tile
  kInvalidWord,           // main word not in the lexicon
  kInvalidCrossWord,      // some perpendicular word not in the lexicon
};

// Human-readable reason, e.g. for the web UI.
std::string_view MoveErrorMessage(MoveError error);

// Like MoveErrorMessage(), but names the offending word for lexicon errors
// when it is known, e.g. "cross-word ZA is not in the lexicon".
std::string DescribeMoveError(MoveError error, std::string_view invalid_word);

// Checks that `move` is legal for a player holding `rack` on `board`.
//
// Placements: geometry (on the board, no rack tile on an occupied square),
// the span covers the whole main word, 1..kRackSize rack tiles available on
// the rack (blank-designated tiles consume '?'), the first move covers the
// centre and later moves touch existing tiles, the main word has at least two
// letters, and the main word and every cross-word are in `gaddag`.
// Exchanges: 1..kRackSize tiles, all on the rack (whether the bag holds
// enough tiles is a game-state rule checked by the caller). Pass: always ok.
//
// On success `move` is canonicalised in place: play-through tiles take their
// letter and blank flag from the board (resolving '.' placeholders) and
// `score` is set to Score(board, move). On failure `move` is unchanged.
//
// When the move fails with kInvalidWord or kInvalidCrossWord and
// `invalid_word` is non-null, it receives the rejected word in uppercase.
//
// Single-tile placements must name the direction in which they form a word
// of at least two letters; when they form words both ways either direction
// is accepted (the move generator reports such plays across only).
MoveError ValidateMove(const Board& board, const Gaddag& gaddag,
                       const Rack& rack, Move& move,
                       std::string* invalid_word = nullptr);

}  // namespace scrabble

#endif  // SCRABBLE_MOVE_VALIDATION_H_
