#include "codec.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace scrabble::server {
namespace {

using Json = nlohmann::json;

constexpr char ToLower(char c) { return static_cast<char>(c - 'A' + 'a'); }

// The board-string character for a tile: uppercase, or lowercase for a blank.
char TileChar(Letter letter, bool is_blank) {
  const char c = LetterToChar(letter);
  return is_blank ? ToLower(c) : c;
}

std::string_view DirectionName(Direction direction) {
  return direction == Direction::kAcross ? "across" : "down";
}

std::string_view MoveTypeName(MoveType type) {
  switch (type) {
    case MoveType::kPass: return "pass";
    case MoveType::kPlace: return "place";
    case MoveType::kExchange: return "exchange";
  }
  return "pass";
}

// Premium code per square, as in the engine's layout table: T/D word,
// t/d letter, '*' the centre, '.' plain.
char PremiumCode(int row, int col) {
  if (row == kCenter && col == kCenter) return '*';
  switch (PremiumAt(row, col)) {
    case Premium::kNone: return '.';
    case Premium::kDoubleLetter: return 'd';
    case Premium::kTripleLetter: return 't';
    case Premium::kDoubleWord: return 'D';
    case Premium::kTripleWord: return 'T';
  }
  return '.';
}

// {"A": per_letter[0], ..., "Z": ..., "?": per_letter[kBlank]}.
template <typename Array>
Json PerLetterJson(const Array& per_letter) {
  Json out = Json::object();
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    out[std::string(1, LetterToChar(letter))] = per_letter[letter];
  }
  return out;
}

}  // namespace

Board BoardFromJson(const Json& value) {
  if (!value.is_array() || value.size() != kBoardSize) {
    throw ApiError(400, "bad_board", "board must be an array of 15 strings");
  }
  Board board;
  for (int row = 0; row < kBoardSize; ++row) {
    const Json& line = value[static_cast<std::size_t>(row)];
    const std::string row_label = "board row " + std::to_string(row + 1);
    if (!line.is_string()) {
      throw ApiError(400, "bad_board", row_label + " is not a string");
    }
    const std::string& text = line.get_ref<const std::string&>();
    if (text.size() != kBoardSize) {
      throw ApiError(400, "bad_board",
                     row_label + " has " + std::to_string(text.size()) +
                         " characters, expected 15");
    }
    for (int col = 0; col < kBoardSize; ++col) {
      const char c = text[static_cast<std::size_t>(col)];
      if (c == '.') continue;
      // CharToLetter() would accept '?', but a blank on the board must be
      // designated, so only letters are allowed here.
      const std::optional<Letter> letter = CharToLetter(c);
      if (c == '?' || !letter) {
        throw ApiError(400, "bad_board",
                       row_label + " column " +
                           std::string(1, static_cast<char>('A' + col)) +
                           " has invalid character '" + std::string(1, c) +
                           "'");
      }
      board.Place(row, col, *letter, /*is_blank=*/c >= 'a' && c <= 'z');
    }
  }
  return board;
}

Json BoardToJson(const Board& board) {
  Json rows = Json::array();
  for (int row = 0; row < kBoardSize; ++row) {
    std::string text(kBoardSize, '.');
    for (int col = 0; col < kBoardSize; ++col) {
      const Square& square = board.At(row, col);
      if (!square.IsEmpty()) {
        text[static_cast<std::size_t>(col)] =
            TileChar(square.letter, square.is_blank);
      }
    }
    rows.push_back(std::move(text));
  }
  return rows;
}

Rack RackFromJson(const Json& value) {
  if (!value.is_string()) {
    throw ApiError(400, "bad_rack",
                   "rack must be a string such as \"AEINST?\"");
  }
  const std::optional<Rack> rack =
      Rack::FromString(value.get_ref<const std::string&>());
  if (!rack) {
    throw ApiError(400, "bad_rack",
                   "rack must hold at most 7 tiles: letters, or '?' for a "
                   "blank");
  }
  return *rack;
}

Move MoveFromNotation(std::string_view notation) {
  const std::optional<Move> move = Move::Parse(notation);
  if (!move) {
    throw ApiError(400, "bad_notation",
                   "cannot parse \"" + std::string(notation) +
                       "\"; expected e.g. \"8D RETAINS\", \"H8 QU(I)T\" "
                       "or \"-ABC\"");
  }
  return *move;
}

Rack RackForMove(const Move& move) {
  Rack rack;
  for (int i = 0; i < move.num_tiles; ++i) {
    const MoveTile tile = move.tiles[static_cast<std::size_t>(i)];
    if (move.type == MoveType::kPlace) {
      if (tile.is_play_through()) continue;
      rack.Add(tile.is_blank() ? kBlank : tile.letter());
    } else {
      rack.Add(tile.letter());
    }
  }
  return rack;
}

