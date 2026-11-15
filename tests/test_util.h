// Shared test helpers: board construction from notation, the ENABLE lexicon,
// a brute-force reference move generator, and a random-game driver.

#ifndef SCRABBLE_TESTS_TEST_UTIL_H_
#define SCRABBLE_TESTS_TEST_UTIL_H_

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "scrabble/bag.h"
#include "scrabble/board.h"
#include "scrabble/gaddag.h"
#include "scrabble/move.h"
#include "scrabble/move_generator.h"
#include "scrabble/position.h"
#include "scrabble/rack.h"
#include "scrabble/rng.h"

namespace scrabble::testing {

// Release builds run the expensive randomized tests at full size; debug
// (sanitized) builds scale them down to keep ctest fast.
#ifdef NDEBUG
inline constexpr int kScale = 10;
#else
inline constexpr int kScale = 1;
#endif

Move ParseOrDie(const std::string& text);
Rack RackOrDie(const std::string& text);

// Places each move's rack tiles on the board without any legality check.
Board MakeBoard(const std::vector<std::string>& moves);

// The ENABLE GADDAG (loaded once per process from the cache), or nullptr if
// the word list has not been fetched.
const Gaddag* EnableGaddag();

// Every legal placement for `rack`, found by trying every start square,
// direction and ordered selection of rack tiles (every designation for a
// blank) and keeping what ValidateMove() accepts. With `prune`, a branch is
// abandoned once the main word so far is not a prefix of any lexicon word,
// which never discards a legal move. The generator's single-tile convention
// is applied so the result is directly comparable.
std::vector<Move> ReferenceMoves(const Board& board, const Gaddag& gaddag,
                                 const Rack& rack, bool prune = true);

// Passes if `generated` has no duplicates and equals `expected` as a set of
// (notation, score) pairs; otherwise lists up to a few differences.
::testing::AssertionResult SameMoves(const std::vector<Move>& generated,
                                     const std::vector<Move>& expected);

// Removes the rack tiles `move` uses from `rack`.
void TakeTiles(Rack& rack, const Move& move);

// Plays a game between two random movers drawing from a standard bag:
// each turn picks a uniformly random generated placement (or passes if there
// is none). Calls visit(position, rack, moves) before every turn. Ends when
// a player goes out or after four consecutive passes.
template <typename Visit>
void PlayRandomGame(const Gaddag& gaddag, std::uint64_t seed, Visit&& visit) {
  Rng rng(seed);
  Bag bag = Bag::Full();
  Position position(gaddag);
  MoveGenerator generator(gaddag);
  std::vector<Move> moves;
  Rack racks[2];
  bag.DrawInto(racks[0], rng);
  bag.DrawInto(racks[1], rng);
  int passes = 0;
  for (int turn = 0; passes < 4; turn ^= 1) {
    Rack& rack = racks[turn];
    generator.Generate(position, rack, moves);
    visit(static_cast<const Position&>(position),
          static_cast<const Rack&>(rack),
          static_cast<const std::vector<Move>&>(moves));
    if (moves.empty()) {
      ++passes;
      continue;
    }
    passes = 0;
    const Move move =
        moves[rng.Below(static_cast<std::uint32_t>(moves.size()))];
    position.Apply(move);
    TakeTiles(rack, move);
    bag.DrawInto(rack, rng);
    if (rack.IsEmpty()) break;
  }
}

}  // namespace scrabble::testing

#endif  // SCRABBLE_TESTS_TEST_UTIL_H_
