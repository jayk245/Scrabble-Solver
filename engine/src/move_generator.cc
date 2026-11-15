#include "scrabble/move_generator.h"

#include <algorithm>
#include <bit>
#include <cstddef>

#include "scrabble/scoring.h"

namespace scrabble {
namespace {

constexpr std::size_t Idx(int pos) { return static_cast<std::size_t>(pos); }

}  // namespace

void MoveGenerator::Generate(const Board& board,
                             const CrossChecks& cross_checks,
                             const Rack& rack, std::vector<Move>& out,
                             const MoveGenOptions& options) {
  out.clear();
  out_ = &out;
  rack_letters_ = 0;
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    rack_[letter] = static_cast<std::uint8_t>(rack.Count(letter));
    if (letter < kNumLetters && rack_[letter] != 0) {
      rack_letters_ |= 1u << letter;
    }
  }
  tiles_left_ = rack.Size();

  if (tiles_left_ > 0) {
    const bool board_empty = board.IsEmpty();
    const std::uint32_t playable =
        rack_[kBlank] != 0 ? LetterSet::kAllBits : rack_letters_;
    for (const Direction direction : {Direction::kAcross, Direction::kDown}) {
      direction_ = direction;
      for (int line = 0; line < kBoardSize; ++line) {
        LoadLine(board, cross_checks, direction, line, board_empty);
        for (int pos = 0; pos < kBoardSize; ++pos) {
          // An anchor no rack tile can legally occupy starts no move.
          if (!is_anchor_[Idx(pos)] || (allowed_[Idx(pos)] & playable) == 0) {
            continue;
          }
          anchor_ = pos;
          right_of_anchor_open_ =
              pos + 1 == kBoardSize || letter_[Idx(pos + 1)] == kEmptySquare;
          ExtendLeft(pos, gaddag_->Root(), Partial{});
        }
      }
    }
  }

  if (options.include_exchanges) {
    Move exchange;
    exchange.type = MoveType::kExchange;
    AddExchanges(0, exchange);
  }
  if (options.include_pass) out.push_back(Move::Pass());
  out_ = nullptr;
}

void MoveGenerator::LoadLine(const Board& board,
                             const CrossChecks& cross_checks,
                             Direction direction, int line, bool board_empty) {
  line_ = line;
  const Direction other = Perpendicular(direction);
  for (int pos = 0; pos < kBoardSize; ++pos) {
    const int row = direction == Direction::kAcross ? line : pos;
    const int col = direction == Direction::kAcross ? pos : line;
    const std::size_t square = SquareIndex(row, col);
    const std::size_t p = Idx(pos);
    const Premium premium = kPremiumLayout[square];
    letter_mult_[p] = static_cast<std::uint8_t>(LetterMultiplier(premium));
    word_mult_[p] = static_cast<std::uint8_t>(WordMultiplier(premium));

    const Square& sq = board.At(row, col);
    if (!sq.IsEmpty()) {
      letter_[p] = sq.letter;
      value_[p] =
          static_cast<std::uint8_t>(sq.is_blank ? 0 : kTileValues[sq.letter]);
      tiles_[p] = MoveTile::PlayThrough(sq.letter, sq.is_blank);
      allowed_[p] = 0;
      cross_score_[p] = -1;
      is_anchor_[p] = false;
      continue;
    }
    const CrossCheck& check = cross_checks.For(direction, square);
    letter_[p] = kEmptySquare;
    value_[p] = 0;
    allowed_[p] = check.allowed.bits();
    cross_score_[p] = check.has_cross_word ? check.score : std::int16_t{-1};
    // A tile on either axis makes this square adjacent to the board, which
    // is exactly when one of the two cross-checks sees a neighbour.
    is_anchor_[p] = board_empty
                        ? (row == kCenter && col == kCenter)
                        : (check.has_cross_word ||
                           cross_checks.For(other, square).has_cross_word);
  }
}

template <typename Next>
void MoveGenerator::PlaceAt(int pos, Gaddag::Node node, Partial partial,
                            Next&& next) {
  const std::size_t p = Idx(pos);
  const bool has_blank = rack_[kBlank] != 0;
  std::uint32_t candidates = allowed_[p] & gaddag_->ChildLetters(node).bits();
  if (!has_blank) candidates &= rack_letters_;
  if (candidates == 0) return;

  const int letter_mult = letter_mult_[p];
  const int word_mult = word_mult_[p];
  const int cross_score = cross_score_[p];
  --tiles_left_;
  partial.word_mult *= word_mult;
  ++partial.placed;
  for (std::uint32_t bits = candidates; bits != 0; bits &= bits - 1) {
    const auto letter = static_cast<Letter>(std::countr_zero(bits));
    const Gaddag::Node child = gaddag_->Child(node, letter);
    if (rack_[letter] != 0) {
      const int points = kTileValues[letter] * letter_mult;
      if (--rack_[letter] == 0) rack_letters_ &= ~(1u << letter);
      tiles_[p] = MoveTile::Placed(letter);
      Partial with = partial;
      with.main_sum += points;
      if (cross_score >= 0) {
        with.cross_total += (cross_score + points) * word_mult;
      }
      next(child, with);
      if (rack_[letter]++ == 0) rack_letters_ |= 1u << letter;
    }
    if (has_blank) {
      --rack_[kBlank];
      tiles_[p] = MoveTile::Placed(letter, /*is_blank=*/true);
      Partial with = partial;
      if (cross_score >= 0) with.cross_total += cross_score * word_mult;
      next(child, with);
      ++rack_[kBlank];
    }
  }
  ++tiles_left_;
}

