#include "scrabble/gcg.h"

#include <charconv>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
  while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
  return text;
}

std::vector<std::string_view> Split(std::string_view text) {
  std::vector<std::string_view> tokens;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && IsSpace(text[i])) ++i;
    const std::size_t start = i;
    while (i < text.size() && !IsSpace(text[i])) ++i;
    if (i > start) tokens.push_back(text.substr(start, i - start));
  }
  return tokens;
}

// A signed decimal integer; a leading '+' is accepted ("+24").
std::optional<int> ParseInt(std::string_view text) {
  if (!text.empty() && text.front() == '+') text.remove_prefix(1);
  if (text.empty()) return std::nullopt;
  int value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

bool IsParenthesised(std::string_view token) {
  return token.size() >= 2 && token.front() == '(' && token.back() == ')';
}

// A board coordinate as written in notation: "8D" (across) or "D8" (down).
bool LooksLikeCoordinate(std::string_view token) {
  if (token.size() < 2 || token.size() > 3) return false;
  const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
  const auto is_alpha = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  };
  return (is_digit(token.front()) && is_alpha(token.back())) ||
         (is_alpha(token.front()) && is_digit(token.back()));
}

// Parses the lines of one record into `game`; tracks the seat assignment.
class Parser {
 public:
  explicit Parser(GcgGame& game) : game_(game) {}

  void ParseLine(std::string_view line, int number) {
    number_ = number;
    line = Trim(line);
    if (line.empty()) return;
    if (line.front() == '#') {
      ParsePragma(line.substr(1));
    } else if (line.front() == '>') {
      ParseEvent(line.substr(1));
    } else {
      Fail("expected a pragma ('#') or an event ('>')");
    }
  }

  bool saw_player() const { return declared_[0] || declared_[1]; }

 private:
  [[noreturn]] void Fail(const std::string& message) const {
    throw GcgParseError(number_, message);
  }

  void ParsePragma(std::string_view pragma) {
    const std::size_t split = pragma.find_first_of(" \t");
    const std::string_view name = pragma.substr(0, split);
    const std::string_view value =
        split == std::string_view::npos ? std::string_view()
                                        : Trim(pragma.substr(split));
    if (name == "player1" || name == "player2") {
      const std::size_t seat = name == "player1" ? 0 : 1;
      const std::vector<std::string_view> words = Split(value);
      if (words.empty()) Fail("#" + std::string(name) + " needs a nickname");
      GcgPlayer& player = game_.info.players[seat];
      player.nickname = std::string(words[0]);
      const std::string_view rest = Trim(value.substr(words[0].size()));
      player.full_name = rest.empty() ? player.nickname : std::string(rest);
      declared_[seat] = true;
    } else if (name == "title") {
      game_.info.title = std::string(value);
    } else if (name == "lexicon") {
      game_.info.lexicon = std::string(value);
    } else if (name == "note") {
      // A note before the first event describes the game as a whole and
      // has no event to attach to.
      if (game_.events.empty()) return;
      std::string& note = game_.events.back().note;
      if (!note.empty()) note += '\n';
      note += value;
    }
  }

  int Seat(std::string_view nickname) {
    for (std::size_t seat = 0; seat < kNumPlayers; ++seat) {
      if (game_.info.players[seat].nickname == nickname) {
        return static_cast<int>(seat);
      }
    }
    for (std::size_t seat = 0; seat < kNumPlayers; ++seat) {
      if (!declared_[seat] && game_.info.players[seat].nickname.empty()) {
        game_.info.players[seat] = {std::string(nickname),
                                    std::string(nickname)};
        return static_cast<int>(seat);
      }
    }
    Fail("unknown player '" + std::string(nickname) +
         "'; a game has two players");
  }

  Rack ParseRack(std::string_view text) const {
    const std::optional<Rack> rack = Rack::FromString(text);
    if (!rack) Fail("bad rack '" + std::string(text) + "'");
    return *rack;
  }

