#include "scrabble/board.h"

#include <cassert>
#include <string>

namespace scrabble {
namespace {

char PremiumSymbol(int row, int col) {
  if (row == kCenter && col == kCenter) return '*';
  switch (PremiumAt(row, col)) {
    case Premium::kNone: return '.';
    case Premium::kDoubleLetter: return '\'';
    case Premium::kTripleLetter: return '"';
    case Premium::kDoubleWord: return '-';
    case Premium::kTripleWord: return '=';
  }
  return '?';
}

}  // namespace

void Board::Place(int row, int col, Letter letter, bool is_blank) {
  assert(InBounds(row, col));
  assert(letter < kNumLetters);
  Square& square = squares_[SquareIndex(row, col)];
  assert(square.IsEmpty() && "Place() called on an occupied square");
  square.letter = letter;
  square.is_blank = is_blank;
  ++tile_count_;
}

void Board::Remove(int row, int col) {
  assert(InBounds(row, col));
  Square& square = squares_[SquareIndex(row, col)];
  assert(!square.IsEmpty() && "Remove() called on an empty square");
  square = Square{};
  --tile_count_;
}

std::string Board::ToString() const {
  std::string out;
  // Header row plus 15 board rows, each "NN " + 15 * 2 chars + newline.
  out.reserve(static_cast<std::size_t>((kBoardSize + 1) * (3 + 2 * kBoardSize + 1)));

  out += "   ";
  for (int col = 0; col < kBoardSize; ++col) {
    out += static_cast<char>('A' + col);
    out += ' ';
  }
  out += '\n';

  for (int row = 0; row < kBoardSize; ++row) {
    const int label = row + 1;
    if (label < 10) out += ' ';
    out += std::to_string(label);
    out += ' ';
    for (int col = 0; col < kBoardSize; ++col) {
      const Square& square = At(row, col);
      if (square.IsEmpty()) {
        out += PremiumSymbol(row, col);
      } else {
        const char c = LetterToChar(square.letter);
        out += square.is_blank ? static_cast<char>(c - 'A' + 'a') : c;
      }
      out += ' ';
    }
    out += '\n';
  }
  return out;
}

}  // namespace scrabble
