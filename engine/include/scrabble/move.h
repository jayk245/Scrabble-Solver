// Move representation and standard Scrabble notation.
//
// A Move is a 32-byte, trivially copyable value so move generation can emit
// millions of them into preallocated buffers without touching the heap.
//
// Notation (as used by tournament tools such as Quackle):
//   "8H HELLO"     placement across, starting at row 8, column H
//   "H8 HELLO"     placement down: column letter first
//   "8D QU(I)T"    letters in parentheses are already on the board
//   "8D QU.T"      '.' is also accepted for a play-through square whose
//                  letter is unknown (parse-only convenience; see below)
//   "8H HeLLO"     lowercase letters are blanks
//   "-ABC?"        exchange (also parsed: "exch ABC?")
//   "-"            pass (also parsed: "pass")
// ToString() always emits the first form of each: parentheses for known
// play-through letters, "-TILES" for exchanges and "-" for a pass.

#ifndef SCRABBLE_MOVE_H_
#define SCRABBLE_MOVE_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "scrabble/board.h"
#include "scrabble/rack.h"
#include "scrabble/tile.h"

namespace scrabble {

enum class MoveType : std::uint8_t { kPass, kPlace, kExchange };
enum class Direction : std::uint8_t { kAcross, kDown };

// A placement spans at most one full row or column.
inline constexpr int kMaxMoveTiles = kBoardSize;

// One square of a placement, or one tile of an exchange, packed into a byte:
//   bits 0-4  letter (A-Z = 0-25, kBlank = 26 in exchanges only,
//             kUnknownLetter = 31 for an unresolved play-through)
//   bit 5     tile is a blank (scores zero); `letter` is its designation
//   bit 6     play-through: the square is already occupied on the board and
//             contributes no tile from the rack
class MoveTile {
 public:
  static constexpr Letter kUnknownLetter = 0x1F;

  constexpr MoveTile() = default;

  // A tile placed from the rack. A blank is passed as its designated letter
  // with `is_blank` set.
  static constexpr MoveTile Placed(Letter letter, bool is_blank = false) {
    return MoveTile(letter, is_blank, /*play_through=*/false);
  }

  // A square already occupied on the board. `letter` may be kUnknownLetter
  // when parsed from '.' notation; such moves must be resolved against a
  // board before scoring.
  static constexpr MoveTile PlayThrough(Letter letter, bool is_blank = false) {
    return MoveTile(letter, is_blank, /*play_through=*/true);
  }

  constexpr Letter letter() const { return code_ & kLetterMask; }
  constexpr bool is_blank() const { return (code_ & kBlankBit) != 0; }
  constexpr bool is_play_through() const {
    return (code_ & kPlayThroughBit) != 0;
  }

  bool operator==(const MoveTile&) const = default;

 private:
  static constexpr std::uint8_t kLetterMask = 0x1F;
  static constexpr std::uint8_t kBlankBit = 0x20;
  static constexpr std::uint8_t kPlayThroughBit = 0x40;

  constexpr MoveTile(Letter letter, bool is_blank, bool play_through)
      : code_(static_cast<std::uint8_t>(
            (letter & kLetterMask) | (is_blank ? kBlankBit : 0) |
            (play_through ? kPlayThroughBit : 0))) {}

  std::uint8_t code_ = 0;
};

// Plain data by design: generators fill fields directly. Fields are ordered
// largest-first so the struct packs into 32 bytes with no padding.
//
// Invariant: tiles[num_tiles, kMaxMoveTiles) are default-constructed, so the
// defaulted operator== is meaningful. For kPlace, (row, col) is the
// zero-based square of tiles[0] and the word extends along `direction`. For
// kExchange, tiles holds the exchanged tiles (blank as kBlank) and position
// fields are unused. For kPass, only `type` is meaningful.
struct Move {
  // Estimated value of the move (score plus leave, refined by simulation).
  // Filled in by evaluators; zero until then.
  double equity = 0.0;
  // Points scored by the placement, including any bingo bonus.
  std::int32_t score = 0;
  std::uint8_t row = 0;
  std::uint8_t col = 0;
  std::uint8_t num_tiles = 0;
  MoveType type = MoveType::kPass;
  Direction direction = Direction::kAcross;
  std::array<MoveTile, kMaxMoveTiles> tiles{};

  static constexpr Move Pass() { return Move{}; }

  // An exchange of exactly the tiles in `rack`, in canonical order.
  static Move Exchange(const Rack& rack);

  // An empty placement anchored at (row, col); append tiles with AddTile().
  static constexpr Move Place(int row, int col, Direction direction) {
    Move move;
    move.type = MoveType::kPlace;
    move.row = static_cast<std::uint8_t>(row);
    move.col = static_cast<std::uint8_t>(col);
    move.direction = direction;
    return move;
  }

  // Requires num_tiles < kMaxMoveTiles.
  constexpr void AddTile(MoveTile tile) { tiles[num_tiles++] = tile; }

  // Tiles taken from the rack: all tiles for an exchange, non-play-through
  // tiles for a placement, zero for a pass.
  int TilesUsed() const;

  // Renders standard notation (see file comment). Score and equity are not
  // part of the notation.
  std::string ToString() const;

  // Parses standard notation. Validates syntax only (coordinates on the
  // board, word fits, 1-7 rack tiles, well-formed parentheses); does not
  // consult a board or lexicon. Score and equity are left zero.
  static std::optional<Move> Parse(std::string_view text);

  bool operator==(const Move&) const = default;
};

// The tiles left on `rack` after `move` takes its rack tiles: the leave.
// A pass leaves the whole rack. Precondition: `rack` holds every tile the
// move uses (as ValidateMove() guarantees).
Rack LeaveAfter(const Rack& rack, const Move& move);

static_assert(sizeof(MoveTile) == 1);
static_assert(sizeof(Move) == 32, "Move layout changed; check packing");
static_assert(std::is_trivially_copyable_v<Move>);

}  // namespace scrabble

#endif  // SCRABBLE_MOVE_H_
