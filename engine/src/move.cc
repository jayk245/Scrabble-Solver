#include "scrabble/move.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace scrabble {
namespace {

constexpr bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

constexpr bool IsDigit(char c) { return c >= '0' && c <= '9'; }
constexpr bool IsUpper(char c) { return c >= 'A' && c <= 'Z'; }
constexpr bool IsLower(char c) { return c >= 'a' && c <= 'z'; }

constexpr char ToLower(char c) {
  return IsUpper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ToLower(a[i]) != ToLower(b[i])) return false;
  }
  return true;
}

// Splits on runs of whitespace. Returns the number of tokens found, which
// may exceed `tokens.size()`; callers treat that as malformed input.
template <std::size_t N>
std::size_t Tokenize(std::string_view text,
                     std::array<std::string_view, N>& tokens) {
  std::size_t count = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && IsSpace(text[i])) ++i;
    if (i == text.size()) break;
    const std::size_t start = i;
    while (i < text.size() && !IsSpace(text[i])) ++i;
    if (count < N) tokens[count] = text.substr(start, i - start);
    ++count;
  }
  return count;
}

// Parses "1".."15" into a zero-based row.
std::optional<int> ParseRow(std::string_view digits) {
  if (digits.empty() || digits.size() > 2 || digits[0] == '0') {
    return std::nullopt;
  }
  int value = 0;
  for (char c : digits) {
    if (!IsDigit(c)) return std::nullopt;
    value = value * 10 + (c - '0');
  }
  if (value < 1 || value > kBoardSize) return std::nullopt;
  return value - 1;
}

// Parses "A".."O" (either case) into a zero-based column.
std::optional<int> ParseCol(char c) {
  const char lower = ToLower(c);
  if (lower < 'a' || lower >= 'a' + kBoardSize) return std::nullopt;
  return lower - 'a';
}

std::optional<Move> ParsePlacement(std::string_view coord,
                                   std::string_view word) {
  if (coord.size() < 2) return std::nullopt;

  // Number first means across ("8H"); letter first means down ("H8").
  const bool across = IsDigit(coord[0]);
  const std::optional<int> row =
      across ? ParseRow(coord.substr(0, coord.size() - 1))
             : ParseRow(coord.substr(1));
  const std::optional<int> col =
      ParseCol(across ? coord.back() : coord.front());
  if (!row || !col) return std::nullopt;

  Move move = Move::Place(*row, *col,
                          across ? Direction::kAcross : Direction::kDown);
  bool in_group = false;
  bool group_empty = false;
  for (char c : word) {
    if (c == '(') {
      if (in_group) return std::nullopt;
      in_group = true;
      group_empty = true;
      continue;
    }
    if (c == ')') {
      if (!in_group || group_empty) return std::nullopt;
      in_group = false;
      continue;
    }
    if (move.num_tiles == kMaxMoveTiles) return std::nullopt;
    if (c == '.') {
      if (in_group) return std::nullopt;
      move.AddTile(MoveTile::PlayThrough(MoveTile::kUnknownLetter));
      continue;
    }
    // '?' is rejected here: a blank on the board must have a designation.
    if (!IsUpper(c) && !IsLower(c)) return std::nullopt;
    const auto letter = static_cast<Letter>(ToLower(c) - 'a');
    const bool is_blank = IsLower(c);
    move.AddTile(in_group ? MoveTile::PlayThrough(letter, is_blank)
                          : MoveTile::Placed(letter, is_blank));
    group_empty = false;
  }
  if (in_group) return std::nullopt;

  const int start = across ? *col : *row;
  if (start + move.num_tiles > kBoardSize) return std::nullopt;
  const int used = move.TilesUsed();
  if (used < 1 || used > kRackSize) return std::nullopt;
  return move;
}

std::optional<Move> ParseExchange(std::string_view letters) {
  const std::optional<Rack> rack = Rack::FromString(letters);
  if (!rack || rack->IsEmpty()) return std::nullopt;
  // Preserve the caller's order rather than canonicalising, so that
  // Parse(m.ToString()) == m for any exchange, not just canonical ones.
  Move move;
  move.type = MoveType::kExchange;
  for (char c : letters) move.AddTile(MoveTile::Placed(*CharToLetter(c)));
  return move;
}

char TileChar(MoveTile tile) {
  if (tile.letter() == MoveTile::kUnknownLetter) return '.';
  const char c = LetterToChar(tile.letter());
  return tile.is_blank() ? ToLower(c) : c;
}

}  // namespace

Move Move::Exchange(const Rack& rack) {
  Move move;
  move.type = MoveType::kExchange;
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    for (int n = rack.Count(letter); n > 0; --n) {
      move.AddTile(MoveTile::Placed(letter));
    }
  }
  return move;
}

int Move::TilesUsed() const {
  switch (type) {
    case MoveType::kPass:
      return 0;
    case MoveType::kExchange:
      return num_tiles;
    case MoveType::kPlace:
      break;
  }
  int used = 0;
  for (std::size_t i = 0; i < num_tiles; ++i) {
    used += !tiles[i].is_play_through();
  }
  return used;
}

std::string Move::ToString() const {
  std::string out;
  switch (type) {
    case MoveType::kPass:
      return "-";
    case MoveType::kExchange:
      out += '-';
      for (std::size_t i = 0; i < num_tiles; ++i) {
        out += LetterToChar(tiles[i].letter());
      }
      return out;
    case MoveType::kPlace:
      break;
  }

  const std::string row_label = std::to_string(row + 1);
  const char col_label = static_cast<char>('A' + col);
  if (direction == Direction::kAcross) {
    out += row_label;
    out += col_label;
  } else {
    out += col_label;
    out += row_label;
  }
  out += ' ';

  bool in_group = false;
  for (std::size_t i = 0; i < num_tiles; ++i) {
    const MoveTile tile = tiles[i];
    // Unknown play-through letters render as bare '.', so only known ones
    // open a parenthesised group.
    const bool grouped = tile.is_play_through() &&
                         tile.letter() != MoveTile::kUnknownLetter;
    if (grouped != in_group) {
      out += grouped ? '(' : ')';
      in_group = grouped;
    }
    out += TileChar(tile);
  }
  if (in_group) out += ')';
  return out;
}

std::optional<Move> Move::Parse(std::string_view text) {
  std::array<std::string_view, 2> tokens;
  const std::size_t count = Tokenize(text, tokens);
  if (count == 0 || count > tokens.size()) return std::nullopt;

  if (count == 1) {
    const std::string_view token = tokens[0];
    if (token == "-" || EqualsIgnoreCase(token, "pass")) return Move::Pass();
    if (token.front() == '-') return ParseExchange(token.substr(1));
    return std::nullopt;
  }

  if (EqualsIgnoreCase(tokens[0], "exch")) return ParseExchange(tokens[1]);
  return ParsePlacement(tokens[0], tokens[1]);
}

}  // namespace scrabble
