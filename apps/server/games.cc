#include "games.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analysis.h"
#include "codec.h"
#include "scrabble/board.h"
#include "scrabble/endgame.h"
#include "scrabble/gcg.h"
#include "scrabble/move.h"
#include "scrabble/move_validation.h"
#include "scrabble/position.h"
#include "scrabble/rack.h"
#include "scrabble/rng.h"
#include "scrabble/scoring.h"
#include "scrabble/simulation.h"
#include "scrabble/tile.h"

namespace scrabble::server {
namespace {

using Json = nlohmann::json;

// GCG records are a few kilobytes; anything far larger is not one.
constexpr std::size_t kMaxGcgBytes = 64 * 1024;
constexpr int kMinSimulationIterations = 16;
constexpr std::int64_t kMinEndgameNodes = 1000;

// Salt separating the bots' decision stream from the game's draw stream,
// which Game seeds from the same user-visible seed.
constexpr std::uint64_t kDecisionSalt = 0x5DEECE66DULL;

// ---------------------------------------------------------------------------
// Request parsing

std::string_view RulesName(EndRules rules) {
  return rules == EndRules::kWespa ? "wespa" : "naspa";
}

PlayerSettings PlayerFromJson(const Json& object, int seat) {
  const std::string path = "players[" + std::to_string(seat) + "]";
  if (!object.is_object()) BadSetting(path, "an object");
  PlayerSettings player;

  player.name = PlayerNameField(object, "name", "bot" + std::to_string(seat + 1),
                                path + ".name");

  if (const Json* leaves = Field(object, "leaves")) {
    const std::string expected = "\"learned\", \"heuristic\" or \"zero\"";
    if (!leaves->is_string()) BadSetting(path + ".leaves", expected);
    const auto& text = leaves->get_ref<const std::string&>();
    if (text == "learned") {
      player.leaves = LeaveKind::kLearned;
    } else if (text == "heuristic") {
      player.leaves = LeaveKind::kHeuristic;
    } else if (text == "zero") {
      player.leaves = LeaveKind::kZero;
    } else {
      BadSetting(path + ".leaves", expected);
    }
  }

  const std::string sim_path = path + ".simulation";
  const Json& sim = ObjectField(object, "simulation", sim_path);
  player.simulate = BoolField(sim, "enabled", false, sim_path + ".enabled");
  player.candidates = static_cast<int>(
      IntField(sim, "candidates", player.candidates, 1,
               kMaxSimulationCandidates, sim_path + ".candidates"));
  player.iterations = static_cast<int>(
      IntField(sim, "iterations", player.iterations, kMinSimulationIterations,
               kMaxSimulationIterations, sim_path + ".iterations"));
  player.plies = static_cast<int>(IntField(
      sim, "plies", player.plies, 0, kMaxRolloutPlies, sim_path + ".plies"));

  const std::string endgame_path = path + ".endgame";
  const Json& endgame = ObjectField(object, "endgame", endgame_path);
  player.endgame =
      BoolField(endgame, "enabled", false, endgame_path + ".enabled");
  player.endgame_nodes =
      IntField(endgame, "nodeLimit", player.endgame_nodes, kMinEndgameNodes,
               kMaxEndgameNodes, endgame_path + ".nodeLimit");
  return player;
}

Json PlayerSettingsToJson(const PlayerSettings& player) {
  return {{"name", player.name},
          {"leaves", LeaveKindName(player.leaves)},
          {"simulation",
           {{"enabled", player.simulate},
            {"candidates", player.candidates},
            {"iterations", player.iterations},
            {"plies", player.plies}}},
          {"endgame",
           {{"enabled", player.endgame},
            {"nodeLimit", player.endgame_nodes}}}};
}

// ---------------------------------------------------------------------------
// Event encoding

Json Pair(int a, int b) { return Json::array({a, b}); }

// ---------------------------------------------------------------------------
// Self-play

void PlaySelfPlay(const Gaddag& gaddag, const SelfPlaySettings& settings,
                  GameSession& session) {
  Game game(gaddag, settings.seed, {.end_rules = settings.rules});
  std::array<std::unique_ptr<Bot>, kNumPlayers> bots;
  for (std::size_t seat = 0; seat < kNumPlayers; ++seat) {
    bots[seat] =
        std::make_unique<Bot>(gaddag, MakeBotConfig(settings.players[seat]));
  }
  Rng rng(settings.seed ^ kDecisionSalt);

  Json start = SelfPlaySettingsToJson(settings);
  start["mode"] = "selfplay";
  start["board"] = BoardToJson(game.board());
  start["racks"] = {game.rack(0).ToString(), game.rack(1).ToString()};
  start["bag"] = game.bag().Size();
  start["scores"] = Pair(0, 0);
  session.Publish("start", std::move(start));

  std::array<int, kNumPlayers> scores{};
  std::vector<Move> ranked;
  bool cancelled = false;
  while (!game.IsOver()) {
    if (session.cancel_requested()) {
      cancelled = true;
      break;
    }
    const int mover = game.to_move();
    const PlayerSettings& player =
        settings.players[static_cast<std::size_t>(mover)];
    Bot& bot = *bots[static_cast<std::size_t>(mover)];
    const PlayerView view = game.ViewForMover();

    const auto begin = std::chrono::steady_clock::now();
    const Move move = bot.ChooseMove(view, rng);
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - begin)
                                  .count();
    // RankMoves() reuses the bot's static buffers but leaves the
    // simulation and endgame results of ChooseMove() intact.
    bot.RankMoves(view, std::numeric_limits<std::size_t>::max(), ranked);
    Json analysis =
        AnalysisJson(ranked, view.rack, move, settings.analysis_candidates,
                     bot.last_simulation(), player.plies, bot.last_endgame());

    const PlayResult result = game.Play(move);
    if (!result.ok()) {
      throw std::logic_error("bot chose an illegal move " + move.ToString() +
                             ": " + std::string(PlayErrorMessage(result)));
    }
    const MoveRecord& record = game.history().back();
    scores[static_cast<std::size_t>(mover)] = record.cumulative;
    session.Publish(
        "move",
        {{"turn", game.turn() - 1},
         {"player", mover},
         {"kind", "play"},
         {"label", record.move.ToString()},
         {"rack", record.rack_before.ToString()},
         {"move", MoveToJson(record.move, &record.rack_before)},
         {"score", record.score},
         {"equity", Round2(move.equity)},
         {"scores", Pair(scores[0], scores[1])},
         {"drawn", record.drawn.ToString()},
         {"bag", game.bag().Size()},
         {"board", BoardToJson(game.board())},
         {"racks", {game.rack(0).ToString(), game.rack(1).ToString()}},
         {"elapsedMs", Round2(elapsed_ms)},
         {"analysis", std::move(analysis)}});
  }

