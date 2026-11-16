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
// what makes a record usable as training data.
//
// ParseGcg() reads records written by this engine and by other tools
// (Quackle, Woogles, hand-edited files). It is a syntactic parser: it
// recognises every event kind of the format but does not replay the moves
// against a board or check the scores; front ends that display a game do
// that themselves (see apps/server/games.cc).

#ifndef SCRABBLE_GCG_H_
#define SCRABBLE_GCG_H_

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "scrabble/game.h"
#include "scrabble/move.h"
#include "scrabble/rack.h"

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

// What a parsed event line records.
enum class GcgEventType : std::uint8_t {
  // A placement, exchange or pass. `move` is set, except for an exchange
  // recorded by count only ("-7"), which sets `exchange_count` instead.
  kMove,
  // "--": the mover's previous placement was challenged off the board.
  // `score` is minus the points it had scored.
  kWithdrawn,
  // "(challenge)": bonus for an unsuccessful challenge of this player's move.
  kChallengeBonus,
  // "(time)": overtime penalty.
  kTimePenalty,
  // "(TILES)": the player went out and scores the opponent's `tiles`.
  kEndPoints,
  // "TILES (TILES)": the player is left holding `tiles` and loses their
  // value.
  kRackPenalty,
};

struct GcgEvent {
  GcgEventType type = GcgEventType::kMove;
  int player = 0;  // seat, as for GcgInfo::players
  // The rack before the event, when the line records one.
  std::optional<Rack> rack;
  // kMove only. Play-through squares written as '.' are left unresolved
  // (MoveTile::kUnknownLetter); ValidateMove() resolves them against a
  // board.
  std::optional<Move> move;
  int exchange_count = 0;
  Rack tiles;           // kEndPoints and kRackPenalty
  int score = 0;        // signed points for this line
  int cumulative = 0;   // the player's total after it, as recorded
  std::string note;     // text of any "#note" lines that follow
  int line = 0;         // 1-based source line, for error messages
};

struct GcgGame {
  GcgInfo info;
  std::vector<GcgEvent> events;
};

// A malformed record. what() includes the line number.
class GcgParseError : public std::runtime_error {
 public:
  GcgParseError(int line, const std::string& message);
  int line() const { return line_; }

 private:
  int line_;
};

// Parses a GCG record. Pragmas other than #player1, #player2, #title,
// #lexicon and #note are ignored, as are blank lines. Players referenced by
// event lines but not declared by a #playerN pragma take the free seats in
// order of first appearance. Throws GcgParseError on a malformed line, a
// third player, or a record with no #player pragma and no events.
GcgGame ParseGcg(std::string_view text);

}  // namespace scrabble

#endif  // SCRABBLE_GCG_H_
