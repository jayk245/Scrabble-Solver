// 15x15 game board: premium-square layout and tile placement.
//
// Squares are stored row-major in a flat array. Coordinates are zero-based
// (row, col); human-facing notation (columns A-O, rows 1-15) is handled only
// at the formatting boundary.

#ifndef SCRABBLE_BOARD_H_
#define SCRABBLE_BOARD_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "scrabble/tile.h"

namespace scrabble {

inline constexpr int kBoardSize = 15;
inline constexpr int kNumSquares = kBoardSize * kBoardSize;
inline constexpr int kCenter = kBoardSize / 2;

enum class Premium : std::uint8_t {
  kNone,
  kDoubleLetter,
  kTripleLetter,
  kDoubleWord,
  kTripleWord,
};

constexpr bool InBounds(int row, int col) {
  return row >= 0 && row < kBoardSize && col >= 0 && col < kBoardSize;
}

constexpr std::size_t SquareIndex(int row, int col) {
  return static_cast<std::size_t>(row * kBoardSize + col);
}

namespace internal {

// Source-of-truth layout. T = triple word, D = double word, t = triple
// letter, d = double letter, * = centre (double word), . = plain.
inline constexpr std::array<std::string_view, kBoardSize> kPremiumRows = {
    "T..d...T...d..T",
    ".D...t...t...D.",
    "..D...d.d...D..",
    "d..D...d...D..d",
    "....D.....D....",
    ".t...t...t...t.",
    "..d...d.d...d..",
    "T..d...*...d..T",
    "..d...d.d...d..",
    ".t...t...t...t.",
    "....D.....D....",
    "d..D...d...D..d",
    "..D...d.d...D..",
    ".D...t...t...D.",
    "T..d...T...d..T",
};

constexpr Premium ParsePremium(char c) {
  switch (c) {
    case '.': return Premium::kNone;
    case 'd': return Premium::kDoubleLetter;
    case 't': return Premium::kTripleLetter;
    case 'D':
    case '*': return Premium::kDoubleWord;
    case 'T': return Premium::kTripleWord;
  }
  // Reached only for a malformed layout; in a constant expression this
  // surfaces as a compile error rather than a runtime failure.
  throw std::invalid_argument("unknown premium square code");
}

constexpr std::array<Premium, kNumSquares> BuildPremiumLayout() {
  std::array<Premium, kNumSquares> layout{};
  for (int row = 0; row < kBoardSize; ++row) {
    if (kPremiumRows[static_cast<std::size_t>(row)].size() != kBoardSize) {
      throw std::invalid_argument("premium row has wrong length");
    }
    for (int col = 0; col < kBoardSize; ++col) {
      layout[SquareIndex(row, col)] = ParsePremium(
          kPremiumRows[static_cast<std::size_t>(row)]
                      [static_cast<std::size_t>(col)]);
    }
  }
  return layout;
}

}  // namespace internal

// Premium layout resolved at compile time; no runtime initialisation cost.
inline constexpr std::array<Premium, kNumSquares> kPremiumLayout =
    internal::BuildPremiumLayout();

constexpr Premium PremiumAt(int row, int col) {
  return kPremiumLayout[SquareIndex(row, col)];
}

// A single board square. `letter` is always a concrete A-Z value when
// occupied; `is_blank` records that the tile scores zero.
struct Square {
  static constexpr Letter kEmpty = 0xFF;

  Letter letter = kEmpty;
  bool is_blank = false;

  constexpr bool IsEmpty() const { return letter == kEmpty; }

  bool operator==(const Square&) const = default;
};

class Board {
 public:
  Board() = default;

  const Square& At(int row, int col) const {
    return squares_[SquareIndex(row, col)];
  }

  // Places a tile on an empty square. `letter` must be A-Z (a played blank
  // is passed as its designated letter with `is_blank` set).
  void Place(int row, int col, Letter letter, bool is_blank = false);

  // Removes the tile at (row, col). Used to undo moves during search.
  void Remove(int row, int col);

  bool IsEmpty() const { return tile_count_ == 0; }
  int TileCount() const { return tile_count_; }

  // Renders the board as a fixed-width grid with A-O / 1-15 headers.
  // Occupied squares show the letter (lowercase for blanks); empty squares
  // show their premium: = TW, - DW, " TL, ' DL, * centre, . plain.
  std::string ToString() const;

  bool operator==(const Board&) const = default;

 private:
  std::array<Square, kNumSquares> squares_{};
  int tile_count_ = 0;
};

}  // namespace scrabble

#endif  // SCRABBLE_BOARD_H_
