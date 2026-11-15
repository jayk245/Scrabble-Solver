// Development CLI. For now it renders a board with a sample opening play to
// smoke-test the engine build; it will grow into a play/analyse front end.

#include <iostream>
#include <string_view>

#include "scrabble/board.h"
#include "scrabble/tile.h"

int main() {
  using scrabble::Board;
  using scrabble::CharToLetter;

  Board board;
  constexpr std::string_view kOpening = "HELLO";
  for (std::size_t i = 0; i < kOpening.size(); ++i) {
    board.Place(scrabble::kCenter, scrabble::kCenter + static_cast<int>(i),
                *CharToLetter(kOpening[i]));
  }

  std::cout << board.ToString();
  std::cout << "\nTiles on board: " << board.TileCount() << '\n';
  return 0;
}
