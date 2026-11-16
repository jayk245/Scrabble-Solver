// Monte Carlo simulation benchmark on real mid-game positions.
//
// Usage: scrabble_sim_bench [--positions N] [--seed S] [--lexicon PATH]
//                           [--candidates N] [--iterations N] [--plies N]
//                           [--threads T] [--min-iterations N]
//                           [--prune-z Z] [--crn-compare]
//
// Positions come from seeded self-play between static bots (learned
// leaves): from game i (seed S + i), every fourth position with at least
// one tile in the bag is kept until N positions are collected. Each is
// simulated with the given settings, and the report gives time per
// decision, rollouts per second and how often simulation overrides the
// static choice.
//
// --crn-compare additionally simulates every position with pruning off,
// once with common random numbers and once without, and reports the
// variance of the paired difference between the best and every other
// candidate under both: the variance reduction CRN buys.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "scrabble/bot.h"
#include "scrabble/gaddag.h"
#include "scrabble/game.h"
#include "scrabble/leave_evaluator.h"
#include "scrabble/lexicon.h"
#include "scrabble/move.h"
#include "scrabble/rng.h"
#include "scrabble/simulation.h"

namespace {

struct Options {
  int positions = 50;
  std::uint64_t seed = 1;
  std::filesystem::path lexicon =
      std::filesystem::path(SCRABBLE_LEXICA_DIR) / "enable1.txt";
  scrabble::SimulationConfig sim;
  bool crn_compare = false;
};

constexpr std::string_view kUsage =
    " [--positions N] [--seed S] [--lexicon PATH] [--candidates N]\n"
    "    [--iterations N] [--plies N] [--threads T] [--min-iterations N]\n"
    "    [--prune-z Z] [--crn-compare]\n";

bool ParseInt(const char* text, long long min, long long max, int& out) {
  char* end = nullptr;
  const long long value = std::strtoll(text, &end, 10);
  if (*end != '\0' || value < min || value > max) return false;
  out = static_cast<int>(value);
  return true;
}

bool ParseArgs(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--crn-compare") {
      options.crn_compare = true;
      continue;
    }
    if (i + 1 >= argc) return false;
    const char* value = argv[++i];
    constexpr long long kMax = 100'000'000;
    if (arg == "--positions") {
      if (!ParseInt(value, 1, kMax, options.positions)) return false;
    } else if (arg == "--seed") {
      char* end = nullptr;
      options.seed = std::strtoull(value, &end, 10);
      if (*end != '\0') return false;
    } else if (arg == "--lexicon") {
      options.lexicon = value;
    } else if (arg == "--candidates") {
      if (!ParseInt(value, 1, kMax, options.sim.candidates)) return false;
    } else if (arg == "--iterations") {
      if (!ParseInt(value, 1, kMax, options.sim.iterations)) return false;
    } else if (arg == "--plies") {
      if (!ParseInt(value, 0, scrabble::kMaxSimulationPlies,
                    options.sim.plies)) {
        return false;
      }
    } else if (arg == "--threads") {
      if (!ParseInt(value, 1, 256, options.sim.threads)) return false;
    } else if (arg == "--min-iterations") {
      if (!ParseInt(value, 2, kMax, options.sim.min_iterations)) return false;
    } else if (arg == "--prune-z") {
      char* end = nullptr;
      options.sim.prune_z = std::strtod(value, &end);
      if (*end != '\0') return false;
    } else {
      return false;
    }
  }
  return true;
}

// A captured decision: the game is kept so the view's position stays
// valid.
struct Decision {
  scrabble::Game game;
  std::vector<scrabble::Move> candidates;
};

std::vector<std::unique_ptr<Decision>> Collect(const scrabble::Gaddag& gaddag,
                                               const Options& options) {
  std::vector<std::unique_ptr<Decision>> decisions;
  scrabble::BotConfig config;
  scrabble::Bot bot(gaddag, config);
  scrabble::Rng rng(options.seed);
  std::vector<scrabble::Move> ranked;
  for (std::uint64_t seed = options.seed;
       static_cast<int>(decisions.size()) < options.positions; ++seed) {
    scrabble::Game game(gaddag, seed);
    while (!game.IsOver() && game.bag().Size() > 0 &&
           static_cast<int>(decisions.size()) < options.positions) {
      const scrabble::PlayerView view = game.ViewForMover();
      if (game.turn() % 4 == 1) {
        bot.RankMoves(view, static_cast<std::size_t>(options.sim.candidates),
                      ranked);
        decisions.push_back(
            std::make_unique<Decision>(Decision{game, ranked}));
      }
      game.Play(bot.ChooseMove(view, rng));
    }
  }
  return decisions;
}

