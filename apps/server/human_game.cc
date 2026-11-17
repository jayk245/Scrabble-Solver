#include "human_game.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <string>
#include <utility>

#include "analysis.h"
#include "codec.h"
#include "scrabble/gcg.h"
#include "scrabble/move.h"
#include "scrabble/rack.h"
#include "scrabble/static_evaluator.h"
#include "scrabble/tile.h"

namespace scrabble::server {
namespace {

using Json = nlohmann::json;

// Same salt as self-play (games.cc): the bot's decisions draw from a
// stream distinct from the bag's, which Game seeds with the game seed.
constexpr std::uint64_t kDecisionSalt = 0x5DEECE66DULL;
constexpr std::uint64_t kMaxSeed = (std::uint64_t{1} << 53) - 1;
constexpr int kHintCandidates = 5;
constexpr const char* kBotName = "bot";

// Two simulation threads per thinking bot, as in self-play.
constexpr int kBotThreads = 2;

Json Pair(int a, int b) { return Json::array({a, b}); }

// What the human sees of a bot's exchange: how many tiles, not which.
Json HiddenExchangeJson(int count) {
  return {{"notation", "-" + std::to_string(count)},
          {"type", "exchange"},
          {"score", 0},
          {"tilesUsed", count},
          {"bingo", false},
          {"word", ""},
          {"tiles", Json::array()}};
}

ApiResponse Conflict(std::string_view code, std::string_view message) {
  return ErrorResponse(409, code, message);
}

double Elapsed(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - since)
      .count();
}

// Releases a thinking slot on every exit path, including exceptions.
class SlotGuard {
 public:
  explicit SlotGuard(HumanGame::ThinkingSlots& slots) : slots_(&slots) {}
  ~SlotGuard() { slots_->Release(); }
  SlotGuard(const SlotGuard&) = delete;
  SlotGuard& operator=(const SlotGuard&) = delete;

 private:
  HumanGame::ThinkingSlots* slots_;
};

}  // namespace

std::string_view BotStrengthName(BotStrength strength) {
  switch (strength) {
    case BotStrength::kEasy: return "easy";
    case BotStrength::kNormal: return "normal";
    case BotStrength::kHard: return "hard";
    case BotStrength::kExpert: return "expert";
  }
  return "normal";
}

BotConfig BotConfigFor(BotStrength strength) {
  BotConfig config;
  config.name = kBotName;
  config.simulation.threads = kBotThreads;
  switch (strength) {
    case BotStrength::kEasy:
      // Greedy: the highest score now, with no regard for the tiles kept.
      config.leaves = LeavesFor(LeaveKind::kZero);
      break;
    case BotStrength::kNormal:
      break;
    case BotStrength::kHard:
      config.simulation.enabled = true;
      config.simulation.candidates = 10;
      config.simulation.iterations = 300;
      config.simulation.min_iterations = 32;
      config.simulation.time_limit_ms = 3000;
      break;
    case BotStrength::kExpert:
      config.simulation.enabled = true;
      config.simulation.candidates = 15;
      config.simulation.iterations = 600;
      config.simulation.min_iterations = 48;
      config.simulation.time_limit_ms = 4000;
      config.endgame.enabled = true;
      config.endgame.node_limit = 0;
      config.endgame.time_limit_ms = 4000;
      break;
  }
  return config;
}

HumanGameSettings HumanGameSettingsFromJson(const Json& request,
                                            std::uint64_t default_seed) {
  HumanGameSettings settings;
  settings.human_name =
      PlayerNameField(request, "name", settings.human_name, "name");
  if (settings.human_name == kBotName) {
    BadSetting("name", std::string("different from \"") + kBotName + "\"");
  }

  if (const Json* strength = Field(request, "strength")) {
    const std::string expected =
        "\"easy\", \"normal\", \"hard\" or \"expert\"";
    if (!strength->is_string()) BadSetting("strength", expected);
    const auto& text = strength->get_ref<const std::string&>();
    if (text == "easy") {
      settings.strength = BotStrength::kEasy;
    } else if (text == "normal") {
      settings.strength = BotStrength::kNormal;
    } else if (text == "hard") {
      settings.strength = BotStrength::kHard;
    } else if (text == "expert") {
      settings.strength = BotStrength::kExpert;
    } else {
      BadSetting("strength", expected);
    }
  }

  settings.human_first =
      BoolField(request, "humanFirst", settings.human_first, "humanFirst");
  settings.seed_given = Field(request, "seed") != nullptr;
  settings.seed = static_cast<std::uint64_t>(
      IntField(request, "seed", static_cast<std::int64_t>(default_seed & kMaxSeed),
               0, static_cast<std::int64_t>(kMaxSeed), "seed"));

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
  return settings;
}

