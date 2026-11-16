// Human-vs-bot games: the server is the referee, the client only proposes
// moves.
//
// The authoritative Game (board, both racks, bag) lives here, and the
// browser only ever learns what a seated human player may know. The event
// stream (the same GameSession log and SSE endpoint as the game viewer;
// see games.h) therefore never carries the bot's rack:
//
//   * "start" and every "move" give the human's rack only; the bot's seat
//     is null in "racks".
//   * A bot move omits its rack, leave, tiles drawn and analysis, and an
//     exchange is shown by tile count only ("-3"), as an opponent would see
//     it across the table.
//   * A random seed is not revealed until the game ends, since the seed
//     determines the bag order. A seed the client chose is echoed.
//   * "end" reveals both racks, the GCG record (which lists every rack) and
//     the post-game review.
//
// Moves are validated by the engine (Game::Play()): a phony or impossible
// placement is rejected with 422 and the MoveError code, and the human
// tries again. There is no challenge rule, so the engine's lexicon is the
// final word and an illegal word never reaches the board.
//
// After each legal human move the bot replies on the game's own worker
// thread, so the POST returns at once and the bot's move arrives over SSE.
// At most HumanGamesOptions::max_thinking bots compute at once; the rest
// wait their turn. Bot strength is a preset (BotStrength) with server-side
// time limits, so a reply takes at most a few seconds.

#ifndef SCRABBLE_SERVER_HUMAN_GAME_H_
#define SCRABBLE_SERVER_HUMAN_GAME_H_

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "api.h"
#include "game_registry.h"
#include "scrabble/bot.h"
#include "scrabble/gaddag.h"
#include "scrabble/game.h"
#include "scrabble/rng.h"

namespace scrabble::server {

enum class BotStrength : std::uint8_t {
  kEasy,    // static, no leave values: always the top-scoring move
  kNormal,  // static equity with the learned leave table
  kHard,    // Monte Carlo simulation (10 candidates, <= 3 s per move)
  kExpert,  // larger simulation plus the exact endgame solver
};

std::string_view BotStrengthName(BotStrength strength);

// The bot configuration behind a preset.
BotConfig BotConfigFor(BotStrength strength);

struct HumanGameSettings {
  std::string human_name = "you";
  BotStrength strength = BotStrength::kNormal;
  bool human_first = true;
  std::uint64_t seed = 1;
  // Whether the client chose the seed (and so may see it during the game).
  bool seed_given = false;
  EndRules rules = EndRules::kNaspa;
};

// Parses the human-game fields of a POST /api/games body:
//
//   {"mode": "human", "name": "alice", "strength": "normal",
//    "humanFirst": true, "seed": 42, "rules": "naspa"}
//
// Every field is optional; `default_seed` is used when "seed" is absent.
// Throws ApiError (400, "bad_settings").
HumanGameSettings HumanGameSettingsFromJson(const nlohmann::json& request,
                                            std::uint64_t default_seed);

// One game. Thread-safe: request handlers and the bot's worker thread
// share it under one mutex, which is never held while the bot thinks.
class HumanGame {
 public:
  using Clock = std::chrono::steady_clock;

  // Limits bots thinking at once across games; shared by every HumanGame.
  class ThinkingSlots {
   public:
    explicit ThinkingSlots(int slots) : free_(slots) {}
    // Waits for a slot; false if `stop` became true first.
    bool Acquire(const std::atomic<bool>& stop);
    void Release();

   private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int free_;  // guarded by mutex_
  };

  // `gaddag` and `slots` must outlive the game. Call Begin() once.
  HumanGame(const Gaddag& gaddag, const HumanGameSettings& settings,
            std::shared_ptr<GameSession> session, ThinkingSlots& slots,
            Clock::time_point now);
  // Stops the bot (discarding a move in progress) and joins its thread.
  ~HumanGame();

  HumanGame(const HumanGame&) = delete;
  HumanGame& operator=(const HumanGame&) = delete;

  // Publishes "start" and, if the bot moves first, starts its reply.
  void Begin();

  // POST /api/games/{id}/moves: {"move": "8D RETAINS" | "-ABC" | "-"} or
  // {"resign": true}. 200 with the human's move, new rack and the scores;
  // 409 "not_your_turn" / "game_over"; 422 with a MoveError code (or
  // "exchange_not_allowed") for an illegal move; 400 for bad notation.
  ApiResponse Play(const nlohmann::json& request, Clock::time_point now);

