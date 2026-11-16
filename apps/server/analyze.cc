#include "analyze.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "analysis.h"
#include "codec.h"
#include "scrabble/board.h"
#include "scrabble/bot.h"
#include "scrabble/game.h"
#include "scrabble/move.h"
#include "scrabble/position.h"
#include "scrabble/rack.h"
#include "scrabble/rng.h"
#include "scrabble/tile.h"

namespace scrabble::server {
namespace {

using Json = nlohmann::json;

constexpr std::int64_t kMaxScore = 10000;
constexpr std::int64_t kMaxEndgameNodes = 50'000'000;
constexpr std::size_t kEndgameTableMb = 32;

enum class Effort : std::uint8_t { kStatic, kQuick, kNormal, kDeep };

struct Budget {
  int candidates = 0;
  int iterations = 0;
  int plies = 2;
  std::int64_t sim_ms = 0;
  std::int64_t endgame_ms = 0;
  std::int64_t endgame_nodes = 0;  // 0 = time limit only
};

std::string_view EffortName(Effort effort) {
  switch (effort) {
    case Effort::kStatic: return "static";
    case Effort::kQuick: return "quick";
    case Effort::kNormal: return "normal";
    case Effort::kDeep: return "deep";
  }
  return "normal";
}

// Presets sized so a request finishes in about 1, 2.5 and 4.5 seconds on
// four cores; pruning usually ends a simulation well before its budget.
Budget BudgetFor(Effort effort) {
  switch (effort) {
    case Effort::kStatic: return {};
    case Effort::kQuick: return {8, 100, 2, 1000, 1000, 0};
    case Effort::kNormal: return {12, 400, 2, 2500, 2500, 0};
    case Effort::kDeep: return {20, 1000, 2, 4500, 4500, 0};
  }
  return {};
}

Effort EffortField(const Json& request) {
  const Json* value = Field(request, "effort");
  if (value == nullptr) return Effort::kNormal;
  const std::string expected =
      "\"static\", \"quick\", \"normal\" or \"deep\"";
  if (!value->is_string()) BadSetting("effort", expected);
  const auto& text = value->get_ref<const std::string&>();
  if (text == "static") return Effort::kStatic;
  if (text == "quick") return Effort::kQuick;
  if (text == "normal") return Effort::kNormal;
  if (text == "deep") return Effort::kDeep;
  BadSetting("effort", expected);
}

Budget BudgetField(const Json& request, Effort effort) {
  Budget budget = BudgetFor(effort);
  if (effort == Effort::kStatic) return budget;
  const Json& sim = ObjectField(request, "simulation", "simulation");
  budget.candidates = static_cast<int>(
      IntField(sim, "candidates", budget.candidates, 1, kMaxAnalyzeCandidates,
               "simulation.candidates"));
  budget.iterations = static_cast<int>(
      IntField(sim, "iterations", budget.iterations, 16, kMaxAnalyzeIterations,
               "simulation.iterations"));
  budget.plies = static_cast<int>(IntField(sim, "plies", budget.plies, 0,
                                           kMaxAnalyzePlies,
                                           "simulation.plies"));
  budget.sim_ms = IntField(sim, "timeLimitMs", budget.sim_ms, 100,
                           kMaxAnalyzeMs, "simulation.timeLimitMs");
  const Json& endgame = ObjectField(request, "endgame", "endgame");
  budget.endgame_ms = IntField(endgame, "timeLimitMs", budget.endgame_ms, 100,
                               kMaxAnalyzeMs, "endgame.timeLimitMs");
  budget.endgame_nodes =
      IntField(endgame, "nodeLimit", budget.endgame_nodes, 0, kMaxEndgameNodes,
               "endgame.nodeLimit");
  return budget;
}

std::optional<Rack> OptionalRack(const Json& request, std::string_view key) {
  const Json* value = Field(request, key);
  if (value == nullptr) return std::nullopt;
  return RackFromJson(*value);
}

[[noreturn]] void Impossible(const std::string& message) {
  throw ApiError(422, "impossible_position", message);
}

std::string Letters(const std::string& rack_text) {
  std::string out;
  for (const char c : rack_text) {
    if (!out.empty()) out += ", ";
    out += c;
  }
  return out;
}

BotConfig ConfigFor(const Budget& budget, Effort effort) {
  BotConfig config;
  config.name = "solver";
  if (effort == Effort::kStatic) return config;
  config.simulation.enabled = true;
  config.simulation.candidates = budget.candidates;
  config.simulation.iterations = budget.iterations;
  config.simulation.plies = budget.plies;
  config.simulation.min_iterations = std::min(48, budget.iterations);
  config.simulation.threads = kAnalyzeThreads;
  config.simulation.time_limit_ms = budget.sim_ms;
  config.endgame.enabled = true;
  config.endgame.time_limit_ms = budget.endgame_ms;
  config.endgame.node_limit = budget.endgame_nodes;
  config.endgame.tt_size_mb = kEndgameTableMb;
  return config;
}

}  // namespace

ApiResponse AnalyzeApi::Analyze(std::string_view body) {
  return GuardedResponse([&]() -> ApiResponse {
    const auto start = std::chrono::steady_clock::now();
    const Json request = ParseRequestObject(body);
    const Json* board_field = Field(request, "board");
    const Board board =
        board_field == nullptr ? Board{} : BoardFromJson(*board_field);
    const std::optional<Rack> rack = OptionalRack(request, "rack");
    if (!rack || rack->IsEmpty()) {
      throw ApiError(400, "missing_field",
                     "\"rack\" is required: the tiles of the player to move");
    }
    const std::optional<Rack> opponent = OptionalRack(request, "opponentRack");
    const Effort effort = EffortField(request);
    const Budget budget = BudgetField(request, effort);
    const int limit = static_cast<int>(
        IntField(request, "limit", kDefaultStaticCandidates, 1,
                 kMaxStaticCandidates, "limit"));
    const auto seed = static_cast<std::uint64_t>(IntField(
        request, "seed", 1, 0, std::numeric_limits<std::int64_t>::max(),
        "seed"));

    PlayerView view;
    view.rack = *rack;
    view.score = static_cast<int>(
        IntField(request, "score", 0, -kMaxScore, kMaxScore, "score"));
    view.opponent_score = static_cast<int>(IntField(
        request, "opponentScore", 0, -kMaxScore, kMaxScore, "opponentScore"));
    view.scoreless_turns = static_cast<int>(
        IntField(request, "scorelessTurns", 0, 0, kScorelessTurnsToEnd - 1,
                 "scorelessTurns"));
    if (const Json* rules = Field(request, "rules")) {
      if (!rules->is_string() || (*rules != "naspa" && *rules != "wespa")) {
        BadSetting("rules", "\"naspa\" or \"wespa\"");
      }
      view.end_rules = *rules == "wespa" ? EndRules::kWespa : EndRules::kNaspa;
    }

    // Tile accounting from the mover's side of the table.
    const Unseen unseen = ComputeUnseen(board, *rack);
    if (!unseen.overused.empty()) {
      Impossible("the board and rack hold more " + Letters(unseen.overused) +
                 " than the bag contains");
    }
    std::array<int, kAlphabetSize> bag_counts = unseen.counts;
    int opponent_size = std::min(unseen.total, kRackSize);
    if (opponent) {
      for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
        bag_counts[letter] -= opponent->Count(letter);
        if (bag_counts[letter] < 0) {
          Impossible(std::string("the opponent's rack holds a ") +
                     LetterToChar(letter) + " that is not among the unseen tiles");
        }
      }
      opponent_size = opponent->Size();
      if (unseen.total > opponent_size && opponent_size != kRackSize) {
        Impossible("with tiles left in the bag the opponent holds " +
                   std::to_string(kRackSize) + " tiles, not " +
                   std::to_string(opponent_size));
      }
      view.known_opponent_rack = *opponent;
    }
    if (opponent_size == 0) {
      throw ApiError(422, "game_over",
                     "the opponent has no tiles left, so the game is over");
    }
    for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
      view.unseen[letter] = static_cast<std::uint8_t>(unseen.counts[letter]);
    }
    view.unseen_size = unseen.total;
    view.opponent_rack_size = opponent_size;
    view.bag_size = unseen.total - opponent_size;
    view.turn = board.TileCount() == 0 ? 0 : 1;

    const Position position(*gaddag_, board);
    view.position = &position;

    // Static analysis is cheap; anything that searches runs alone.
    std::unique_lock<std::timed_mutex> lock(search_, std::defer_lock);
    if (effort != Effort::kStatic &&
        !lock.try_lock_for(std::chrono::milliseconds(kAnalyzeQueueMs))) {
      return ErrorResponse(503, "busy",
                           "the engine is busy with another analysis; try "
                           "again shortly");
    }

    Bot bot(*gaddag_, ConfigFor(budget, effort));
    Rng rng(seed);
    Move best = bot.ChooseMove(view, rng);
    std::vector<Move> ranked;
    bot.RankMoves(view, std::numeric_limits<std::size_t>::max(), ranked);
    // A static choice is the top of the ranking; taking it from there keeps
    // equal-equity ties consistent with the listed order.
    if (bot.last_simulation() == nullptr && bot.last_endgame() == nullptr) {
      best = ranked.front();
    }
    Json response =
        AnalysisJson(ranked, view.rack, best, limit, bot.last_simulation(),
                     budget.plies, bot.last_endgame());

    response["effort"] = EffortName(effort);
    response["position"] = {{"unseen", TileCountsToJson(unseen.counts)},
                            {"unseenCount", unseen.total},
                            {"bagCount", view.bag_size},
                            {"opponentRackSize", opponent_size},
                            {"opponentRackKnown", opponent.has_value()},
                            {"tilesOnBoard", board.TileCount()}};
    response["elapsedMs"] = Round2(std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - start)
                                       .count());
    return ApiResponse{200, std::move(response)};
  });
}

}  // namespace scrabble::server