// ---------------------------------------------------------------------------
// HumanGame::ThinkingSlots

bool HumanGame::ThinkingSlots::Acquire(const std::atomic<bool>& stop) {
  std::unique_lock lock(mutex_);
  // Polls `stop` so a game destroyed while waiting does not block on a
  // slot another game holds.
  while (free_ == 0) {
    if (stop.load()) return false;
    changed_.wait_for(lock, std::chrono::milliseconds(100));
  }
  if (stop.load()) return false;
  --free_;
  return true;
}

void HumanGame::ThinkingSlots::Release() {
  {
    const std::lock_guard lock(mutex_);
    ++free_;
  }
  changed_.notify_one();
}

// ---------------------------------------------------------------------------
// HumanGame

HumanGame::HumanGame(const Gaddag& gaddag, const HumanGameSettings& settings,
                     std::shared_ptr<GameSession> session, ThinkingSlots& slots,
                     Clock::time_point now)
    : settings_(settings),
      human_seat_(settings.human_first ? 0 : 1),
      session_(std::move(session)),
      slots_(&slots),
      game_(gaddag, settings.seed, {.end_rules = settings.rules}),
      analyst_(gaddag, BotConfig{}),
      last_active_(now),
      bot_(gaddag, BotConfigFor(settings.strength)),
      rng_(settings.seed ^ kDecisionSalt) {}

HumanGame::~HumanGame() {
  stop_.store(true);
  std::thread worker;
  {
    const std::lock_guard lock(mutex_);
    worker = std::move(worker_);
  }
  if (worker.joinable()) worker.join();
}

void HumanGame::Begin() {
  const std::lock_guard lock(mutex_);
  session_->Publish("start", StartEventLocked());
  if (game_.to_move() == bot_seat()) StartBotLocked();
}

bool HumanGame::finished() const {
  const std::lock_guard lock(mutex_);
  return finished_;
}

bool HumanGame::Idle(Clock::time_point now, Clock::duration ttl) {
  const std::lock_guard lock(mutex_);
  // A connected stream counts as activity: the player is looking.
  if (session_->readers() > 0) last_active_ = now;
  return now - last_active_ >= ttl;
}

Json HumanGame::StartEventLocked() const {
  Json players = Json::array();
  for (int seat = 0; seat < kNumPlayers; ++seat) {
    if (seat == human_seat_) {
      players.push_back({{"name", settings_.human_name}, {"kind", "human"}});
    } else {
      players.push_back({{"name", kBotName},
                         {"kind", "bot"},
                         {"strength", BotStrengthName(settings_.strength)}});
    }
  }
  Json racks = Json::array({nullptr, nullptr});
  racks[static_cast<std::size_t>(human_seat_)] =
      game_.rack(human_seat_).ToString();
  Json start = {{"mode", "human"},
                {"players", std::move(players)},
                {"humanSeat", human_seat_},
                {"strength", BotStrengthName(settings_.strength)},
                {"rules", settings_.rules == EndRules::kWespa ? "wespa"
                                                              : "naspa"},
                {"board", BoardToJson(game_.board())},
                {"racks", std::move(racks)},
                {"bag", game_.bag().Size()},
                {"scores", Pair(0, 0)},
                {"toMove", game_.to_move()}};
  // The seed fixes the bag order, so a random one stays secret until the
  // end; one the client chose is no secret.
  if (settings_.seed_given) start["seed"] = settings_.seed;
  return start;
}

