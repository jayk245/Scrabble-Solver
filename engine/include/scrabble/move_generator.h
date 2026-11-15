// Legal-move generation with Gordon's GADDAG algorithm.
//
// For every anchor (an empty square orthogonally adjacent to a tile, or the
// centre on an empty board) the generator walks the GADDAG leftwards from the
// anchor, then crosses the separator and walks rightwards, placing rack tiles
// on empty squares that pass the cross-check and following board tiles
// through play-through squares. Scores are accumulated incrementally along
// the way and always agree with Score().
//
// Conventions:
//  * Each move is generated from the leftmost (topmost, for down moves)
//    anchor it covers with a rack tile: the leftward walk never places a tile
//    on another anchor. This makes every placement unique within a direction
//    without a dedupe pass.
//  * Down moves reuse the across code: each column is loaded into a 15-entry
//    line buffer (letters, premiums, down-move cross-checks), so the
//    recursion only ever sees a 1-D line. Nothing board-sized is copied.
//  * A single-tile play that forms words in both directions is emitted once,
//    as an across move. A single tile forming only a vertical word is a down
//    move. Every emitted move's main word has at least two letters, matching
//    ValidateMove().
//  * On an empty board the across and down plays through the centre are both
//    emitted; they are distinct placements.
//  * Score is filled in (bingo included); equity is left at zero.

#ifndef SCRABBLE_MOVE_GENERATOR_H_
#define SCRABBLE_MOVE_GENERATOR_H_

#include <array>
#include <cstdint>
#include <vector>

#include "scrabble/board.h"
#include "scrabble/cross_checks.h"
#include "scrabble/gaddag.h"
#include "scrabble/move.h"
#include "scrabble/position.h"
#include "scrabble/rack.h"

namespace scrabble {

struct MoveGenOptions {
  // Append every distinct non-empty sub-multiset of the rack as an exchange.
  // Whether exchanging is legal (bag size) is the caller's decision.
  bool include_exchanges = false;
  // Append a pass.
  bool include_pass = false;
};

// Holds per-call scratch state, so one instance must not be used from two
// threads at once; give each simulation thread its own. Cheap to construct.
class MoveGenerator {
 public:
  // `gaddag` must outlive the generator and must be the lexicon the
  // cross-checks were computed with.
  explicit MoveGenerator(const Gaddag& gaddag) : gaddag_(&gaddag) {}

  // Replaces the contents of `out` with every legal move for `rack`. `out`
  // is cleared, not shrunk, so reusing one vector across calls makes
  // generation allocation-free once it has grown to the largest move count
  // seen. Placements come first, then exchanges, then the pass.
  void Generate(const Position& position, const Rack& rack,
                std::vector<Move>& out, const MoveGenOptions& options = {}) {
    Generate(position.board(), position.cross_checks(), rack, out, options);
  }

  // As above for a board and matching cross-checks
  // (== CrossChecks::ComputeAll(board, gaddag)).
  void Generate(const Board& board, const CrossChecks& cross_checks,
                const Rack& rack, std::vector<Move>& out,
                const MoveGenOptions& options = {});

 private:
  // Running score of the partial move; small enough to pass in registers.
  struct Partial {
    int main_sum = 0;     // main-word letter points (premiums applied)
    int word_mult = 1;    // product of word premiums under placed tiles
    int cross_total = 0;  // finished cross-word scores
    int placed = 0;       // rack tiles placed so far
  };

  static constexpr Letter kEmptySquare = 0xFF;

  void LoadLine(const Board& board, const CrossChecks& cross_checks,
                Direction direction, int line, bool board_empty);
  void ExtendLeft(int pos, Gaddag::Node node, Partial partial);
  void AfterLeft(int pos, Gaddag::Node node, Partial partial);
  void ExtendRight(int pos, Gaddag::Node node, int left, Partial partial);
  void AfterRight(int pos, Gaddag::Node node, int left, Partial partial);
  template <typename Next>
  void PlaceAt(int pos, Gaddag::Node node, Partial partial, Next&& next);
  void Record(int left, int right, const Partial& partial);
  void AddExchanges(Letter letter, Move& exchange);

  const Gaddag* gaddag_;
  std::vector<Move>* out_ = nullptr;

  // Rack state during the search.
  std::array<std::uint8_t, kAlphabetSize> rack_{};
  std::uint32_t rack_letters_ = 0;  // letters (not blank) with count > 0
  int tiles_left_ = 0;

  // The line being searched: a row for across moves, a column for down.
  Direction direction_ = Direction::kAcross;
  int line_ = 0;
  int anchor_ = 0;
  bool right_of_anchor_open_ = false;  // anchor is the last square or the
                                       // next one is empty
  std::array<Letter, kBoardSize> letter_{};      // kEmptySquare if empty
  std::array<std::uint8_t, kBoardSize> value_{};  // face value of a tile
  std::array<std::uint32_t, kBoardSize> allowed_{};
  std::array<std::int16_t, kBoardSize> cross_score_{};  // -1: no cross-word
  std::array<std::uint8_t, kBoardSize> letter_mult_{};
  std::array<std::uint8_t, kBoardSize> word_mult_{};
  std::array<bool, kBoardSize> is_anchor_{};
  // Board tiles (as play-through) plus the rack tiles placed on the current
  // search path; a recorded move is a slice of this array.
  std::array<MoveTile, kBoardSize> tiles_{};
};

}  // namespace scrabble

#endif  // SCRABBLE_MOVE_GENERATOR_H_
