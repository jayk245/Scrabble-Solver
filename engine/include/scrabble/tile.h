// Tile alphabet, point values and bag distribution for English Scrabble.
//
// Letters are stored as small integers (A = 0 ... Z = 25) rather than chars so
// they can index lookup tables and bitmasks directly in hot paths such as move
// generation and cross-check computation.

#ifndef SCRABBLE_TILE_H_
#define SCRABBLE_TILE_H_

#include <array>
#include <cstdint>
#include <numeric>
#include <optional>

namespace scrabble {

using Letter = std::uint8_t;

inline constexpr int kNumLetters = 26;
// Index of an undesignated blank tile. Only meaningful on a rack or in the
// bag; once played, a blank takes a concrete letter and is flagged instead.
inline constexpr Letter kBlank = 26;
inline constexpr int kAlphabetSize = kNumLetters + 1;

inline constexpr int kRackSize = 7;
inline constexpr int kBingoBonus = 50;

// Point value per tile, indexed by Letter. Blanks score zero.
inline constexpr std::array<int, kAlphabetSize> kTileValues = {
    1, 3, 3, 2, 1, 4, 2, 4, 1, 8,  // A-J
    5, 1, 3, 1, 1, 3, 10, 1, 1, 1,  // K-T
    1, 4, 4, 8, 4, 10,              // U-Z
    0,                              // blank
};

// Number of copies of each tile in a fresh bag, indexed by Letter.
inline constexpr std::array<int, kAlphabetSize> kTileCounts = {
    9, 2, 2, 4, 12, 2, 3, 2, 9, 1,  // A-J
    1, 4, 2, 6, 8, 2, 1, 6, 4, 6,   // K-T
    4, 2, 2, 1, 2, 1,               // U-Z
    2,                              // blank
};

inline constexpr int kTotalTiles =
    std::accumulate(kTileCounts.begin(), kTileCounts.end(), 0);
static_assert(kTotalTiles == 100, "Standard bag must contain 100 tiles");

// Returns 'A'-'Z' for letters and '?' for the blank.
constexpr char LetterToChar(Letter letter) {
  return letter == kBlank ? '?' : static_cast<char>('A' + letter);
}

// Parses a tile character. Accepts either case for letters and '?' for a
// blank; returns std::nullopt for anything else.
constexpr std::optional<Letter> CharToLetter(char c) {
  if (c >= 'A' && c <= 'Z') return static_cast<Letter>(c - 'A');
  if (c >= 'a' && c <= 'z') return static_cast<Letter>(c - 'a');
  if (c == '?') return kBlank;
  return std::nullopt;
}

}  // namespace scrabble

#endif  // SCRABBLE_TILE_H_
