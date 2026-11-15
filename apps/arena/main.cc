// Self-play arena: plays N games between bot configurations A and B.
//
// Usage: scrabble_arena [--games N] [--threads T] [--seed S]
//                       [--lexicon PATH] [--leaves-a L] [--leaves-b L]
//                       [--rules naspa|wespa] [--gcg-dir DIR] [--log FILE]
//                       [--endgame-a] [--endgame-b] [--endgame-nodes N]
//                       [--endgame-ms T]
//
// L is "heuristic" (default), "zero", or the path of a leave table CSV (see
// leave_evaluator.h). --log writes the JSONL training log documented in
// arena.h. --endgame-a / --endgame-b turn on the endgame solver for that
// configuration, limited to --endgame-nodes nodes (default 2,000,000) and,
// if given, --endgame-ms milliseconds per move. Results are identical for
// any --threads value, unless --endgame-ms makes them timing-dependent.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "scrabble/arena.h"
#include "scrabble/bot.h"
#include "scrabble/gaddag.h"
#include "scrabble/game.h"
#include "scrabble/leave_evaluator.h"
#include "scrabble/lexicon.h"

namespace {

struct Options {
  int games = 1000;
  int threads = static_cast<int>(std::thread::hardware_concurrency());
  std::uint64_t seed = 1;
  std::filesystem::path lexicon =
      std::filesystem::path(SCRABBLE_LEXICA_DIR) / "enable1.txt";
  std::string leaves_a = "heuristic";
  std::string leaves_b = "heuristic";
  scrabble::EndRules rules = scrabble::EndRules::kNaspa;
  std::filesystem::path gcg_dir;
  std::filesystem::path log;
  bool endgame_a = false;
  bool endgame_b = false;
  std::int64_t endgame_nodes = scrabble::EndgameConfig{}.node_limit;
  std::int64_t endgame_ms = 0;
};

constexpr std::string_view kUsage =
    " [--games N] [--threads T] [--seed S] [--lexicon PATH]\n"
    "    [--leaves-a heuristic|zero|FILE] [--leaves-b heuristic|zero|FILE]\n"
    "    [--rules naspa|wespa] [--gcg-dir DIR] [--log FILE]\n"
    "    [--endgame-a] [--endgame-b] [--endgame-nodes N] [--endgame-ms T]\n";

bool ParsePositive(const char* text, int& out) {
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (*end != '\0' || value <= 0 || value > 100'000'000) return false;
  out = static_cast<int>(value);
  return true;
}

bool ParseArgs(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--endgame-a") {
      options.endgame_a = true;
      continue;
    }
    if (arg == "--endgame-b") {
      options.endgame_b = true;
      continue;
    }
    if (i + 1 >= argc) return false;
    const char* value = argv[++i];
    if (arg == "--games") {
      if (!ParsePositive(value, options.games)) return false;
    } else if (arg == "--threads") {
      if (!ParsePositive(value, options.threads)) return false;
    } else if (arg == "--seed") {
      char* end = nullptr;
      options.seed = std::strtoull(value, &end, 10);
      if (*end != '\0') return false;
    } else if (arg == "--lexicon") {
      options.lexicon = value;
    } else if (arg == "--leaves-a") {
      options.leaves_a = value;
    } else if (arg == "--leaves-b") {
      options.leaves_b = value;
    } else if (arg == "--rules") {
      const std::string_view rules = value;
      if (rules == "naspa") {
        options.rules = scrabble::EndRules::kNaspa;
      } else if (rules == "wespa") {
        options.rules = scrabble::EndRules::kWespa;
      } else {
        return false;
      }
    } else if (arg == "--gcg-dir") {
      options.gcg_dir = value;
    } else if (arg == "--log") {
      options.log = value;
    } else if (arg == "--endgame-nodes" || arg == "--endgame-ms") {
      char* end = nullptr;
      const long long n = std::strtoll(value, &end, 10);
      if (*end != '\0' || n < 0) return false;
      (arg == "--endgame-nodes" ? options.endgame_nodes : options.endgame_ms) =
          n;
    } else {
      return false;
    }
  }
  return true;
}

