// In-memory registry of streamed games (self-play and GCG replays).
//
// A game is produced once and may be watched many times: the producer
// appends events to a GameSession, and every Server-Sent Events connection
// reads that append-only log from its own cursor. Readers therefore never
// block the producer, a reconnecting client resumes from Last-Event-ID by
// simply restarting its cursor, and a finished game can be replayed from
// the start until it expires.
//
// Threading model:
//  * Self-play games run on one dedicated std::thread each, started by
//    GameRegistry::StartBackground() and bounded by
//    RegistryOptions::max_running. HTTP handler threads never run the
//    engine; they only read event logs.
//  * GameSession is internally synchronised (a mutex plus a condition
//    variable that wakes readers when events arrive). Cancellation is a
//    separate atomic flag the producer polls between moves, so a cancel
//    request never waits on the engine.
//  * GameRegistry guards its map with its own mutex. Sessions are held by
//    shared_ptr, so a stream that is still being read keeps its session
//    alive after the registry has evicted it.
//
// Lifetime: finished games are kept for finished_ttl after they finish (for
// reconnects and GCG download); a running game that has had no reader for
// idle_ttl is cancelled, since nobody is watching it. Housekeeping is lazy:
// Sweep() runs on every registry call, so no janitor thread is needed.

#ifndef SCRABBLE_SERVER_GAME_REGISTRY_H_
#define SCRABBLE_SERVER_GAME_REGISTRY_H_

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
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace scrabble::server {

// Renders one Server-Sent Events frame:
//
//   id: <id>
//   event: <type>
//   data: <json>
//   <blank line>
//
// `data` is serialised compactly, so it never contains a newline and fits
// on a single "data:" line.
std::string SseFrame(std::size_t id, std::string_view type,
                     const nlohmann::json& data);

// The event log of one game. Thread-safe.
class GameSession {
 public:
  enum class ReadResult : std::uint8_t {
    kData,     // `out` received one or more frames
    kTimeout,  // nothing new within the timeout; the game is still going
    kEnd,      // closed, and the cursor is past the last frame
  };

  GameSession(std::string id, std::string mode);

  GameSession(const GameSession&) = delete;
  GameSession& operator=(const GameSession&) = delete;

  const std::string& id() const { return id_; }
  // "selfplay" or "replay".
  const std::string& mode() const { return mode_; }

  // Appends an event. `data` must be a JSON object; its "type" field is set
  // to `type`. The event's SSE id is its zero-based index in the log.
  // Ignored once the session is closed.
  void Publish(std::string_view type, nlohmann::json data);

  // Marks the log complete and wakes every reader. Idempotent.
  void Close();
  bool closed() const;

  // Asks the producer to stop at its next opportunity. The producer is
  // still expected to publish a final event and Close().
  void RequestCancel() { cancel_requested_.store(true); }
  bool cancel_requested() const { return cancel_requested_.load(); }

  // Appends the frames at indices [from, size()) to `out`, waiting up to
  // `timeout` for one to arrive if there are none yet.
  ReadResult Read(std::size_t from, std::chrono::milliseconds timeout,
                  std::vector<std::string>& out) const;

  // Number of events published so far.
  std::size_t size() const;

  // The game record, once known (set by the producer before closing).
  void set_gcg(std::string gcg);
  std::string gcg() const;

  // Open SSE connections; maintained by the HTTP layer for idle detection.
  void AddReader() { readers_.fetch_add(1); }
  void RemoveReader() { readers_.fetch_sub(1); }
  int readers() const { return readers_.load(); }

  // {"id", "mode", "status": "running" | "finished" | "cancelled",
  //  "events": n}
  nlohmann::json Summary() const;

 private:
  const std::string id_;
  const std::string mode_;
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::vector<std::string> frames_;  // guarded by mutex_
  std::string gcg_;                  // guarded by mutex_
  bool closed_ = false;              // guarded by mutex_
  std::atomic<bool> cancel_requested_{false};
  std::atomic<int> readers_{0};
};

struct RegistryOptions {
  // Games held in memory, running or finished. When full, the oldest
  // finished game is evicted to make room.
  std::size_t max_games = 32;
  // Self-play games computing at once. Each uses up to
  // kSimulationThreadsPerGame cores (see games.h).
  std::size_t max_running = 2;
  // How long a finished game stays available.
  std::chrono::seconds finished_ttl{15 * 60};
  // A running game with no connected reader for this long is cancelled.
  std::chrono::seconds idle_ttl{2 * 60};
};

class GameRegistry {
 public:
  using Clock = std::chrono::steady_clock;
  // Produces a game's events. Must publish its own final event; the
  // registry closes the session when the task returns (or throws).
  using Task = std::function<void(GameSession&)>;

  // `now` is injectable so tests can drive expiry without sleeping.
  explicit GameRegistry(RegistryOptions options = {},
                        std::function<Clock::time_point()> now = Clock::now);
  // Cancels running games and joins their threads.
  ~GameRegistry();

  GameRegistry(const GameRegistry&) = delete;
  GameRegistry& operator=(const GameRegistry&) = delete;

  // Runs `task` on a new thread. Returns null if max_running games are
  // already running.
  std::shared_ptr<GameSession> StartBackground(std::string mode, Task task);

  // Runs `task` to completion on the calling thread, for games whose events
  // are computed up front (replays). Returns null only if the registry is
  // full of running games.
  std::shared_ptr<GameSession> AddCompleted(std::string mode,
                                            const Task& task);

  // Null if `id` is unknown or has expired.
  std::shared_ptr<GameSession> Find(std::string_view id);

  // Requests cancellation of a running game; the session stays available
  // until it expires, so its final "end" event can still be read. Returns
  // false if `id` is unknown.
  bool Cancel(std::string_view id);

  // Expires finished games and cancels unwatched running ones. Called by
  // every other method; public for tests.
  void Sweep();

  std::size_t size() const;
  std::size_t running() const;

 private:
  struct Entry {
    std::shared_ptr<GameSession> session;
    std::thread worker;  // not joinable for AddCompleted() sessions
    std::uint64_t sequence = 0;  // creation order, for eviction
    Clock::time_point last_watched;
    std::optional<Clock::time_point> finished_at;  // when first seen closed
  };
  using Map = std::map<std::string, Entry, std::less<>>;

  // Both require mutex_ held.
  void SweepLocked(std::vector<std::thread>& finished);
  bool MakeRoomLocked(std::vector<std::thread>& finished);
  std::size_t RunningLocked() const;
  std::string NewIdLocked();
  Map::iterator InsertLocked(std::string mode);

  // Joins threads collected while the lock was held. They belong to closed
  // sessions, so each join returns as soon as the task's last statement
  // has run.
  static void Join(std::vector<std::thread>& threads);

  const RegistryOptions options_;
  const std::function<Clock::time_point()> now_;
  mutable std::mutex mutex_;
  Map games_;                 // guarded by mutex_
  std::uint64_t sequence_ = 0;  // guarded by mutex_
  std::mt19937_64 id_source_;   // guarded by mutex_
};

}  // namespace scrabble::server

#endif  // SCRABBLE_SERVER_GAME_REGISTRY_H_