  GcgInfo info;
  for (std::size_t seat = 0; seat < kNumPlayers; ++seat) {
    info.players[seat] = {settings.players[seat].name, ""};
  }
  info.title = "Self-play, seed " + std::to_string(settings.seed);
  std::string gcg = ToGcg(game, info);
  session.set_gcg(gcg);

  const char* reason = cancelled ? "cancelled"
                       : game.status() == GameStatus::kWentOut
                           ? "went_out"
                           : "scoreless_turns";
  const int spread = game.spread(0);
  session.Publish("end",
                  {{"reason", reason},
                   {"scores", Pair(game.score(0), game.score(1))},
                   {"adjustments",
                    Pair(game.end_adjustment(0), game.end_adjustment(1))},
                   {"racks", {game.rack(0).ToString(), game.rack(1).ToString()}},
                   {"winner", cancelled     ? Json(nullptr)
                              : spread > 0  ? Json(0)
                              : spread < 0  ? Json(1)
                                            : Json(-1)},
                   {"turns", game.turn()},
                   {"gcg", std::move(gcg)}});
}

// ---------------------------------------------------------------------------
// GCG replay

bool IsWordError(MoveError error) {
  return error == MoveError::kInvalidWord ||
         error == MoveError::kInvalidCrossWord;
}

bool RackHolds(const Rack& rack, const Rack& needed) {
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    if (rack.Count(letter) < needed.Count(letter)) return false;
  }
  return true;
}

// Resolves play-through squares from the board without consulting the
// lexicon. False if the geometry is impossible: off the board, a
// play-through square that is empty, or a placed tile on an occupied one.
bool ResolveGeometry(const Board& board, Move& move) {
  const int dr = move.direction == Direction::kDown ? 1 : 0;
  const int dc = 1 - dr;
  for (int i = 0; i < move.num_tiles; ++i) {
    const int row = move.row + i * dr;
    const int col = move.col + i * dc;
    if (!InBounds(row, col)) return false;
    const Square& square = board.At(row, col);
    MoveTile& tile = move.tiles[static_cast<std::size_t>(i)];
    if (tile.is_play_through()) {
      if (square.IsEmpty()) return false;
      tile = MoveTile::PlayThrough(square.letter, square.is_blank);
    } else if (!square.IsEmpty()) {
      return false;
    }
  }
  return true;
}