double Mean(const std::vector<double>& values) {
  double sum = 0.0;
  for (const double v : values) sum += v;
  return values.empty() ? 0.0 : sum / static_cast<double>(values.size());
}

double Percentile(std::vector<double> values, double p) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const auto index = static_cast<std::size_t>(
      p * static_cast<double>(values.size() - 1) + 0.5);
  return values[std::min(index, values.size() - 1)];
}

// Mean over candidates other than the best of the paired-difference
// variance (n * diff_std_error^2), for one simulation.
double MeanDiffVariance(const scrabble::SimulationResult& result) {
  double sum = 0.0;
  int n = 0;
  for (std::size_t i = 0; i < result.candidates.size(); ++i) {
    if (static_cast<int>(i) == result.best) continue;
    const scrabble::CandidateStats& stats = result.candidates[i];
    sum += stats.iterations * stats.diff_std_error * stats.diff_std_error;
    ++n;
  }
  return n > 0 ? sum / n : 0.0;
}

int Run(const Options& options) {
  const scrabble::Gaddag gaddag = scrabble::LoadOrBuildGaddag(options.lexicon);
  const std::vector<std::unique_ptr<Decision>> decisions =
      Collect(gaddag, options);
  scrabble::Simulator simulator(gaddag, scrabble::DefaultLeaves(),
                                options.sim);

  std::vector<double> ms;
  double total_seconds = 0.0;
  std::int64_t rollouts = 0;
  std::int64_t iterations = 0;
  int overrides = 0;
  std::uint64_t checksum = 0;
  for (std::size_t d = 0; d < decisions.size(); ++d) {
    const Decision& decision = *decisions[d];
    const scrabble::PlayerView view = decision.game.ViewForMover();
    const scrabble::SimulationResult& result =
        simulator.Run(view, decision.candidates, options.seed + d);
    ms.push_back(1e3 * result.seconds);
    total_seconds += result.seconds;
    rollouts += result.rollouts;
    iterations += result.iterations;
    overrides += result.best != 0;
    checksum = checksum * 1'000'003 + static_cast<std::uint64_t>(result.best) +
               static_cast<std::uint64_t>(result.rollouts);
  }

  const scrabble::SimulationConfig& sim = options.sim;
  std::printf("Positions        %zu (seed %llu)\n", decisions.size(),
              static_cast<unsigned long long>(options.seed));
  std::printf("Settings         %d candidates x %d iterations x %d plies, "
              "%d threads, prune z %.1f after %d\n",
              sim.candidates, sim.iterations, sim.plies, sim.threads,
              sim.prune_z, sim.min_iterations);
  std::printf("Time/decision    mean %.1f ms  median %.1f ms  p90 %.1f ms  "
              "max %.1f ms\n",
              Mean(ms), Percentile(ms, 0.5), Percentile(ms, 0.9),
              Percentile(ms, 1.0));
  std::printf("Iterations       %.1f per decision\n",
              static_cast<double>(iterations) /
                  static_cast<double>(decisions.size()));
  std::printf("Rollouts/s       %.0f (%.1f us each, %d plies after the "
              "candidate)\n",
              static_cast<double>(rollouts) / total_seconds,
              1e6 * total_seconds / static_cast<double>(rollouts), sim.plies);
  std::printf("Overrides        %d of %zu decisions differ from the static "
              "choice\n",
              overrides, decisions.size());
  std::printf("Checksum         %016llx\n",
              static_cast<unsigned long long>(checksum));

  if (options.crn_compare) {
    scrabble::SimulationConfig unpruned = options.sim;
    unpruned.prune_z = 0.0;
    std::vector<double> ratios;
    double var_on = 0.0;
    double var_off = 0.0;
    for (int crn = 1; crn >= 0; --crn) {
      unpruned.common_random_numbers = crn == 1;
      scrabble::Simulator fixed(gaddag, scrabble::DefaultLeaves(), unpruned);
      for (std::size_t d = 0; d < decisions.size(); ++d) {
        const Decision& decision = *decisions[d];
        const scrabble::PlayerView view = decision.game.ViewForMover();
        const double v = MeanDiffVariance(
            fixed.Run(view, decision.candidates, options.seed + d));
        if (crn == 1) {
          var_on += v;
          ratios.push_back(v);
        } else {
          var_off += v;
          ratios[d] = ratios[d] > 0.0 ? v / ratios[d] : 0.0;
        }
      }
    }
    std::printf("CRN              paired-difference variance %.0f with CRN, "
                "%.0f without: %.1fx lower (median position %.1fx)\n",
                var_on / static_cast<double>(decisions.size()),
                var_off / static_cast<double>(decisions.size()),
                var_on > 0.0 ? var_off / var_on : 0.0,
                Percentile(ratios, 0.5));
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  options.sim.enabled = true;
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