  // GET /api/games/{id}/hint: the engine's top static moves for the
  // human's rack, as a MoveAnalysis (analysis.h). 409 unless it is the
  // human's turn. Counted, and reported in the end event.
  ApiResponse Hint(Clock::time_point now);

  // Ends the game without a result ("cancelled" or "abandoned"). No-op if
  // it has already ended.
  void Abort(std::string_view reason);

  bool finished() const;
  // True if nobody has made a request or watched the stream for `ttl`.
  bool Idle(Clock::time_point now, Clock::duration ttl);
  const std::string& id() const { return session_->id(); }

 private:
  int bot_seat() const { return 1 - human_seat_; }

  // All require mutex_ held.
  nlohmann::json StartEventLocked() const;
  nlohmann::json MoveEventLocked(const MoveRecord& record) const;
  // The human's move judged against the engine's best static move, for the
  // post-game review.
  nlohmann::json ReviewEntryLocked(const PlayerView& view, const Move& played);
  void StartBotLocked();
  void FinishLocked(std::string_view reason);

  // Body of the worker thread: choose the bot's move on a snapshot of the
  // game (without the lock), then apply it.
  void RunBot();

  const HumanGameSettings settings_;
  const int human_seat_;
  const std::shared_ptr<GameSession> session_;
  ThinkingSlots* slots_;

  mutable std::mutex mutex_;
  Game game_;                          // guarded by mutex_
  Bot analyst_;                        // guarded by mutex_; hints, review
  std::vector<nlohmann::json> review_;  // guarded by mutex_
  int hints_ = 0;                      // guarded by mutex_
  bool thinking_ = false;              // guarded by mutex_
  bool finished_ = false;              // guarded by mutex_
  Clock::time_point last_active_;      // guarded by mutex_
  std::thread worker_;                 // guarded by mutex_

  // Touched only by the worker thread (one reply at a time).
  Bot bot_;
  Rng rng_;
  std::atomic<bool> stop_{false};
};

struct HumanGamesOptions {
  // Human games in progress at once.
  std::size_t max_open = 8;
  // Bots computing a reply at once; each uses up to two simulation
  // threads (Hard and Expert).
  int max_thinking = 2;
  // A game nobody has touched or watched for this long is abandoned.
  std::chrono::seconds idle_ttl{30 * 60};
};

// The human games of one server. Thread-safe.
class HumanGames {
 public:
  using Clock = HumanGame::Clock;

  // `gaddag` and `registry` must outlive this object, which joins every
  // bot thread when destroyed. `now` is injectable for tests.
  HumanGames(const Gaddag& gaddag, GameRegistry& registry,
             HumanGamesOptions options = {},
             std::function<Clock::time_point()> now = Clock::now);
  ~HumanGames();

  HumanGames(const HumanGames&) = delete;
  HumanGames& operator=(const HumanGames&) = delete;

  // POST /api/games with "mode": "human". 201 {"id", "mode", "events",
  // "humanSeat", "seed"?}; 503 "too_many_games" when full.
  ApiResponse Start(const nlohmann::json& request, std::uint64_t default_seed);
  ApiResponse Play(std::string_view id, std::string_view body);
  ApiResponse Hint(std::string_view id);
  // Ends a human game; false if `id` is not one.
  bool Cancel(std::string_view id);

  // Abandons idle games and forgets ones the registry has expired. Called
  // by every other method; public for tests.
  void Sweep();
  std::size_t size() const;

 private:
  using Map = std::map<std::string, std::shared_ptr<HumanGame>, std::less<>>;

  std::shared_ptr<HumanGame> Find(std::string_view id);
  // Requires mutex_ held. Moves expired games into `expired`, so they are
  // destroyed (joining their threads) after the lock is released.
  void SweepLocked(std::vector<std::shared_ptr<HumanGame>>& expired);

  const Gaddag* gaddag_;
  GameRegistry* registry_;
  const HumanGamesOptions options_;
  const std::function<Clock::time_point()> now_;
  HumanGame::ThinkingSlots slots_;
  mutable std::mutex mutex_;
  Map games_;  // guarded by mutex_
};

}  // namespace scrabble::server

#endif  // SCRABBLE_SERVER_HUMAN_GAME_H_