std::string Signed(int value) {
  return (value >= 0 ? "+" : "") + std::to_string(value);
}

Json RackOrNull(const std::optional<Rack>& rack) {
  return rack ? Json(rack->ToString()) : Json(nullptr);
}

class Replayer {
 public:
  Replayer(const Gaddag& gaddag, std::string_view text, GcgGame record)
      : gaddag_(gaddag),
        text_(text),
        record_(std::move(record)),
        bot_(gaddag, BotConfig{}) {}

  std::vector<GameEvent> Run() {
    ComputeRacksAfter();
    PublishStart();
    for (std::size_t i = 0; i < record_.events.size(); ++i) Replay(i);
    PublishEnd();
    return std::move(events_);
  }

 private:
  // What a player's rack is after event i: the rack recorded on their next
  // line, or what the end-of-game lines reveal. Unknown when the record
  // does not say.
  void ComputeRacksAfter() {
    const std::size_t n = record_.events.size();
    racks_after_.resize(n);
    std::array<std::optional<Rack>, kNumPlayers> upcoming;
    for (std::size_t i = n; i-- > 0;) {
      racks_after_[i] = upcoming;
      const GcgEvent& event = record_.events[i];
      const auto seat = static_cast<std::size_t>(event.player);
      if (event.type == GcgEventType::kEndPoints) {
        upcoming[seat] = Rack{};  // went out
        upcoming[1 - seat] = event.tiles;
      } else if (event.rack) {
        upcoming[seat] = event.rack;
      }
    }
    initial_racks_ = upcoming;
  }

  int RackSizeOrFull(const std::optional<Rack>& rack) const {
    return rack ? rack->Size() : kRackSize;
  }

  // Tiles in the bag after event i, inferred from the board and the racks.
  int BagAfter(std::size_t i) const {
    const auto& racks = racks_after_[i];
    const int bag = kTotalTiles - board_.TileCount() -
                    RackSizeOrFull(racks[0]) - RackSizeOrFull(racks[1]);
    return std::max(bag, 0);
  }

  void PublishStart() {
    Json players = Json::array();
    for (const GcgPlayer& player : record_.info.players) {
      players.push_back({{"name", player.nickname.empty() ? std::string("?")
                                                          : player.nickname},
                         {"fullName", player.full_name}});
    }
    Json start = {{"mode", "replay"},
                  {"players", std::move(players)},
                  {"title", record_.info.title},
                  {"lexicon", record_.info.lexicon},
                  {"board", BoardToJson(board_)},
                  {"racks", {RackOrNull(initial_racks_[0]),
                             RackOrNull(initial_racks_[1])}},
                  {"bag", kTotalTiles - 2 * kRackSize},
                  {"scores", Pair(0, 0)}};
    events_.push_back({"start", std::move(start)});
  }

  [[noreturn]] void Illegal(const GcgEvent& event, const Move& move,
                            std::string_view reason) const {
    throw ApiError(422, "illegal_gcg_move",
                   "GCG line " + std::to_string(event.line) + ": " +
                       move.ToString() + ": " + std::string(reason));
  }

  // Validates and canonicalises a placement or exchange. Returns the
  // warning to attach, if any; sets `rack_ok` if the recorded rack holds
  // the move's tiles (so a leave can be shown and the position analysed).
  std::string CheckMove(const GcgEvent& event, Move& move, bool& rack_ok) {
    const Rack needed = RackForMove(move);
    const Rack rack = event.rack.value_or(needed);
    Move checked = move;
    const MoveError error = ValidateMove(board_, gaddag_, rack, checked);
    if (error == MoveError::kOk) {
      move = checked;
      rack_ok = event.rack.has_value();
      return {};
    }
    if (move.type != MoveType::kPlace) {
      // An exchange can only fail on tiles the rack lacks.
      rack_ok = false;
      return std::string(MoveErrorMessage(error));
    }
    if (!IsWordError(error) && error != MoveError::kNotInRack) {
      Illegal(event, move, MoveErrorMessage(error));
    }
    if (!ResolveGeometry(board_, move)) {
      Illegal(event, move, "the squares do not match the board");
    }
    move.score = Score(board_, move);
    rack_ok = event.rack.has_value() && RackHolds(rack, RackForMove(move));
    return std::string(MoveErrorMessage(error));
  }

