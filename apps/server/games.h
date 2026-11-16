// Streamed games for the game viewer: bot-vs-bot self-play and GCG replay,
// plus routing for human-vs-bot games (human_game.h).
//
// Both modes produce the same event stream (see web/src/api/types.ts for
// the full schema), so the viewer has one code path:
//
//   start  players, settings, the empty board, opening racks, bag count
//   move   one per turn: notation, mover, rack before and leave, points,
//          cumulative scores, tiles drawn, bag count, the board after the
//          move, both racks after it, and the engine's analysis of the
//          position (top candidates with static equity; for simulated
//          moves the rollout statistics; for endgame moves the solver's
//          result)
//   end    final scores with end-of-game adjustments, the reason the game
//          ended, and the game record as GCG text
//
// Self-play runs on a registry worker thread (game_registry.h) and
// publishes each event as soon as the move is decided. A replay is parsed,
// checked against the board and annotated up front, so a malformed record
// is rejected with a 4xx before any game is registered.
//
// Expensive settings are capped (kMax* below) so one request cannot
// monopolise the machine: a game's simulations use at most
// kSimulationThreadsPerGame threads, and at most
// RegistryOptions::max_running games compute at once.

#ifndef SCRABBLE_SERVER_GAMES_H_
#define SCRABBLE_SERVER_GAMES_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "analysis.h"
#include "api.h"
#include "game_registry.h"
#include "human_game.h"
#include "scrabble/bot.h"
#include "scrabble/gaddag.h"
#include "scrabble/game.h"
#include "scrabble/leave_evaluator.h"

namespace scrabble::server {

inline constexpr int kMaxSimulationCandidates = 20;
inline constexpr int kMaxSimulationIterations = 1000;
inline constexpr int kMaxRolloutPlies = 4;
inline constexpr int kSimulationThreadsPerGame = 2;
inline constexpr std::int64_t kMaxEndgameNodes = 5'000'000;
// Static alternatives listed per move when the move was not simulated.
inline constexpr int kDefaultAnalysisCandidates = 8;
inline constexpr int kMaxAnalysisCandidates = 30;
// Seeds are exchanged as JSON numbers, which JavaScript reads as doubles,
// so they are kept within the exactly representable integer range.
inline constexpr std::uint64_t kMaxSeed = (std::uint64_t{1} << 53) - 1;

struct PlayerSettings {
  std::string name;
  LeaveKind leaves = LeaveKind::kLearned;
  bool simulate = false;
  int candidates = 10;
  int iterations = 200;
  int plies = 2;
  bool endgame = false;
  std::int64_t endgame_nodes = 1'000'000;
};

struct SelfPlaySettings {
  std::uint64_t seed = 1;
  EndRules rules = EndRules::kNaspa;
  std::array<PlayerSettings, kNumPlayers> players;
  int analysis_candidates = kDefaultAnalysisCandidates;
};

// Parses and validates the self-play fields of a POST /api/games body:
//
//   {"mode": "selfplay", "seed": 42, "rules": "naspa" | "wespa",
//    "analysisCandidates": 8,
//    "players": [{"name": "sim", "leaves": "learned" | "heuristic" | "zero",
//                 "simulation": {"enabled": true, "candidates": 10,
//                                "iterations": 200, "plies": 2},
//                 "endgame": {"enabled": true, "nodeLimit": 1000000}},
//                {...}]}
//
// Every field is optional; `default_seed` is used when "seed" is absent.
// Out-of-range values are rejected (400) rather than clamped, so the client
// learns the limits; the message names the field and its range.
SelfPlaySettings SelfPlaySettingsFromJson(const nlohmann::json& request,
                                          std::uint64_t default_seed);

// The settings as echoed in the "start" event.
nlohmann::json SelfPlaySettingsToJson(const SelfPlaySettings& settings);

// The bot configuration for one seat.
BotConfig MakeBotConfig(const PlayerSettings& player);

// Plays one game to completion (or until the session is cancelled),
// publishing every event to `session`, and stores its GCG. Never throws:
// an engine failure ends the stream with reason "error".
void RunSelfPlay(const Gaddag& gaddag, const SelfPlaySettings& settings,
                 GameSession& session);

// One event of a precomputed stream.
struct GameEvent {
  std::string type;
  nlohmann::json data;
};

// Parses `gcg`, replays it on a board and returns the full event stream.
// Moves are validated against `gaddag`; a word the lexicon lacks (another
// dictionary, or a phony) is kept with a warning, since the record is what
// happened, but impossible geometry is rejected. Throws ApiError: 400
// "bad_gcg" for a syntax error, 422 "illegal_gcg_move" for a move that
// cannot be placed; both messages name the line.
std::vector<GameEvent> ReplayEvents(const Gaddag& gaddag, std::string_view gcg);

// Handlers for the /api/games endpoints. Thread-safe (the registry is).
class GamesApi {
 public:
  // `gaddag` and `registry` must outlive the GamesApi.
  GamesApi(const Gaddag& gaddag, GameRegistry& registry,
           HumanGamesOptions human_options = {})
      : gaddag_(&gaddag),
        registry_(&registry),
        humans_(gaddag, registry, human_options) {}

  // POST /api/games: {"mode": "selfplay", ...},
  // {"mode": "replay", "gcg": "..."} or {"mode": "human", ...} (see
  // human_game.h). 201 with {"id", "mode", "events"} (the events URL);
  // 503 "too_many_games" when the server is busy.
  ApiResponse Start(std::string_view body);
  // POST /api/gcg: {"gcg": "..."} parsed and replayed without registering
  // a game, for clients that load a record into their own editor (the
  // solver's import). 200 with the replay's events grouped by type:
  // {"start": {...}, "moves": [...], "end": {...}}, the same objects the
  // event stream would carry; errors as for a "replay" game.
  ApiResponse ParseRecord(std::string_view body) const;
  // GET /api/games/{id}: the session summary.
  ApiResponse Status(std::string_view id);
  // DELETE /api/games/{id}: cancels a running game (or ends a human game).
  ApiResponse Cancel(std::string_view id);

  // POST /api/games/{id}/moves: the human's move in a human-vs-bot game.
  ApiResponse HumanMove(std::string_view id, std::string_view body) {
    return humans_.Play(id, body);
  }
  // GET /api/games/{id}/hint: the engine's suggestions for the human.
  ApiResponse Hint(std::string_view id) { return humans_.Hint(id); }

  // The session behind GET /api/games/{id}/events and /gcg; null if
  // unknown.
  std::shared_ptr<GameSession> Find(std::string_view id) {
    return registry_->Find(id);
  }

 private:
  const Gaddag* gaddag_;
  GameRegistry* registry_;
  HumanGames humans_;
};

}  // namespace scrabble::server

#endif  // SCRABBLE_SERVER_GAMES_H_
