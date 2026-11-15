#include "test_util.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

#include "scrabble/lexicon.h"
#include "scrabble/move_validation.h"

namespace scrabble::testing {
namespace {

// True if `word[0, length)` begins some lexicon word: REV(prefix) is
// terminal (the prefix is itself a word) or continues with a separator.
bool IsWordPrefix(const Gaddag& gaddag, const Letter* word, int length) {
  Gaddag::Node node = gaddag.Root();
  for (int i = length - 1; i >= 0; --i) {
    node = gaddag.Child(node, word[i]);
    if (node == Gaddag::kInvalidNode) return false;
  }
  return gaddag.IsTerminal(node) || gaddag.HasSeparator(node);
}

class ReferenceGenerator {
 public:
  ReferenceGenerator(const Board& board, const Gaddag& gaddag,
                     const Rack& rack, bool prune)
      : board_(board), gaddag_(gaddag), rack_(rack), prune_(prune) {}

  std::vector<Move> Run() {
    for (const Direction direction : {Direction::kAcross, Direction::kDown}) {
      dr_ = direction == Direction::kDown ? 1 : 0;
      dc_ = 1 - dr_;
      for (int row = 0; row < kBoardSize; ++row) {
        for (int col = 0; col < kBoardSize; ++col) {
          current_ = Move::Place(row, col, direction);
          Extend(row, col);
        }
      }
    }
    return out_;
  }

 private:
  void Extend(int row, int col) {
    if (!InBounds(row, col)) return;
    const Square& square = board_.At(row, col);
    if (!square.IsEmpty()) {
      Try(row, col, MoveTile::PlayThrough(square.letter, square.is_blank));
      return;
    }
    for (Letter letter = 0; letter < kNumLetters; ++letter) {
      if (!rack_.Contains(letter)) continue;
      rack_.Remove(letter);
      Try(row, col, MoveTile::Placed(letter));
      rack_.Add(letter);
    }
    if (rack_.Contains(kBlank)) {
      rack_.Remove(kBlank);
      for (Letter letter = 0; letter < kNumLetters; ++letter) {
        Try(row, col, MoveTile::Placed(letter, /*is_blank=*/true));
      }
      rack_.Add(kBlank);
    }
  }

  void Try(int row, int col, MoveTile tile) {
    const std::size_t depth = current_.num_tiles;
    word_[depth] = tile.letter();
    current_.AddTile(tile);
    if (!prune_ ||
        IsWordPrefix(gaddag_, word_.data(), static_cast<int>(depth + 1))) {
      Move candidate = current_;
      if (candidate.TilesUsed() > 0 &&
          ValidateMove(board_, gaddag_, original_rack_, candidate) ==
              MoveError::kOk &&
          !IsDuplicateSingle(candidate)) {
        out_.push_back(candidate);
      }
      Extend(row + dr_, col + dc_);
    }
    current_.tiles[--current_.num_tiles] = MoveTile{};
  }

  // The generator reports a lone tile with a horizontal neighbour only as
  // an across move.
  bool IsDuplicateSingle(const Move& move) const {
    if (move.direction != Direction::kDown || move.TilesUsed() != 1) {
      return false;
    }
    for (int i = 0; i < move.num_tiles; ++i) {
      if (move.tiles[static_cast<std::size_t>(i)].is_play_through()) continue;
      const int row = move.row + i;
      const int col = move.col;
      return (col > 0 && !board_.At(row, col - 1).IsEmpty()) ||
             (col + 1 < kBoardSize && !board_.At(row, col + 1).IsEmpty());
    }
    return false;
  }

  const Board& board_;
  const Gaddag& gaddag_;
  Rack rack_;
  const Rack original_rack_ = rack_;
  bool prune_;
  int dr_ = 0;
  int dc_ = 0;
  Move current_;
  std::array<Letter, kBoardSize> word_{};
  std::vector<Move> out_;
};

std::string Describe(const Move& move) {
  return move.ToString() + " =" + std::to_string(move.score);
}

}  // namespace

Move ParseOrDie(const std::string& text) {
  const std::optional<Move> move = Move::Parse(text);
  if (!move) throw std::invalid_argument("bad move notation: " + text);
  return *move;
}

Rack RackOrDie(const std::string& text) {
  const std::optional<Rack> rack = Rack::FromString(text);
  if (!rack) throw std::invalid_argument("bad rack: " + text);
  return *rack;
}

Board MakeBoard(const std::vector<std::string>& moves) {
  Board board;
  for (const std::string& text : moves) PlaceTiles(board, ParseOrDie(text));
  return board;
}

const Gaddag* EnableGaddag() {
  static const std::unique_ptr<Gaddag> gaddag = [] {
    const std::filesystem::path path =
        std::filesystem::path(SCRABBLE_LEXICA_DIR) / "enable1.txt";
    std::unique_ptr<Gaddag> loaded;
    if (std::filesystem::exists(path)) {
      loaded = std::make_unique<Gaddag>(LoadOrBuildGaddag(path));
    }
    return loaded;
  }();
  return gaddag.get();
}

std::vector<Move> ReferenceMoves(const Board& board, const Gaddag& gaddag,
                                 const Rack& rack, bool prune) {
  return ReferenceGenerator(board, gaddag, rack, prune).Run();
}

::testing::AssertionResult SameMoves(const std::vector<Move>& generated,
                                     const std::vector<Move>& expected) {
  std::multiset<std::string> got;
  for (const Move& move : generated) got.insert(Describe(move));
  std::set<std::string> want;
  for (const Move& move : expected) want.insert(Describe(move));

  std::vector<std::string> duplicates;
  for (auto it = got.begin(); it != got.end(); it = got.upper_bound(*it)) {
    if (got.count(*it) > 1) duplicates.push_back(*it);
  }
  const std::set<std::string> got_set(got.begin(), got.end());
  std::vector<std::string> missing;
  std::vector<std::string> extra;
  std::set_difference(want.begin(), want.end(), got_set.begin(),
                      got_set.end(), std::back_inserter(missing));
  std::set_difference(got_set.begin(), got_set.end(), want.begin(),
                      want.end(), std::back_inserter(extra));
  if (duplicates.empty() && missing.empty() && extra.empty()) {
    return ::testing::AssertionSuccess() << got.size() << " moves";
  }
  std::ostringstream message;
  const auto list = [&message](const char* label,
                               const std::vector<std::string>& items) {
    if (items.empty()) return;
    message << "\n  " << label << " (" << items.size() << "):";
    for (std::size_t i = 0; i < items.size() && i < 10; ++i) {
      message << " [" << items[i] << "]";
    }
  };
  message << "generated " << got.size() << ", expected " << want.size();
  list("duplicated", duplicates);
  list("missing", missing);
  list("unexpected", extra);
  return ::testing::AssertionFailure() << message.str();
}

void TakeTiles(Rack& rack, const Move& move) {
  for (int i = 0; i < move.num_tiles; ++i) {
    const MoveTile tile = move.tiles[static_cast<std::size_t>(i)];
    if (move.type == MoveType::kPlace && tile.is_play_through()) continue;
    rack.Remove(move.type == MoveType::kPlace && tile.is_blank()
                    ? kBlank
                    : tile.letter());
  }
}

}  // namespace scrabble::testing