  Json BaseEvent(std::size_t i, const GcgEvent& event, std::string_view kind,
                 std::string label) {
    scores_[static_cast<std::size_t>(event.player)] = event.cumulative;
    const auto& racks = racks_after_[i];
    Json out = {{"turn", turn_++},
                {"player", event.player},
                {"kind", kind},
                {"label", std::move(label)},
                {"rack", RackOrNull(event.rack)},
                {"move", nullptr},
                {"score", event.score},
                {"scores", Pair(scores_[0], scores_[1])},
                {"bag", BagAfter(i)},
                {"board", BoardToJson(board_)},
                {"racks", {RackOrNull(racks[0]), RackOrNull(racks[1])}}};
    if (!event.note.empty()) out["note"] = event.note;
    return out;
  }

  // Static analysis of the position before a recorded move, from the
  // mover's point of view.
  Json Analyse(const GcgEvent& event, const Move& played) {
    const Position position(gaddag_, board_);
    PlayerView view;
    view.position = &position;
    view.rack = *event.rack;
    view.player = event.player;
    view.score = scores_[static_cast<std::size_t>(event.player)];
    view.opponent_score = scores_[static_cast<std::size_t>(1 - event.player)];
    view.unseen = UnseenTiles(board_, view.rack);
    view.unseen_size = 0;
    for (const std::uint8_t count : view.unseen) view.unseen_size += count;
    view.opponent_rack_size = std::min(view.unseen_size, kRackSize);
    view.bag_size = view.unseen_size - view.opponent_rack_size;
    view.turn = turn_;
    std::vector<Move> ranked;
    bot_.RankMoves(view, std::numeric_limits<std::size_t>::max(), ranked);
    return AnalysisJson(ranked, view.rack, played, kDefaultAnalysisCandidates,
                        nullptr, 0, nullptr);
  }

  void Replay(std::size_t i) {
    const GcgEvent& event = record_.events[i];
    switch (event.type) {
      case GcgEventType::kMove: {
        if (!event.move) {
          // An exchange recorded by count only: the tiles are unknown.
          const std::string label = "-" + std::to_string(event.exchange_count);
          Json out = BaseEvent(i, event, "play", label);
          out["move"] = {{"notation", label},       {"type", "exchange"},
                         {"score", 0},              {"tilesUsed",
                                                     event.exchange_count},
                         {"bingo", false},          {"word", ""},
                         {"tiles", Json::array()}};
          events_.push_back({"move", std::move(out)});
          return;
        }
        Move move = *event.move;
        bool rack_ok = false;
        const std::string warning = CheckMove(event, move, rack_ok);
        Json analysis = rack_ok ? Analyse(event, move) : Json(nullptr);
        const Board before = board_;
        if (move.type == MoveType::kPlace) {
          PlaceTiles(board_, move);
          last_placement_ = {before, event.player};
        }
        Json out = BaseEvent(i, event, "play", move.ToString());
        out["move"] = MoveToJson(move, rack_ok ? &*event.rack : nullptr);
        if (!warning.empty()) out["warning"] = warning;
        if (move.score != event.score) {
          const std::string mismatch = "recorded " + Signed(event.score) +
                                       ", the engine scores " +
                                       Signed(move.score);
          out["warning"] =
              warning.empty() ? mismatch : warning + "; " + mismatch;
        }
        if (!analysis.is_null()) out["analysis"] = std::move(analysis);
        events_.push_back({"move", std::move(out)});
        return;
      }
      case GcgEventType::kWithdrawn: {
        if (!last_placement_ || last_placement_->player != event.player) {
          throw ApiError(422, "illegal_gcg_move",
                         "GCG line " + std::to_string(event.line) +
                             ": nothing to withdraw");
        }
        board_ = last_placement_->board_before;
        last_placement_.reset();
        events_.push_back(
            {"move", BaseEvent(i, event, "withdrawn", "-- (withdrawn)")});
        return;
      }
      case GcgEventType::kChallengeBonus:
      case GcgEventType::kTimePenalty: {
        const std::string label =
            std::string(event.type == GcgEventType::kChallengeBonus
                            ? "(challenge) "
                            : "(time) ") +
            Signed(event.score);
        events_.push_back({"move", BaseEvent(i, event, "adjustment", label)});
        return;
      }
      case GcgEventType::kEndPoints:
        went_out_ = true;
        [[fallthrough]];
      case GcgEventType::kRackPenalty: {
        const auto seat = static_cast<std::size_t>(event.player);
        scores_[seat] = event.cumulative;
        adjustments_[seat] += event.score;
        if (event.type == GcgEventType::kRackPenalty) {
          final_racks_[seat] = event.tiles;
          penalties_ = true;
        } else {
          final_racks_[seat] = Rack{};
          final_racks_[1 - seat] = event.tiles;
        }
        return;
      }
    }
  }

