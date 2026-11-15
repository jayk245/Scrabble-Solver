#include "scrabble/gcg.h"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace scrabble {
namespace {

void CheckNickname(const std::string& nickname) {
  if (nickname.empty() ||
      nickname.find_first_of(" \t\r\n") != std::string::npos) {
    throw std::invalid_argument("GCG nickname must be one non-empty word: '" +
                                nickname + "'");
  }
}

// GCG writes tiles already on the board as '.', not in parentheses.
std::string MoveText(const Move& move) {
  if (move.type != MoveType::kPlace) return move.ToString();
  Move masked = move;
  for (std::size_t i = 0; i < masked.num_tiles; ++i) {
    if (masked.tiles[i].is_play_through()) {
      masked.tiles[i] = MoveTile::PlayThrough(MoveTile::kUnknownLetter);
    }
  }
  return masked.ToString();
}

std::string Signed(int value) {
  return (value >= 0 ? "+" : "") + std::to_string(value);
}

}  // namespace

std::string ToGcg(const Game& game, const GcgInfo& info) {
  for (const GcgPlayer& player : info.players) CheckNickname(player.nickname);
  const auto nick = [&info](int player) -> const std::string& {
    return info.players[static_cast<std::size_t>(player)].nickname;
  };

  std::string out = "#character-encoding UTF-8\n";
  for (int player = 0; player < kNumPlayers; ++player) {
    const GcgPlayer& p = info.players[static_cast<std::size_t>(player)];
    out += "#player" + std::to_string(player + 1) + ' ' + p.nickname + ' ' +
           (p.full_name.empty() ? p.nickname : p.full_name) + '\n';
  }
  if (!info.title.empty()) out += "#title " + info.title + '\n';
  if (!info.lexicon.empty()) out += "#lexicon " + info.lexicon + '\n';

  for (const MoveRecord& record : game.history()) {
    out += '>' + nick(record.player) + ": " + record.rack_before.ToString() +
           ' ' + MoveText(record.move) + ' ' + Signed(record.score) + ' ' +
           std::to_string(record.cumulative) + '\n';
  }

  // Rack penalty line: the player's own tiles, then the same tiles in
  // parentheses with the (non-positive) adjustment.
  const auto penalty = [&](int player) {
    const std::string rack = game.rack(player).ToString();
    out += '>' + nick(player) + ": " + rack + " (" + rack + ") -" +
           std::to_string(-game.end_adjustment(player)) + ' ' +
           std::to_string(game.score(player)) + '\n';
  };
  switch (game.status()) {
    case GameStatus::kInProgress:
      break;
    case GameStatus::kWentOut: {
      // Going-out line: no rack field, the opponent's tiles in parentheses.
      const int winner = game.went_out();
      const int loser = 1 - winner;
      out += '>' + nick(winner) + ": (" + game.rack(loser).ToString() + ") " +
             Signed(game.end_adjustment(winner)) + ' ' +
             std::to_string(game.score(winner)) + '\n';
      if (game.end_adjustment(loser) != 0) penalty(loser);
      break;
    }
    case GameStatus::kScorelessTurns:
      for (int player = 0; player < kNumPlayers; ++player) {
        if (!game.rack(player).IsEmpty()) penalty(player);
      }
      break;
  }
  return out;
}

}  // namespace scrabble