  // Exchange ("-ABC" or "-7"), pass ("-") or withdrawal ("--").
  void ParseDash(std::string_view token, GcgEvent& event) const {
    if (token == "--") {
      event.type = GcgEventType::kWithdrawn;
      return;
    }
    if (const std::optional<int> count = ParseInt(token.substr(1))) {
      if (*count < 1 || *count > kRackSize) Fail("bad exchange count");
      event.exchange_count = *count;
      return;
    }
    event.move = Move::Parse(token);
    if (!event.move) Fail("bad exchange '" + std::string(token) + "'");
  }

  void ParsePlacement(std::string_view coordinate, std::string_view word,
                      GcgEvent& event) const {
    const std::string text = std::string(coordinate) + ' ' + std::string(word);
    event.move = Move::Parse(text);
    if (!event.move || event.move->type != MoveType::kPlace) {
      Fail("bad placement '" + text + "'");
    }
  }

  // "(challenge)", "(time)" or "(TILES)", after an optional rack.
  void ParseParenthesised(std::string_view token, bool has_rack,
                          GcgEvent& event) const {
    const std::string_view inner = token.substr(1, token.size() - 2);
    if (inner == "challenge") {
      event.type = GcgEventType::kChallengeBonus;
    } else if (inner == "time") {
      event.type = GcgEventType::kTimePenalty;
    } else {
      event.type =
          has_rack ? GcgEventType::kRackPenalty : GcgEventType::kEndPoints;
      event.tiles = ParseRack(inner);
    }
  }

  void ParseEvent(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) Fail("event has no ':'");
    const std::string_view nickname = Trim(text.substr(0, colon));
    if (nickname.empty()) Fail("event has no player");
    const std::vector<std::string_view> tokens = Split(text.substr(colon + 1));
    if (tokens.size() < 3 || tokens.size() > 5) {
      Fail("expected '[rack] move score total'");
    }

    GcgEvent event;
    event.line = number_;
    event.player = Seat(nickname);
    const std::optional<int> score = ParseInt(tokens[tokens.size() - 2]);
    const std::optional<int> total = ParseInt(tokens.back());
    if (!score || !total) Fail("bad score or total");
    event.score = *score;
    event.cumulative = *total;

    // What precedes the score: [rack] coordinate word, [rack] -TILES,
    // [rack] (...), or rack --.
    const std::size_t body = tokens.size() - 2;
    if (body == 3) {
      event.rack = ParseRack(tokens[0]);
      ParsePlacement(tokens[1], tokens[2], event);
    } else if (body == 2 && IsParenthesised(tokens[1])) {
      event.rack = ParseRack(tokens[0]);
      ParseParenthesised(tokens[1], /*has_rack=*/true, event);
    } else if (body == 2 && tokens[1].front() == '-') {
      event.rack = ParseRack(tokens[0]);
      ParseDash(tokens[1], event);
    } else if (body == 2 && LooksLikeCoordinate(tokens[0])) {
      ParsePlacement(tokens[0], tokens[1], event);
    } else if (body == 1 && IsParenthesised(tokens[0])) {
      ParseParenthesised(tokens[0], /*has_rack=*/false, event);
    } else if (body == 1 && tokens[0].front() == '-') {
      ParseDash(tokens[0], event);
    } else {
      Fail("unrecognised event");
    }
    game_.events.push_back(std::move(event));
  }

  GcgGame& game_;
  std::array<bool, kNumPlayers> declared_{};
  int number_ = 0;
};

}  // namespace

GcgParseError::GcgParseError(int line, const std::string& message)
    : std::runtime_error("GCG line " + std::to_string(line) + ": " + message),
      line_(line) {}

GcgGame ParseGcg(std::string_view text) {
  GcgGame game;
  Parser parser(game);
  int number = 0;
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    ++number;
    parser.ParseLine(text.substr(0, end), number);
    if (end == std::string_view::npos) break;
    text.remove_prefix(end + 1);
  }
  if (!parser.saw_player() && game.events.empty()) {
    throw GcgParseError(number, "no players and no events: not a GCG record");
  }
  return game;
}

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