scrabble::BotConfig MakeBot(const std::string& leaves, bool endgame,
                            const Options& options) {
  scrabble::BotConfig config;
  if (leaves == "heuristic") {
    config.leaves = std::make_shared<const scrabble::LeaveEvaluator>(
        scrabble::LeaveEvaluator::Heuristic());
  } else if (leaves == "zero") {
    config.leaves = std::make_shared<const scrabble::LeaveEvaluator>(
        scrabble::LeaveEvaluator::Zero());
  } else {
    config.leaves = std::make_shared<const scrabble::LeaveEvaluator>(
        scrabble::LeaveEvaluator::Load(leaves));
  }
  config.name = config.leaves->name();
  if (endgame) {
    config.endgame.enabled = true;
    config.endgame.node_limit = options.endgame_nodes;
    config.endgame.time_limit_ms = options.endgame_ms;
    config.name += "+endgame";
  }
  return config;
}

int Run(const Options& options) {
  const scrabble::Gaddag gaddag = scrabble::LoadOrBuildGaddag(options.lexicon);

  scrabble::ArenaConfig config;
  config.games = options.games;
  config.threads = options.threads;
  config.seed = options.seed;
  config.game.end_rules = options.rules;
  config.bot_a = MakeBot(options.leaves_a, options.endgame_a, options);
  config.bot_b = MakeBot(options.leaves_b, options.endgame_b, options);
  config.gcg_dir = options.gcg_dir;
  std::ofstream log;
  if (!options.log.empty()) {
    log.open(options.log);
    if (!log) {
      std::cerr << "error: cannot open " << options.log << '\n';
      return 1;
    }
    config.log = &log;
  }

  const scrabble::ArenaResult result = scrabble::RunArena(gaddag, config);
  if (log.is_open()) {
    log.flush();
    if (!log) {
      std::cerr << "error: failed writing " << options.log << '\n';
      return 1;
    }
  }

  const scrabble::ArenaSummary& s = result.summary;
  std::printf("Lexicon       %s (%zu words)\n",
              options.lexicon.string().c_str(), gaddag.WordCount());
  std::printf("A             %s\n", config.bot_a.name.c_str());
  std::printf("B             %s\n", config.bot_b.name.c_str());
  std::printf("Games         %d (seed %llu, %d threads, %s rules)\n", s.games,
              static_cast<unsigned long long>(options.seed),
              std::min(options.threads, s.games),
              options.rules == scrabble::EndRules::kNaspa ? "NASPA" : "WESPA");
  std::printf("A W-L-T       %d-%d-%d  win rate %.1f%% +/- %.1f%%\n", s.wins,
              s.losses, s.ties, 100.0 * s.win_rate, 100.0 * s.win_rate_ci);
  std::printf("Mean score    A %.1f +/- %.1f   B %.1f +/- %.1f\n",
              s.mean_score_a, s.mean_score_a_ci, s.mean_score_b,
              s.mean_score_b_ci);
  std::printf("Spread (A-B)  %+.1f +/- %.1f (95%% CI)\n", s.mean_spread,
              s.mean_spread_ci);
  std::printf("Bingos/game   A %.2f   B %.2f\n", s.bingos_per_game_a,
              s.bingos_per_game_b);
  std::printf("Turns/game    %.1f\n", s.turns_per_game);
  std::printf("Time          %.2f s (%.1f games/s)\n", result.seconds,
              result.seconds > 0 ? s.games / result.seconds : 0.0);
  // Thread-seconds per game: what one game costs on one core.
  std::printf("Time/game     %.1f ms per thread\n",
              s.games > 0 ? 1e3 * result.seconds *
                                std::min(options.threads, s.games) / s.games
                          : 0.0);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (options.threads < 1) options.threads = 1;
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
