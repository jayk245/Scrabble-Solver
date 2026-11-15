// Export of finished or in-progress games in the GCG format.
//
// GCG (poslfit.com/scrabble/gcg) is the de facto interchange format for
// Scrabble game records, read by Quackle, Woogles and annotation tools.
// Output looks like:
//
//   #character-encoding UTF-8
//   #player1 alice Alice
//   #player2 bob Bob
//   >alice: AEINRST 8D RETAINS +76 76      placement
//   >bob: DGOOORT -OOO +0 0                exchange
//   >alice: ACDEOPR E5 COP.ED +24 100      '.' = tile already on the board
//   >bob: DGORTUV - +0 0                   pass
//   ...
//   >alice: (DGT) +10 410                  went out: 2x opponent rack (NASPA)
//   >bob: DGT (DGT) -5 352                 rack penalty (WESPA, six zeros)
//
// Every move line carries the mover's full rack before the move, which is
// what makes a record usable as training data. Parsing is not supported.

#ifndef SCRABBLE_GCG_H_
#define SCRABBLE_GCG_H_

#include <array>
#include <string>

#include "scrabble/game.h"

namespace scrabble {

struct GcgPlayer {
  // Must be non-empty and contain no whitespace (GCG delimits by spaces).
  std::string nickname;
  // Free text; defaults to the nickname when empty.
  std::string full_name;
};

struct GcgInfo {
  // Indexed by seat: players[0] moved first.
  std::array<GcgPlayer, kNumPlayers> players;
  std::string title;    // optional "#title" pragma
  std::string lexicon;  // optional "#lexicon" pragma, e.g. "ENABLE"
};

// Renders `game` as GCG. Moves are written in history order; if the game is
// over, the end-of-game rack adjustment lines follow. Throws
// std::invalid_argument if a nickname is empty or contains whitespace.
std::string ToGcg(const Game& game, const GcgInfo& info);

}  // namespace scrabble

#endif  // SCRABBLE_GCG_H_