  void PublishEnd() {
    const int spread = scores_[0] - scores_[1];
    const char* reason = went_out_    ? "went_out"
                         : penalties_ ? "scoreless_turns"
                                      : "record_end";
    events_.push_back(
        {"end",
         {{"reason", reason},
          {"scores", Pair(scores_[0], scores_[1])},
          {"adjustments", Pair(adjustments_[0], adjustments_[1])},
          {"racks", {RackOrNull(final_racks_[0]), RackOrNull(final_racks_[1])}},
          {"winner", spread > 0 ? 0 : spread < 0 ? 1 : -1},
          {"turns", turn_},
          {"gcg", std::string(text_)}}});
  }

  struct Placement {
    Board board_before;
    int player = 0;
  };

  const Gaddag& gaddag_;
  std::string_view text_;
  GcgGame record_;
  Bot bot_;  // static evaluation with the default (learned) leaves
  std::vector<std::array<std::optional<Rack>, kNumPlayers>> racks_after_;
  std::array<std::optional<Rack>, kNumPlayers> initial_racks_;
  std::array<std::optional<Rack>, kNumPlayers> final_racks_;
  Board board_;
  std::optional<Placement> last_placement_;
  std::array<int, kNumPlayers> scores_{};
  std::array<int, kNumPlayers> adjustments_{};
  int turn_ = 0;
  bool went_out_ = false;
  bool penalties_ = false;
  std::vector<GameEvent> events_;
};

std::uint64_t RandomSeed() {
  std::random_device device;
  const std::uint64_t seed =
      (std::uint64_t{device()} << 32) ^ std::uint64_t{device()};
  return seed & kMaxSeed;
}

}  // namespace

SelfPlaySettings SelfPlaySettingsFromJson(const Json& request,
                                          std::uint64_t default_seed) {
  SelfPlaySettings settings;
  settings.seed = static_cast<std::uint64_t>(IntField(
      request, "seed", static_cast<std::int64_t>(default_seed & kMaxSeed), 0,
      static_cast<std::int64_t>(kMaxSeed), "seed"));

  if (const Json* rules = Field(request, "rules")) {
    if (!rules->is_string()) BadSetting("rules", "\"naspa\" or \"wespa\"");
    const auto& text = rules->get_ref<const std::string&>();
    if (text == "naspa") {
      settings.rules = EndRules::kNaspa;
    } else if (text == "wespa") {
      settings.rules = EndRules::kWespa;
    } else {
      BadSetting("rules", "\"naspa\" or \"wespa\"");
    }
  }

  settings.analysis_candidates = static_cast<int>(IntField(
      request, "analysisCandidates", kDefaultAnalysisCandidates, 1,
      kMaxAnalysisCandidates, "analysisCandidates"));

  const Json* players = Field(request, "players");
  if (players != nullptr &&
      (!players->is_array() || players->size() != kNumPlayers)) {
    BadSetting("players", "an array of two player objects");
  }
  for (std::size_t seat = 0; seat < kNumPlayers; ++seat) {
    settings.players[seat] =
        PlayerFromJson(players != nullptr ? (*players)[seat] : Json::object(),
                       static_cast<int>(seat));
  }
  if (settings.players[0].name == settings.players[1].name) {
    BadSetting("players[1].name", "different from players[0].name");
  }
  return settings;
}

Json SelfPlaySettingsToJson(const SelfPlaySettings& settings) {
  return {{"seed", settings.seed},
          {"rules", RulesName(settings.rules)},
          {"analysisCandidates", settings.analysis_candidates},
          {"players", {PlayerSettingsToJson(settings.players[0]),
                       PlayerSettingsToJson(settings.players[1])}}};
}

