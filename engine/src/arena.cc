#include "scrabble/arena.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "scrabble/gcg.h"

namespace scrabble {
namespace {

constexpr double kZ95 = 1.96;

// Writes per-game chunks to a stream strictly in game order although games
// finish out of order. A chunk waits in `pending_` until every earlier game
// has been written; with work handed out in index order, at most about one
// chunk per thread is ever buffered.
class OrderedWriter {
 public:
  explicit OrderedWriter(std::ostream* out) : out_(out) {}

  void Submit(int index, std::string chunk) {
    if (out_ == nullptr) return;
    const std::lock_guard<std::mutex> lock(mutex_);
    pending_.emplace(index, std::move(chunk));
    for (auto it = pending_.begin();
         it != pending_.end() && it->first == next_;
         it = pending_.erase(it), ++next_) {
      *out_ << it->second;
    }
  }

 private:
  std::ostream* out_;
  std::mutex mutex_;
  std::map<int, std::string> pending_;
  int next_ = 0;
};

std::string_view TypeName(MoveType type) {
  switch (type) {
    case MoveType::kPass: return "pass";
    case MoveType::kPlace: return "place";
    case MoveType::kExchange: return "exchange";
  }
  return "unknown";
}

// Appends the JSONL lines for a finished game. No string escaping is
// needed: every value is drawn from [A-Za-z0-9?()+-. ].
void AppendLog(const Game& game, int index, std::uint64_t seed, int a_seat,
               std::string& out) {
  std::array<int, kNumPlayers> running{};
  char buffer[512];
  for (std::size_t turn = 0; turn < game.history().size(); ++turn) {
    const MoveRecord& record = game.history()[turn];
    const int player = record.player;
    const int opponent = 1 - player;
    const std::string rack = record.rack_before.ToString();
    const std::string move = record.move.ToString();
    const std::string leave =
        LeaveAfter(record.rack_before, record.move).ToString();
    const int written = std::snprintf(
        buffer, sizeof(buffer),
        "{\"game\":%d,\"seed\":%llu,\"turn\":%zu,\"player\":%d,"
        "\"bot\":\"%c\",\"rack\":\"%s\",\"move\":\"%s\",\"type\":\"%s\","
        "\"score\":%d,\"equity\":%.3f,\"leave\":\"%s\",\"bag\":%d,"
        "\"spread\":%d,\"final_spread\":%d}\n",
        index, static_cast<unsigned long long>(seed), turn, player,
        player == a_seat ? 'A' : 'B', rack.c_str(), move.c_str(),
        TypeName(record.move.type).data(), record.score, record.move.equity,
        leave.c_str(), record.bag_before,
        running[static_cast<std::size_t>(player)] -
            running[static_cast<std::size_t>(opponent)],
        game.spread(player));
    // Every field is bounded (a move is at most ~20 characters), so the
    // buffer cannot overflow; check anyway rather than emit a torn line.
    if (written < 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
      throw std::logic_error("arena log line overflow");
    }
    out.append(buffer, static_cast<std::size_t>(written));
    running[static_cast<std::size_t>(player)] = record.cumulative;
  }
}

void WriteGcg(const Game& game, const ArenaConfig& config, int index,
              int a_seat) {
  GcgInfo info;
  const std::array<const BotConfig*, 2> by_config = {&config.bot_a,
                                                     &config.bot_b};
  for (int seat = 0; seat < kNumPlayers; ++seat) {
    const int which = seat == a_seat ? 0 : 1;
    GcgPlayer& player = info.players[static_cast<std::size_t>(seat)];
    player.nickname = which == 0 ? "A" : "B";
    player.full_name = player.nickname + " (" +
                       by_config[static_cast<std::size_t>(which)]->name + ")";
  }
  info.title = "arena seed " + std::to_string(config.seed) + " game " +
               std::to_string(index);
  char name[32];
  std::snprintf(name, sizeof(name), "game_%06d.gcg", index);
  const std::filesystem::path path = config.gcg_dir / name;
  std::ofstream out(path);
  out << ToGcg(game, info);
  if (!out) throw std::runtime_error("cannot write " + path.string());
}

// The per-thread worker: claims game indices until none are left.
class Worker {
 public:
  Worker(const Gaddag& gaddag, const ArenaConfig& config)
      : config_(config),
        game_(gaddag, 0, config.game),
        bot_a_(gaddag, config.bot_a),
        bot_b_(gaddag, config.bot_b) {}