void MoveGenerator::ExtendLeft(int pos, Gaddag::Node node, Partial partial) {
  const Letter on_board = letter_[Idx(pos)];
  if (on_board != kEmptySquare) {
    const Gaddag::Node child = gaddag_->Child(node, on_board);
    if (child == Gaddag::kInvalidNode) return;
    partial.main_sum += value_[Idx(pos)];
    AfterLeft(pos, child, partial);
    return;
  }
  PlaceAt(pos, node, partial, [this, pos](Gaddag::Node child, Partial with) {
    AfterLeft(pos, child, with);
  });
}

void MoveGenerator::AfterLeft(int pos, Gaddag::Node node, Partial partial) {
  const bool left_open = pos == 0 || letter_[Idx(pos - 1)] == kEmptySquare;
  if (left_open) {
    // Path so far is REV(word) with no separator: a complete word iff the
    // node is terminal and nothing adjoins either end.
    if (right_of_anchor_open_ && pos < anchor_ && gaddag_->IsTerminal(node)) {
      Record(pos, anchor_, partial);
    }
    const int right = anchor_ + 1;
    if (right < kBoardSize &&
        (letter_[Idx(right)] != kEmptySquare || tiles_left_ > 0)) {
      const Gaddag::Node sep = gaddag_->Child(node, kGaddagSeparator);
      if (sep != Gaddag::kInvalidNode) ExtendRight(right, sep, pos, partial);
    }
  }
  if (pos == 0) return;
  // A tile to the left must be absorbed. An empty square may take a tile
  // unless it is an anchor, in which case that anchor generates the move.
  if (!left_open ||
      (tiles_left_ > 0 && !is_anchor_[Idx(pos - 1)])) {
    ExtendLeft(pos - 1, node, partial);
  }
}

void MoveGenerator::ExtendRight(int pos, Gaddag::Node node, int left,
                                Partial partial) {
  const Letter on_board = letter_[Idx(pos)];
  if (on_board != kEmptySquare) {
    const Gaddag::Node child = gaddag_->Child(node, on_board);
    if (child == Gaddag::kInvalidNode) return;
    partial.main_sum += value_[Idx(pos)];
    AfterRight(pos, child, left, partial);
    return;
  }
  PlaceAt(pos, node, partial,
          [this, pos, left](Gaddag::Node child, Partial with) {
            AfterRight(pos, child, left, with);
          });
}

void MoveGenerator::AfterRight(int pos, Gaddag::Node node, int left,
                               Partial partial) {
  const bool at_end =
      pos + 1 == kBoardSize || letter_[Idx(pos + 1)] == kEmptySquare;
  if (at_end && gaddag_->IsTerminal(node)) Record(left, pos, partial);
  if (pos + 1 < kBoardSize && (!at_end || tiles_left_ > 0)) {
    ExtendRight(pos + 1, node, left, partial);
  }
}

void MoveGenerator::Record(int left, int right, const Partial& partial) {
  // A lone tile with a horizontal neighbour is also found by the across
  // pass; keep only that copy (see the file comment).
  if (direction_ == Direction::kDown && partial.placed == 1 &&
      cross_score_[Idx(anchor_)] >= 0) {
    return;
  }
  Move& move = out_->emplace_back();
  move.type = MoveType::kPlace;
  move.direction = direction_;
  move.row = static_cast<std::uint8_t>(
      direction_ == Direction::kAcross ? line_ : left);
  move.col = static_cast<std::uint8_t>(
      direction_ == Direction::kAcross ? left : line_);
  move.num_tiles = static_cast<std::uint8_t>(right - left + 1);
  std::copy(tiles_.begin() + left, tiles_.begin() + right + 1,
            move.tiles.begin());
  move.score = partial.main_sum * partial.word_mult + partial.cross_total +
               (partial.placed == kRackSize ? kBingoBonus : 0);
}

void MoveGenerator::AddExchanges(Letter letter, Move& exchange) {
  if (letter == kAlphabetSize) {
    if (exchange.num_tiles > 0) out_->push_back(exchange);
    return;
  }
  AddExchanges(static_cast<Letter>(letter + 1), exchange);
  const int count = rack_[letter];
  for (int k = 0; k < count; ++k) {
    exchange.AddTile(MoveTile::Placed(letter));
    AddExchanges(static_cast<Letter>(letter + 1), exchange);
  }
  // Restore the "unused tiles are default" invariant Move::operator== needs.
  for (int k = 0; k < count; ++k) {
    exchange.tiles[--exchange.num_tiles] = MoveTile{};
  }
}

}  // namespace scrabble