Json HumanGame::MoveEventLocked(const MoveRecord& record) const {
  const bool by_human = record.player == human_seat_;
  std::string label = record.move.ToString();
  Json move;
  if (!by_human && record.move.type == MoveType::kExchange) {
    move = HiddenExchangeJson(record.move.TilesUsed());
    label = move["notation"];
  } else {
    // The leave is only shown for the human's own move.
    move = MoveToJson(record.move, by_human ? &record.rack_before : nullptr);
  }
  Json racks = Json::array({nullptr, nullptr});
  racks[static_cast<std::size_t>(human_seat_)] =
      game_.rack(human_seat_).ToString();
  Json event = {
      {"turn", game_.turn() - 1},
      {"player", record.player},
      {"kind", "play"},
      {"label", std::move(label)},
      {"rack", by_human ? Json(record.rack_before.ToString()) : Json(nullptr)},
      {"move", std::move(move)},
      {"score", record.score},
      {"scores", Pair(game_.score(0) - game_.end_adjustment(0),
                      game_.score(1) - game_.end_adjustment(1))},
      {"bag", game_.bag().Size()},
      {"board", BoardToJson(game_.board())},
      {"racks", std::move(racks)},
      {"toMove", game_.IsOver() ? Json(nullptr) : Json(game_.to_move())}};
  if (by_human) event["drawn"] = record.drawn.ToString();
  return event;
}

Json HumanGame::ReviewEntryLocked(const PlayerView& view, const Move& played) {
  std::vector<Move> ranked;
  analyst_.RankMoves(view, std::numeric_limits<std::size_t>::max(), ranked);

  // The played move's equity under the same evaluator as the ranking. It is
  // computed directly rather than looked up, because the generator lists a
  // single-tile play in one direction only.
  StaticEvaluator evaluator(*LeavesFor(LeaveKind::kLearned));
  int unseen_value = 0;
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    unseen_value += view.unseen[letter] * kTileValues[letter];
  }
  evaluator.Prepare(view.rack, view.bag_size == 0, unseen_value);
  const double equity = evaluator.Equity(played);

  int rank = 1;
  for (const Move& move : ranked) {
    if (move.equity > equity + 1e-9) ++rank;
  }
  Json entry = {{"turn", view.turn},
                {"notation", played.ToString()},
                {"score", played.score},
                {"equity", Round2(equity)},
                {"rank", rank},
                {"totalMoves", ranked.size()}};
  if (!ranked.empty()) {
    const Move& best = ranked.front();
    entry["best"] = StaticCandidateJson(best, view.rack, 0, false);
    entry["equityLost"] = Round2(std::max(0.0, best.equity - equity));
  }
  return entry;
}

ApiResponse HumanGame::Play(const Json& request, Clock::time_point now) {
  const std::lock_guard lock(mutex_);
  last_active_ = now;
  if (finished_) return Conflict("game_over", "the game is over");

  if (BoolField(request, "resign", false, "resign")) {
    FinishLocked("resigned");
    return {200, {{"gameOver", true}, {"reason", "resigned"}}};
  }
  if (thinking_ || game_.to_move() != human_seat_) {
    return Conflict("not_your_turn", "wait for the bot to move");
  }
  const Json* notation = Field(request, "move");
  if (notation == nullptr || !notation->is_string()) {
    throw ApiError(400, "missing_field",
                   "\"move\" (a notation string) or \"resign\" is required");
  }
  const Move move = MoveFromNotation(notation->get_ref<const std::string&>());

  // The review judges the move in the position it was played from. A copy
  // of the game is a few kilobytes, far cheaper than ranking the moves of
  // a rejected attempt.
  const Game before = game_;
  const PlayResult result = game_.Play(move);
  switch (result.error) {
    case PlayError::kOk:
      break;
    case PlayError::kIllegalMove: {
      // Game::Play reports only the error code; validate again against the
      // pre-move position to name the rejected word.
      std::string invalid_word;
      Move retry = move;
      ValidateMove(before.board(), before.lexicon(), before.rack(human_seat_),
                   retry, &invalid_word);
      return ErrorResponse(422, MoveErrorCode(result.move_error),
                           DescribeMoveError(result.move_error, invalid_word));
    }
    case PlayError::kExchangeNotAllowed:
      return ErrorResponse(422, "exchange_not_allowed",
                           PlayErrorMessage(result));
    case PlayError::kGameOver:
      return Conflict("game_over", "the game is over");
  }

  const MoveRecord& record = game_.history().back();
  // The canonical move: play-through letters resolved and score set.
  review_.push_back(ReviewEntryLocked(before.View(human_seat_), record.move));
  session_->Publish("move", MoveEventLocked(record));

  Json response = {{"turn", game_.turn() - 1},
                   {"move", MoveToJson(record.move, &record.rack_before)},
                   {"score", record.score},
                   {"rack", game_.rack(human_seat_).ToString()},
                   {"drawn", record.drawn.ToString()},
                   {"bag", game_.bag().Size()},
                   {"gameOver", game_.IsOver()}};
  if (game_.IsOver()) {
    FinishLocked(game_.status() == GameStatus::kWentOut ? "went_out"
                                                        : "scoreless_turns");
  } else {
    StartBotLocked();
  }
  response["scores"] = Pair(game_.score(0), game_.score(1));
  return {200, std::move(response)};
}