BotConfig MakeBotConfig(const PlayerSettings& player) {
  BotConfig config;
  config.name = player.name;
  config.leaves = LeavesFor(player.leaves);
  config.simulation.enabled = player.simulate;
  config.simulation.candidates = player.candidates;
  config.simulation.iterations = player.iterations;
  config.simulation.plies = player.plies;
  // Keep pruning reachable at the small budgets the viewer allows.
  config.simulation.min_iterations =
      std::min(config.simulation.min_iterations, player.iterations);
  config.simulation.threads = kSimulationThreadsPerGame;
  config.endgame.enabled = player.endgame;
  config.endgame.node_limit = player.endgame_nodes;
  return config;
}

void RunSelfPlay(const Gaddag& gaddag, const SelfPlaySettings& settings,
                 GameSession& session) {
  try {
    PlaySelfPlay(gaddag, settings, session);
  } catch (const std::exception& e) {
    session.Publish("end", {{"reason", "error"}, {"message", e.what()}});
  }
}

std::vector<GameEvent> ReplayEvents(const Gaddag& gaddag,
                                    std::string_view gcg) {
  GcgGame record;
  try {
    record = ParseGcg(gcg);
  } catch (const GcgParseError& e) {
    throw ApiError(400, "bad_gcg", e.what());
  }
  if (record.events.empty()) {
    throw ApiError(400, "bad_gcg", "the record has no moves");
  }
  return Replayer(gaddag, gcg, std::move(record)).Run();
}

ApiResponse GamesApi::Start(std::string_view body) {
  static const std::string kModes =
      "\"selfplay\", \"replay\" or \"human\"";
  return GuardedResponse([&]() -> ApiResponse {
    const Json request = ParseRequestObject(body);
    std::string mode = "selfplay";
    if (const Json* value = Field(request, "mode")) {
      if (!value->is_string()) BadSetting("mode", kModes);
      mode = value->get<std::string>();
    }

    std::shared_ptr<GameSession> session;
    Json response;
    if (mode == "selfplay") {
      const SelfPlaySettings settings =
          SelfPlaySettingsFromJson(request, RandomSeed());
      const Gaddag* gaddag = gaddag_;
      session = registry_->StartBackground(
          mode, [gaddag, settings](GameSession& game) {
            RunSelfPlay(*gaddag, settings, game);
          });
      response["seed"] = settings.seed;
    } else if (mode == "human") {
      return humans_.Start(request, RandomSeed());
    } else if (mode == "replay") {
      const Json* gcg = Field(request, "gcg");
      if (gcg == nullptr || !gcg->is_string()) {
        throw ApiError(400, "missing_field",
                       "\"gcg\" is required and must be the record's text");
      }
      const auto& text = gcg->get_ref<const std::string&>();
      if (text.size() > kMaxGcgBytes) {
        throw ApiError(413, "gcg_too_large", "the GCG record is too large");
      }
      std::vector<GameEvent> events = ReplayEvents(*gaddag_, text);
      session = registry_->AddCompleted(mode, [&](GameSession& game) {
        for (GameEvent& event : events) {
          game.Publish(event.type, std::move(event.data));
        }
        game.set_gcg(text);
      });
    } else {
      BadSetting("mode", kModes);
    }

    if (session == nullptr) {
      return ErrorResponse(503, "too_many_games",
                           "the server is already running the maximum number "
                           "of games; try again when one finishes");
    }
    response["id"] = session->id();
    response["mode"] = mode;
    response["events"] = "/api/games/" + session->id() + "/events";
    return ApiResponse{201, std::move(response)};
  });
}

ApiResponse GamesApi::Status(std::string_view id) {
  const std::shared_ptr<GameSession> session = registry_->Find(id);
  if (session == nullptr) {
    return ErrorResponse(404, "unknown_game",
                         "no game " + std::string(id) + " (it may have expired)");
  }
  return {200, session->Summary()};
}

ApiResponse GamesApi::Cancel(std::string_view id) {
  if (humans_.Cancel(id)) return Status(id);
  if (!registry_->Cancel(id)) {
    return ErrorResponse(404, "unknown_game",
                         "no game " + std::string(id) + " (it may have expired)");
  }
  return Status(id);
}

}  // namespace scrabble::server