std::string_view MoveErrorCode(MoveError error) {
  switch (error) {
    case MoveError::kOk: return "ok";
    case MoveError::kEmptyMove: return "empty_move";
    case MoveError::kOutOfBounds: return "out_of_bounds";
    case MoveError::kBadTile: return "bad_tile";
    case MoveError::kSquareOccupied: return "square_occupied";
    case MoveError::kPlayThroughEmpty: return "play_through_empty";
    case MoveError::kPlayThroughMismatch: return "play_through_mismatch";
    case MoveError::kIncompleteWord: return "incomplete_word";
    case MoveError::kNoRackTiles: return "no_rack_tiles";
    case MoveError::kTooManyTiles: return "too_many_tiles";
    case MoveError::kNotInRack: return "not_in_rack";
    case MoveError::kWordTooShort: return "word_too_short";
    case MoveError::kFirstMoveOffCenter: return "first_move_off_center";
    case MoveError::kNotConnected: return "not_connected";
    case MoveError::kInvalidWord: return "invalid_word";
    case MoveError::kInvalidCrossWord: return "invalid_cross_word";
  }
  return "unknown";
}

Json MoveToJson(const Move& move, const Rack* rack) {
  const int tiles_used = move.TilesUsed();
  Json out = {
      {"notation", move.ToString()},
      {"type", MoveTypeName(move.type)},
      {"score", move.score},
      {"tilesUsed", tiles_used},
      {"bingo", move.type == MoveType::kPlace && tiles_used == kRackSize},
  };
  Json tiles = Json::array();
  std::string word;
  if (move.type == MoveType::kPlace) {
    const int dr = move.direction == Direction::kDown ? 1 : 0;
    const int dc = 1 - dr;
    out["row"] = move.row;
    out["col"] = move.col;
    out["direction"] = DirectionName(move.direction);
    for (int i = 0; i < move.num_tiles; ++i) {
      const MoveTile tile = move.tiles[static_cast<std::size_t>(i)];
      const char c = TileChar(tile.letter(), tile.is_blank());
      word += c;
      tiles.push_back({
          {"row", move.row + i * dr},
          {"col", move.col + i * dc},
          {"letter", std::string(1, c)},
          {"blank", tile.is_blank()},
          {"playThrough", tile.is_play_through()},
      });
    }
  } else if (move.type == MoveType::kExchange) {
    for (int i = 0; i < move.num_tiles; ++i) {
      word += LetterToChar(move.tiles[static_cast<std::size_t>(i)].letter());
    }
  }
  out["word"] = word;
  out["tiles"] = std::move(tiles);
  if (rack != nullptr) out["leave"] = LeaveAfter(*rack, move).ToString();
  return out;
}

Unseen ComputeUnseen(const Board& board, const Rack& rack) {
  Unseen unseen;
  std::array<int, kAlphabetSize> used{};
  for (int row = 0; row < kBoardSize; ++row) {
    for (int col = 0; col < kBoardSize; ++col) {
      const Square& square = board.At(row, col);
      if (square.IsEmpty()) continue;
      ++used[square.is_blank ? kBlank : square.letter];
    }
  }
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    const int remaining =
        kTileCounts[letter] - used[letter] - rack.Count(letter);
    if (remaining < 0) unseen.overused += LetterToChar(letter);
    unseen.counts[letter] = remaining < 0 ? 0 : remaining;
    unseen.total += unseen.counts[letter];
  }
  return unseen;
}

Json PositionSummaryToJson(const Board& board, const Rack& rack) {
  const Unseen unseen = ComputeUnseen(board, rack);
  const int bag = unseen.total - kRackSize;
  return {
      {"unseen", PerLetterJson(unseen.counts)},
      {"unseenCount", unseen.total},
      {"bagCount", bag < 0 ? 0 : bag},
      {"tilesOnBoard", board.TileCount()},
      {"overused", unseen.overused},
  };
}

Json RulesToJson() {
  Json premiums = Json::array();
  for (int row = 0; row < kBoardSize; ++row) {
    std::string text(kBoardSize, '.');
    for (int col = 0; col < kBoardSize; ++col) {
      text[static_cast<std::size_t>(col)] = PremiumCode(row, col);
    }
    premiums.push_back(std::move(text));
  }
  return {
      {"boardSize", kBoardSize},
      {"rackSize", kRackSize},
      {"bingoBonus", kBingoBonus},
      {"premiums", std::move(premiums)},
      {"tileValues", PerLetterJson(kTileValues)},
      {"tileCounts", PerLetterJson(kTileCounts)},
  };
}

}  // namespace scrabble::server