ApiResponse HumanGame::Hint(Clock::time_point now) {
  const std::lock_guard lock(mutex_);
  last_active_ = now;
  if (finished_) return Conflict("game_over", "the game is over");
  if (thinking_ || game_.to_move() != human_seat_) {
    return Conflict("not_your_turn", "hints are for your own turn");
  }
  const PlayerView view = game_.View(human_seat_);
  std::vector<Move> ranked;
  analyst_.RankMoves(view, std::numeric_limits<std::size_t>::max(), ranked);
  ++hints_;
  return {200, AnalysisJson(ranked, view.rack, ranked.front(), kHintCandidates,
                            nullptr, 0, nullptr)};
}

void HumanGame::Abort(std::string_view reason) {
  const std::lock_guard lock(mutex_);
  if (!finished_) FinishLocked(reason);
}

void HumanGame::StartBotLocked() {
  thinking_ = true;
  // The previous reply's thread has published its move and is exiting (it
  // never takes the lock again), so this join returns at once.
  if (worker_.joinable()) worker_.join();
  worker_ = std::thread([this] { RunBot(); });
}

void HumanGame::RunBot() {
  if (!slots_->Acquire(stop_)) return;
  Move move;
  std::string error;
  double elapsed_ms = 0.0;
  {
    const SlotGuard guard(*slots_);
    // A snapshot, so requests (hints, resignation) are served while the
    // bot thinks; nothing else changes the game until it replies.
    Game snapshot = [this] {
      const std::lock_guard lock(mutex_);
      return game_;
    }();
    const auto begin = std::chrono::steady_clock::now();
    try {
      move = bot_.ChooseMove(snapshot.ViewForMover(), rng_);
    } catch (const std::exception& e) {
      error = e.what();
    }
    elapsed_ms = Elapsed(begin);
  }

  const std::lock_guard lock(mutex_);
  if (stop_.load() || finished_) return;
  thinking_ = false;
  const PlayResult result =
      error.empty() ? game_.Play(move) : PlayResult{PlayError::kIllegalMove};
  if (!result.ok()) {
    session_->Publish("end",
                      {{"reason", "error"},
                       {"message", error.empty()
                                       ? "the bot chose an illegal move " +
                                             move.ToString()
                                       : error}});
    finished_ = true;
    session_->Close();
    return;
  }
  Json event = MoveEventLocked(game_.history().back());
  event["elapsedMs"] = Round2(elapsed_ms);
  session_->Publish("move", std::move(event));
  if (game_.IsOver()) {
    FinishLocked(game_.status() == GameStatus::kWentOut ? "went_out"
                                                        : "scoreless_turns");
  }
}

void HumanGame::FinishLocked(std::string_view reason) {
  finished_ = true;
  thinking_ = false;
  const bool aborted = reason == "cancelled" || reason == "abandoned";
  if (aborted) session_->RequestCancel();

  GcgInfo info;
  info.players[static_cast<std::size_t>(human_seat_)] = {settings_.human_name,
                                                         ""};
  info.players[static_cast<std::size_t>(bot_seat())] = {kBotName, ""};
  info.title = std::string("Human vs bot (") +
               std::string(BotStrengthName(settings_.strength)) + ")";
  std::string gcg = ToGcg(game_, info);
  session_->set_gcg(gcg);

  Json winner;
  if (aborted) {
    winner = nullptr;
  } else if (reason == "resigned") {
    winner = bot_seat();
  } else {
    const int spread = game_.spread(0);
    winner = spread > 0 ? 0 : spread < 0 ? 1 : -1;
  }
  session_->Publish(
      "end",
      {{"reason", reason},
       {"scores", Pair(game_.score(0), game_.score(1))},
       {"adjustments", Pair(game_.end_adjustment(0), game_.end_adjustment(1))},
       {"racks", {game_.rack(0).ToString(), game_.rack(1).ToString()}},
       {"winner", std::move(winner)},
       {"turns", game_.turn()},
       {"seed", settings_.seed},
       {"humanSeat", human_seat_},
       {"hintsUsed", hints_},
       {"review", review_},
       {"gcg", std::move(gcg)}});
  session_->Close();
}

