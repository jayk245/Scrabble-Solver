// JSON <-> engine conversions for the web API.
//
// The wire format is documented in web/src/api/types.ts, which mirrors it for
// the client; keep the two in sync. In short:
//
//   board   15 strings of 15 characters, row 1 first. '.' is an empty square,
//           'A'-'Z' a tile and 'a'-'z' a blank designated as that letter,
//           matching the case convention of move notation.
//   rack    a string of up to 7 tiles, '?' for a blank ("AEINST?").
//   move    an object with the notation plus the resolved squares, so the
//           client can draw a move without re-implementing notation parsing.
//
// Decoding failures throw ApiError carrying an HTTP status and a stable,
// machine-readable code; the request handlers turn that into the response.

#ifndef SCRABBLE_SERVER_CODEC_H_
#define SCRABBLE_SERVER_CODEC_H_

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "scrabble/board.h"
#include "scrabble/move.h"
#include "scrabble/move_validation.h"
#include "scrabble/rack.h"
#include "scrabble/tile.h"

namespace scrabble::server {

// A request the server cannot act on. `code` is a snake_case identifier the
// client may branch on; what() is a human-readable explanation.
class ApiError : public std::runtime_error {
 public:
  ApiError(int status, std::string code, const std::string& message)
      : std::runtime_error(message), status_(status), code_(std::move(code)) {}

  int status() const { return status_; }
  const std::string& code() const { return code_; }

 private:
  int status_;
  std::string code_;
};

// Parses a board in the 15x15 string format. Throws ApiError (400) naming
// the offending row and column on malformed input. Tile counts are not
// checked against the distribution here; see ComputeUnseen().
Board BoardFromJson(const nlohmann::json& value);
nlohmann::json BoardToJson(const Board& board);

// Parses a rack string. Throws ApiError (400) if it is not a string of at
// most kRackSize tiles.
Rack RackFromJson(const nlohmann::json& value);

// Parses notation with Move::Parse(). Throws ApiError (400, "bad_notation")
// on a syntax error.
Move MoveFromNotation(std::string_view notation);

// Exactly the tiles a move takes from a rack. Used when a move's rack is
// unknown (e.g. a pasted or recorded game) so validation still checks
// geometry and words but not tile ownership.
Rack RackForMove(const Move& move);

// Snake_case identifier for a validation result, e.g. "not_in_rack".
std::string_view MoveErrorCode(MoveError error);

// Serialises a move. Placements must be resolved (play-through letters
// known), as they are after ValidateMove() or from the move generator.
// `rack`, when given, is the rack the move was played from and adds the
// "leave" field.
nlohmann::json MoveToJson(const Move& move, const Rack* rack = nullptr);

// Tiles not visible to the player to move: the full distribution minus the
// board and the rack. Counts that would go negative (more of a letter on
// the board than the bag holds, which an analysis board allows) are clamped
// to zero and the letter is reported in `overused`.
struct Unseen {
  std::array<int, kAlphabetSize> counts{};
  int total = 0;
  std::string overused;  // letters as in a rack string, e.g. "Z?"
};
Unseen ComputeUnseen(const Board& board, const Rack& rack);

// {"unseen": {"A": 9, ...}, "unseenCount", "bagCount", "tilesOnBoard",
// "overused"}. bagCount assumes the opponent holds a full rack drawn from the
// unseen tiles.
nlohmann::json PositionSummaryToJson(const Board& board, const Rack& rack);

// The static rules the client renders from: premium layout, tile values and
// distribution, rack size and bingo bonus.
nlohmann::json RulesToJson();

}  // namespace scrabble::server

#endif  // SCRABBLE_SERVER_CODEC_H_
