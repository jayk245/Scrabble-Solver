// Endgame solver benchmark on real endgame positions.
//
// Usage: scrabble_endgame_bench [--positions N] [--seed S] [--lexicon PATH]
//                               [--nodes N] [--ms T] [--tt-mb M]
//                               [--max-tiles K] [--rules naspa|wespa]
//
// Positions come from seeded self-play between static bots: game i (seed
// S + i) is played until the bag empties, and the position the next player
// faces is solved. --max-tiles K keeps a random K-tile subset of each rack
// (to study how cost scales with rack size); by default racks are as dealt.
// Each solve gets a fresh-generation table and the given node/time limits;
// a solve is "exact" when it finished without hitting a limit.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "scrabble/bot.h"
#include "scrabble/endgame.h"
#include "scrabble/gaddag.h"
#include "scrabble/game.h"
#include "scrabble/lexicon.h"
#include "scrabble/position.h"
#include "scrabble/rack.h"
#include "scrabble/rng.h"

namespace {

struct Options {
  int positions = 100;
  std::uint64_t seed = 1;
  std::filesystem::path lexicon =
      std::filesystem::path(SCRABBLE_LEXICA_DIR) / "enable1.txt";
  std::int64_t nodes = 5'000'000;
  std::int64_t ms = 10'000;
  std::size_t tt_mb = 64;
  int max_tiles = scrabble::kRackSize;
  scrabble::EndRules rules = scrabble::EndRules::kNaspa;
};

constexpr std::string_view kUsage =
    " [--positions N] [--seed S] [--lexicon PATH] [--nodes N] [--ms T]\n"
    "    [--tt-mb M] [--max-tiles K] [--rules naspa|wespa]\n";

bool ParseInt(const char* text, std::int64_t min, std::int64_t& out) {
  char* end = nullptr;
  const long long value = std::strtoll(text, &end, 10);
  if (*end != '\0' || value < min) return false;
  out = value;
  return true;
}

bool ParseArgs(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (i + 1 >= argc) return false;
    const char* value = argv[++i];
    std::int64_t n = 0;
    if (arg == "--positions") {
      if (!ParseInt(value, 1, n)) return false;
      options.positions = static_cast<int>(n);
    } else if (arg == "--seed") {
      if (!ParseInt(value, 0, n)) return false;
      options.seed = static_cast<std::uint64_t>(n);
    } else if (arg == "--lexicon") {
      options.lexicon = value;
    } else if (arg == "--nodes") {
      if (!ParseInt(value, 0, options.nodes)) return false;
    } else if (arg == "--ms") {
      if (!ParseInt(value, 0, options.ms)) return false;
    } else if (arg == "--tt-mb") {
      if (!ParseInt(value, 0, n)) return false;
      options.tt_mb = static_cast<std::size_t>(n);
    } else if (arg == "--max-tiles") {
      if (!ParseInt(value, 1, n) || n > scrabble::kRackSize) return false;
      options.max_tiles = static_cast<int>(n);
    } else if (arg == "--rules") {
      const std::string_view rules = value;
      if (rules == "naspa") {
        options.rules = scrabble::EndRules::kNaspa;
      } else if (rules == "wespa") {
        options.rules = scrabble::EndRules::kWespa;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

struct Endgame {
  scrabble::Board board;
  scrabble::EndgameState state;
};

scrabble::Rack Truncate(const scrabble::Rack& rack, int max_tiles,
                        scrabble::Rng& rng) {
  std::vector<scrabble::Letter> tiles;
  for (scrabble::Letter l = 0; l < scrabble::kAlphabetSize; ++l) {
    for (int n = 0; n < rack.Count(l); ++n) tiles.push_back(l);
  }
  for (std::size_t i = tiles.size(); i > 1; --i) {
    std::swap(tiles[i - 1], tiles[rng.Below(static_cast<std::uint32_t>(i))]);
  }
  scrabble::Rack out;
  for (std::size_t i = 0;
       i < tiles.size() && i < static_cast<std::size_t>(max_tiles); ++i) {
    out.Add(tiles[i]);
  }
  return out;
}

std::vector<Endgame> Collect(const scrabble::Gaddag& gaddag,
                             const Options& options) {
  std::vector<Endgame> endgames;
  scrabble::Bot bot(gaddag, scrabble::BotConfig{});
  scrabble::Rng rng(options.seed);
  const scrabble::GameConfig config{.end_rules = options.rules};
  for (std::uint64_t seed = options.seed;
       static_cast<int>(endgames.size()) < options.positions; ++seed) {
    scrabble::Game game(gaddag, seed, config);
    while (!game.IsOver() && game.bag().Size() > 0) {
      game.Play(bot.ChooseMove(game.ViewForMover(), rng));
    }
    if (game.IsOver()) continue;  // ended before the bag emptied
    Endgame& endgame = endgames.emplace_back();
    endgame.board = game.board();
    for (int p = 0; p < scrabble::kNumPlayers; ++p) {
      endgame.state.racks[static_cast<std::size_t>(p)] =
          Truncate(game.rack(p), options.max_tiles, rng);
    }
    endgame.state.to_move = game.to_move();
    endgame.state.spread = game.spread(game.to_move());
    endgame.state.scoreless_turns = game.scoreless_turns();
    endgame.state.end_rules = options.rules;
  }
  return endgames;
}

template <typename T>
T Percentile(std::vector<T> values, double p) {
  if (values.empty()) return T{};
  std::sort(values.begin(), values.end());
  const std::size_t index = std::min(
      values.size() - 1,
      static_cast<std::size_t>(p * static_cast<double>(values.size() - 1) +
                               0.5));
  return values[index];
}

int Run(const Options& options) {
  const scrabble::Gaddag gaddag = scrabble::LoadOrBuildGaddag(options.lexicon);
  const std::vector<Endgame> endgames = Collect(gaddag, options);
  scrabble::EndgameSolver solver(gaddag, {.tt_size_mb = options.tt_mb});
  const scrabble::EndgameLimits limits{.max_nodes = options.nodes,
                                       .max_time_ms = options.ms};

  std::vector<double> ms;
  std::vector<std::int64_t> nodes;
  std::vector<int> unsolved_depth;
  std::int64_t total_nodes = 0, probes = 0, hits = 0;
  double total_seconds = 0;
  int solved = 0, tiles_mover = 0, tiles_opponent = 0;
  // Exact-solve rate by tiles on both racks: <=6, 7-9, 10-12, 13-14.
  std::array<std::array<int, 2>, 4> by_tiles{};
  for (const Endgame& endgame : endgames) {
    scrabble::Position position(gaddag, endgame.board);
    const scrabble::EndgameResult result =
        solver.Solve(position, endgame.state, limits);
    const int mover = endgame.state.racks[static_cast<std::size_t>(
                                              endgame.state.to_move)]
                          .Size();
    const int opponent = endgame.state.racks[static_cast<std::size_t>(
                                                 1 - endgame.state.to_move)]
                             .Size();
    tiles_mover += mover;
    tiles_opponent += opponent;
    const int total = mover + opponent;
    auto& bucket = by_tiles[total <= 6 ? 0 : total <= 9 ? 1 : total <= 12 ? 2
                                                                          : 3];
    ++bucket[1];
    if (result.complete) {
      ++solved;
      ++bucket[0];
    } else {
      unsolved_depth.push_back(result.depth);
    }
    ms.push_back(1e3 * result.seconds);
    nodes.push_back(result.nodes);
    total_nodes += result.nodes;
    total_seconds += result.seconds;
    probes += result.tt_probes;
    hits += result.tt_hits;
  }

  const int n = static_cast<int>(endgames.size());
  std::printf("Lexicon        %s\n", options.lexicon.string().c_str());
  std::printf("Endgames       %d from self-play (seed %llu), racks %.1f vs "
              "%.1f tiles (mover vs opponent)\n",
              n, static_cast<unsigned long long>(options.seed),
              static_cast<double>(tiles_mover) / n,
              static_cast<double>(tiles_opponent) / n);
  std::printf("Limits         %lld nodes, %lld ms, TT %zu MB\n",
              static_cast<long long>(options.nodes),
              static_cast<long long>(options.ms), options.tt_mb);
  std::printf("Solved exactly %d / %d (%.1f%%)\n", solved, n,
              100.0 * solved / n);
  const char* labels[] = {"<=6", "7-9", "10-12", "13-14"};
  for (std::size_t i = 0; i < by_tiles.size(); ++i) {
    if (by_tiles[i][1] == 0) continue;
    std::printf("  %-5s tiles  %d / %d\n", labels[i], by_tiles[i][0],
                by_tiles[i][1]);
  }
  std::printf("Time (ms)      median %.1f  p90 %.1f  p99 %.1f  max %.1f\n",
              Percentile(ms, 0.5), Percentile(ms, 0.9), Percentile(ms, 0.99),
              Percentile(ms, 1.0));
  std::printf("Nodes          median %lld  p90 %lld  p99 %lld  max %lld\n",
              static_cast<long long>(Percentile(nodes, 0.5)),
              static_cast<long long>(Percentile(nodes, 0.9)),
              static_cast<long long>(Percentile(nodes, 0.99)),
              static_cast<long long>(Percentile(nodes, 1.0)));
  std::printf("Nodes total    %lld\n", static_cast<long long>(total_nodes));
  std::printf("Nodes/sec      %.0f\n",
              total_seconds > 0 ? static_cast<double>(total_nodes) /
                                      total_seconds
                                : 0.0);
  std::printf("TT hit rate    %.1f%% of %lld probes\n",
              probes > 0 ? 100.0 * static_cast<double>(hits) /
                               static_cast<double>(probes)
                         : 0.0,
              static_cast<long long>(probes));
  if (!unsolved_depth.empty()) {
    std::printf("Depth reached  unsolved: median %d  min %d  max %d\n",
                Percentile(unsolved_depth, 0.5),
                Percentile(unsolved_depth, 0.0),
                Percentile(unsolved_depth, 1.0));
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseArgs(argc, argv, options)) {
    std::cerr << "usage: " << argv[0] << kUsage;
    return 2;
  }
  try {
    return Run(options);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
