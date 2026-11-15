#include "scrabble/cross_checks.h"

#include <bit>
#include <cassert>

namespace scrabble {
namespace {

struct Step {
  int dr;
  int dc;
};

// Unit step along the line a move in `direction` occupies.
constexpr Step AlongLine(Direction direction) {
  return direction == Direction::kAcross ? Step{0, 1} : Step{1, 0};
}

bool Occupied(const Board& board, int row, int col) {
  return InBounds(row, col) && !board.At(row, col).IsEmpty();
}

int FaceValue(const Square& square) {
  return square.is_blank ? 0 : kTileValues[square.letter];
}

}  // namespace

CrossCheck CrossChecks::Compute(const Board& board, const Gaddag& gaddag,
                                Direction direction, int row, int col) {
  if (!board.At(row, col).IsEmpty()) return kOccupiedCrossCheck;

  // The cross-word runs perpendicular to the move; `offset` is the signed
  // distance from (row, col) along it.
  const Step step = AlongLine(Perpendicular(direction));
  const auto occupied = [&](int offset) {
    return Occupied(board, row + offset * step.dr, col + offset * step.dc);
  };
  const auto at = [&](int offset) -> const Square& {
    return board.At(row + offset * step.dr, col + offset * step.dc);
  };
  int before = 0;
  while (occupied(-(before + 1))) ++before;
  int after = 0;
  while (occupied(after + 1)) ++after;
  if (before == 0 && after == 0) return CrossCheck{};

  CrossCheck check{.allowed = LetterSet(), .score = 0, .has_cross_word = true};
  int score = 0;
  for (int i = -before; i <= after; ++i) {
    if (i != 0) score += FaceValue(at(i));
  }
  check.score = static_cast<std::int16_t>(score);

  Gaddag::Node node = gaddag.Root();
  if (before > 0) {
    // Path REV(prefix) ^ L suffix: walk the prefix nearest-first, cross the
    // separator, then try each candidate L against the fixed suffix.
    for (int i = 1; i <= before && node != Gaddag::kInvalidNode; ++i) {
      node = gaddag.Child(node, at(-i).letter);
    }
    if (node != Gaddag::kInvalidNode) {
      node = gaddag.Child(node, kGaddagSeparator);
    }
    if (node == Gaddag::kInvalidNode) return check;
    const std::uint32_t candidates = gaddag.ChildLetters(node).bits();
    for (std::uint32_t bits = candidates; bits != 0; bits &= bits - 1) {
      const auto letter = static_cast<Letter>(std::countr_zero(bits));
      Gaddag::Node n = gaddag.Child(node, letter);
      for (int i = 1; i <= after && n != Gaddag::kInvalidNode; ++i) {
        n = gaddag.Child(n, at(i).letter);
      }
      if (n != Gaddag::kInvalidNode && gaddag.IsTerminal(n)) {
        check.allowed.Add(letter);
      }
    }
  } else {
    // No prefix: the word is L + suffix, whose full reversal REV(suffix) L
    // needs no separator. One walk, then a terminal test per child.
    for (int i = after; i >= 1 && node != Gaddag::kInvalidNode; --i) {
      node = gaddag.Child(node, at(i).letter);
    }
    if (node == Gaddag::kInvalidNode) return check;
    const std::uint32_t candidates = gaddag.ChildLetters(node).bits();
    for (std::uint32_t bits = candidates; bits != 0; bits &= bits - 1) {
      const auto letter = static_cast<Letter>(std::countr_zero(bits));
      if (gaddag.IsTerminal(gaddag.Child(node, letter))) {
        check.allowed.Add(letter);
      }
    }
  }
  return check;
}

CrossChecks CrossChecks::ComputeAll(const Board& board, const Gaddag& gaddag) {
  CrossChecks checks;
  for (const Direction direction : {Direction::kAcross, Direction::kDown}) {
    for (int row = 0; row < kBoardSize; ++row) {
      for (int col = 0; col < kBoardSize; ++col) {
        checks.checks_[static_cast<std::size_t>(direction)]
                      [SquareIndex(row, col)] =
            Compute(board, gaddag, direction, row, col);
      }
    }
  }
  return checks;
}

void CrossChecks::Set(const Board& board, const Gaddag& gaddag,
                      Direction direction, int row, int col,
                      CrossCheckUndo* undo) {
  CrossCheck& entry = checks_[static_cast<std::size_t>(direction)]
                             [SquareIndex(row, col)];
  if (undo != nullptr) {
    assert(undo->count < CrossCheckUndo::kMaxEntries);
    undo->entries[static_cast<std::size_t>(undo->count++)] = {
        .square = static_cast<std::uint8_t>(SquareIndex(row, col)),
        .direction = direction,
        .value = entry};
  }
  entry = Compute(board, gaddag, direction, row, col);
}

void CrossChecks::Update(const Board& board, const Gaddag& gaddag,
                         const Move& move, CrossCheckUndo* undo) {
  assert(move.type == MoveType::kPlace);
  const Direction main = move.direction;
  const Direction cross = Perpendicular(main);
  const Step along = AlongLine(main);
  const Step perp = AlongLine(cross);

  // Walks from (row, col) in steps of (dr, dc) past occupied squares and
  // refreshes the first empty square reached, if it is on the board.
  const auto refresh_cap = [&](Direction direction, int row, int col, int dr,
                               int dc) {
    while (Occupied(board, row, col)) {
      row += dr;
      col += dc;
    }
    if (InBounds(row, col)) Set(board, gaddag, direction, row, col, undo);
  };

  const int row0 = move.row;
  const int col0 = move.col;
  for (int i = 0; i < move.num_tiles; ++i) {
    if (move.tiles[static_cast<std::size_t>(i)].is_play_through()) continue;
    const int row = row0 + i * along.dr;
    const int col = col0 + i * along.dc;
    Set(board, gaddag, Direction::kAcross, row, col, undo);
    Set(board, gaddag, Direction::kDown, row, col, undo);
    // Squares capping the perpendicular run see a changed `main`-direction
    // cross-word (a move along `main` there crosses this run).
    refresh_cap(main, row - perp.dr, col - perp.dc, -perp.dr, -perp.dc);
    refresh_cap(main, row + perp.dr, col + perp.dc, perp.dr, perp.dc);
  }
  // Squares capping the main line see a changed cross-word for moves along
  // the other direction.
  const int last = move.num_tiles - 1;
  refresh_cap(cross, row0 - along.dr, col0 - along.dc, -along.dr, -along.dc);
  refresh_cap(cross, row0 + (last + 1) * along.dr,
              col0 + (last + 1) * along.dc, along.dr, along.dc);
}

void CrossChecks::Restore(const CrossCheckUndo& undo) {
  for (int i = undo.count - 1; i >= 0; --i) {
    const CrossCheckUndo::Entry& entry =
        undo.entries[static_cast<std::size_t>(i)];
    checks_[static_cast<std::size_t>(entry.direction)][entry.square] =
        entry.value;
  }
}

}  // namespace scrabble