  ArenaGame Play(int index, std::string& log) {
    const std::uint64_t seed = ArenaGameSeed(config_.seed, index);
    game_.Reset(seed);
    // A distinct stream for the bots, so bot randomness (simulation, once
    // it exists) cannot perturb the tile draws.
    bot_rng_.Seed(~seed);
    const int a_seat = index % 2;
    const std::array<Bot*, 2> by_seat = {a_seat == 0 ? &bot_a_ : &bot_b_,
                                         a_seat == 0 ? &bot_b_ : &bot_a_};
    while (!game_.IsOver()) {
      const Move move = by_seat[static_cast<std::size_t>(game_.to_move())]
                            ->ChooseMove(game_.ViewForMover(), bot_rng_);
      const PlayResult result = game_.Play(move);
      if (!result.ok()) {
        throw std::logic_error("bot chose an illegal move " +
                               move.ToString() + ": " +
                               std::string(PlayErrorMessage(result)));
      }
    }

    ArenaGame outcome;
    outcome.index = index;
    outcome.a_seat = a_seat;
    outcome.score = {game_.score(a_seat), game_.score(1 - a_seat)};
    for (const MoveRecord& record : game_.history()) {
      if (record.move.type == MoveType::kPlace &&
          record.move.TilesUsed() == kRackSize) {
        ++outcome.bingos[record.player == a_seat ? 0 : 1];
      }
    }
    outcome.turns = game_.turn();
    outcome.end = game_.status();

    log.clear();
    if (config_.log != nullptr) {
      AppendLog(game_, index, config_.seed, a_seat, log);
    }
    if (!config_.gcg_dir.empty()) WriteGcg(game_, config_, index, a_seat);
    return outcome;
  }

 private:
  const ArenaConfig& config_;
  Game game_;
  Bot bot_a_;
  Bot bot_b_;
  Rng bot_rng_{0};
};

double Mean(double sum, int n) { return n > 0 ? sum / n : 0.0; }

// Half-width of the 95% interval for a mean, from the sample variance.
double MeanCi(double sum, double sum_squares, int n) {
  if (n < 2) return 0.0;
  const double mean = sum / n;
  const double variance =
      std::max(0.0, (sum_squares - n * mean * mean) / (n - 1));
  return kZ95 * std::sqrt(variance / n);
}

}  // namespace

std::uint64_t ArenaGameSeed(std::uint64_t seed, int index) {
  std::uint64_t z = seed + 0x9E3779B97F4A7C15ULL *
                               (static_cast<std::uint64_t>(index) + 1);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

ArenaResult RunArena(const Gaddag& gaddag, const ArenaConfig& config) {
  const auto start = std::chrono::steady_clock::now();
  const int games = std::max(config.games, 0);
  const int threads = std::clamp(config.threads, 1, std::max(games, 1));
  if (!config.gcg_dir.empty()) {
    std::filesystem::create_directories(config.gcg_dir);
  }

  ArenaResult result;
  result.games.resize(static_cast<std::size_t>(games));
  OrderedWriter log(config.log);
  std::atomic<int> next{0};
  std::atomic<bool> failed{false};
  std::mutex error_mutex;
  std::exception_ptr error;

  const auto work = [&] {
    try {
      Worker worker(gaddag, config);
      std::string chunk;
      for (int index = next.fetch_add(1, std::memory_order_relaxed);
           index < games && !failed.load(std::memory_order_relaxed);
           index = next.fetch_add(1, std::memory_order_relaxed)) {
        // Each slot is written by exactly one thread and read only after
        // join(), so the vector needs no lock.
        result.games[static_cast<std::size_t>(index)] =
            worker.Play(index, chunk);
        log.Submit(index, std::move(chunk));
      }
    } catch (...) {
      const std::lock_guard<std::mutex> lock(error_mutex);
      if (!error) error = std::current_exception();
      failed.store(true, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> pool;
  pool.reserve(static_cast<std::size_t>(threads - 1));
  for (int i = 1; i < threads; ++i) pool.emplace_back(work);
  work();  // the calling thread is worker 0
  for (std::thread& thread : pool) thread.join();
  if (error) std::rethrow_exception(error);

  result.summary = Summarize(result.games);
  result.seconds = std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - start)
                       .count();
  return result;
}

ArenaSummary Summarize(std::span<const ArenaGame> games) {
  ArenaSummary summary;
  const int n = static_cast<int>(games.size());
  summary.games = n;
  if (n == 0) return summary;
  double score_a = 0, score_a_sq = 0, score_b = 0, score_b_sq = 0;
  double spread = 0, spread_sq = 0;
  double bingos_a = 0, bingos_b = 0, turns = 0;
  for (const ArenaGame& game : games) {
    const int s = game.spread();
    summary.wins += s > 0;
    summary.losses += s < 0;
    summary.ties += s == 0;
    score_a += game.score[0];
    score_a_sq += double{1.0} * game.score[0] * game.score[0];
    score_b += game.score[1];
    score_b_sq += double{1.0} * game.score[1] * game.score[1];
    spread += s;
    spread_sq += double{1.0} * s * s;
    bingos_a += game.bingos[0];
    bingos_b += game.bingos[1];
    turns += game.turns;
  }
  summary.win_rate = (summary.wins + 0.5 * summary.ties) / n;
  summary.win_rate_ci =
      kZ95 * std::sqrt(summary.win_rate * (1.0 - summary.win_rate) / n);
  summary.mean_score_a = Mean(score_a, n);
  summary.mean_score_a_ci = MeanCi(score_a, score_a_sq, n);
  summary.mean_score_b = Mean(score_b, n);
  summary.mean_score_b_ci = MeanCi(score_b, score_b_sq, n);
  summary.mean_spread = Mean(spread, n);
  summary.mean_spread_ci = MeanCi(spread, spread_sq, n);
  summary.bingos_per_game_a = Mean(bingos_a, n);
  summary.bingos_per_game_b = Mean(bingos_b, n);
  summary.turns_per_game = Mean(turns, n);
  return summary;
}

}  // namespace scrabble
