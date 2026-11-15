#include "scrabble/game.h"

#include <cassert>
#include <cstddef>

namespace scrabble {

std::string_view PlayErrorMessage(const PlayResult& result) {
  switch (result.error) {
    case PlayError::kOk: return "ok";
    case PlayError::kGameOver: return "the game is over";
    case PlayError::kIllegalMove: return MoveErrorMessage(result.move_error);
    case PlayError::kExchangeNotAllowed:
      return "exchange needs at least 7 tiles in the bag";
  }
  return "unknown error";
}

std::array<std::uint8_t, kAlphabetSize> UnseenTiles(const Board& board,
                                                    const Rack& own_rack) {
  std::array<int, kAlphabetSize> counts = kTileCounts;
  for (int row = 0; row < kBoardSize; ++row) {
    for (int col = 0; col < kBoardSize; ++col) {
      const Square& square = board.At(row, col);
      if (!square.IsEmpty()) {
        --counts[square.is_blank ? kBlank : square.letter];
      }
    }
  }
  std::array<std::uint8_t, kAlphabetSize> unseen{};
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    const int count = counts[letter] - own_rack.Count(letter);
    assert(count >= 0 && "board and rack exceed the tile distribution");
    unseen[letter] = static_cast<std::uint8_t>(count > 0 ? count : 0);
  }
  return unseen;
}

Game::Game(const Gaddag& gaddag, std::uint64_t seed, const GameConfig& config)
    : config_(config), position_(gaddag), rng_(seed) {
  Reset(seed);
}

Game::Game(const Gaddag& gaddag, const GameSetup& setup, std::uint64_t seed,
           const GameConfig& config)
    : config_(config),
      position_(gaddag, setup.board),
      racks_(setup.racks),
      bag_(setup.bag),
      rng_(seed) {}

void Game::Reset(std::uint64_t seed) {
  position_ = Position(position_.lexicon());
  rng_.Seed(seed);
  bag_ = Bag::Full();
  for (Rack& rack : racks_) {
    rack.Clear();
    bag_.DrawInto(rack, rng_);
  }
  scores_ = {};
  end_adjustments_ = {};
  to_move_ = 0;
  scoreless_turns_ = 0;
  went_out_ = -1;
  status_ = GameStatus::kInProgress;
  history_.clear();
}

PlayResult Game::Play(const Move& input) {
  if (IsOver()) return {PlayError::kGameOver};
  Rack& rack = racks_[Idx(to_move_)];
  Move move = input;
  const MoveError error = ValidateMove(board(), lexicon(), rack, move);
  if (error != MoveError::kOk) return {PlayError::kIllegalMove, error};
  if (move.type == MoveType::kExchange &&
      bag_.Size() < kMinBagForExchange) {
    return {PlayError::kExchangeNotAllowed};
  }
  // Nothing below can fail, so the state changes are all-or-nothing.

  // ValidateMove() scores placements only; never trust a caller's score
  // for a pass or exchange.
  if (move.type != MoveType::kPlace) move.score = 0;

  MoveRecord& record = history_.emplace_back();
  record.player = to_move_;
  record.rack_before = rack;
  record.bag_before = bag_.Size();
  rack = LeaveAfter(rack, move);
  if (move.type == MoveType::kPlace) position_.Apply(move);
  // A pass draws nothing (that only matters for short racks in a custom
  // GameSetup; in a real game racks are always full or the bag is empty).
  while (move.type != MoveType::kPass && rack.Size() < kRackSize &&
         !bag_.IsEmpty()) {
    const Letter letter = bag_.Draw(rng_);
    rack.Add(letter);
    record.drawn.Add(letter);
  }
  // Exchanged tiles go back only after the replacements are drawn, so a
  // player can never redraw their own tiles.
  if (move.type == MoveType::kExchange) {
    for (std::size_t i = 0; i < move.num_tiles; ++i) {
      bag_.Return(move.tiles[i].letter());
    }
  }

  int& score = scores_[Idx(to_move_)];
  score += move.score;
  record.move = move;
  record.score = move.score;
  record.cumulative = score;
  scoreless_turns_ = move.score == 0 ? scoreless_turns_ + 1 : 0;

  if (move.type == MoveType::kPlace && rack.IsEmpty()) {
    // Racks refill while the bag has tiles, so an empty rack implies an
    // empty bag.
    assert(bag_.IsEmpty());
    went_out_ = to_move_;
    Finish(GameStatus::kWentOut);
  } else if (scoreless_turns_ >= kScorelessTurnsToEnd) {
    Finish(GameStatus::kScorelessTurns);
  }
  to_move_ = 1 - to_move_;
  return {};
}

void Game::Finish(GameStatus status) {
  status_ = status;
  if (status == GameStatus::kWentOut) {
    const int opponent = 1 - went_out_;
    const int value = racks_[Idx(opponent)].Score();
    if (config_.end_rules == EndRules::kNaspa) {
      end_adjustments_[Idx(went_out_)] = 2 * value;
    } else {
      end_adjustments_[Idx(went_out_)] = value;
      end_adjustments_[Idx(opponent)] = -value;
    }
  } else {
    for (int player = 0; player < kNumPlayers; ++player) {
      end_adjustments_[Idx(player)] = -racks_[Idx(player)].Score();
    }
  }
  for (std::size_t player = 0; player < scores_.size(); ++player) {
    scores_[player] += end_adjustments_[player];
  }
}

PlayerView Game::View(int player) const {
  const int opponent = 1 - player;
  const Rack& opponent_rack = rack(opponent);
  PlayerView view;
  view.position = &position_;
  view.rack = rack(player);
  view.player = player;
  view.score = score(player);
  view.opponent_score = score(opponent);
  view.bag_size = bag_.Size();
  view.opponent_rack_size = opponent_rack.Size();
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    view.unseen[letter] = static_cast<std::uint8_t>(
        bag_.Count(letter) + opponent_rack.Count(letter));
  }
  view.unseen_size = view.bag_size + view.opponent_rack_size;
  view.scoreless_turns = scoreless_turns_;
  view.turn = turn();
  view.end_rules = config_.end_rules;
  return view;
}

}  // namespace scrabble