// ---------------------------------------------------------------------------
// HumanGames

HumanGames::HumanGames(const Gaddag& gaddag, GameRegistry& registry,
                       HumanGamesOptions options,
                       std::function<Clock::time_point()> now)
    : gaddag_(&gaddag),
      registry_(&registry),
      options_(options),
      now_(std::move(now)),
      slots_(options.max_thinking) {}

HumanGames::~HumanGames() {
  Map games;
  {
    const std::lock_guard lock(mutex_);
    games.swap(games_);
  }
  // Destroying each game joins its bot thread.
}

ApiResponse HumanGames::Start(const Json& request,
                              std::uint64_t default_seed) {
  const HumanGameSettings settings =
      HumanGameSettingsFromJson(request, default_seed);
  std::vector<std::shared_ptr<HumanGame>> expired;
  std::shared_ptr<HumanGame> game;
  {
    const std::lock_guard lock(mutex_);
    SweepLocked(expired);
    const auto open = static_cast<std::size_t>(std::count_if(
        games_.begin(), games_.end(),
        [](const auto& entry) { return !entry.second->finished(); }));
    std::shared_ptr<GameSession> session =
        open < options_.max_open ? registry_->AddInteractive("human")
                                 : nullptr;
    if (session == nullptr) {
      return ErrorResponse(503, "too_many_games",
                           "the server is already hosting the maximum number "
                           "of games; finish or resign one first");
    }
    game = std::make_shared<HumanGame>(*gaddag_, settings, session, slots_,
                                       now_());
    games_.emplace(session->id(), game);
  }
  game->Begin();
  Json response = {{"id", game->id()},
                   {"mode", "human"},
                   {"events", "/api/games/" + game->id() + "/events"},
                   {"humanSeat", settings.human_first ? 0 : 1}};
  if (settings.seed_given) response["seed"] = settings.seed;
  return {201, std::move(response)};
}

std::shared_ptr<HumanGame> HumanGames::Find(std::string_view id) {
  std::vector<std::shared_ptr<HumanGame>> expired;
  std::shared_ptr<HumanGame> game;
  {
    const std::lock_guard lock(mutex_);
    SweepLocked(expired);
    const auto it = games_.find(id);
    if (it != games_.end()) game = it->second;
  }
  return game;
}

ApiResponse HumanGames::Play(std::string_view id, std::string_view body) {
  return GuardedResponse([&]() -> ApiResponse {
    const Json request = ParseRequestObject(body);
    const std::shared_ptr<HumanGame> game = Find(id);
    if (game == nullptr) {
      return ErrorResponse(404, "unknown_game",
                           "no human game " + std::string(id) +
                               " (it may have expired)");
    }
    return game->Play(request, now_());
  });
}

ApiResponse HumanGames::Hint(std::string_view id) {
  return GuardedResponse([&]() -> ApiResponse {
    const std::shared_ptr<HumanGame> game = Find(id);
    if (game == nullptr) {
      return ErrorResponse(404, "unknown_game",
                           "no human game " + std::string(id) +
                               " (it may have expired)");
    }
    return game->Hint(now_());
  });
}

bool HumanGames::Cancel(std::string_view id) {
  const std::shared_ptr<HumanGame> game = Find(id);
  if (game == nullptr) return false;
  game->Abort("cancelled");
  return true;
}

void HumanGames::Sweep() {
  std::vector<std::shared_ptr<HumanGame>> expired;
  const std::lock_guard lock(mutex_);
  SweepLocked(expired);
}

std::size_t HumanGames::size() const {
  const std::lock_guard lock(mutex_);
  return games_.size();
}

void HumanGames::SweepLocked(std::vector<std::shared_ptr<HumanGame>>& expired) {
  const Clock::time_point now = now_();
  for (auto it = games_.begin(); it != games_.end();) {
    HumanGame& game = *it->second;
    if (!registry_->Contains(it->first)) {
      // The registry expired or evicted its session: nobody can reach it.
      game.Abort("abandoned");
      expired.push_back(std::move(it->second));
      it = games_.erase(it);
      continue;
    }
    if (!game.finished() && game.Idle(now, options_.idle_ttl)) {
      game.Abort("abandoned");
    }
    ++it;
  }
}

}  // namespace scrabble::server
