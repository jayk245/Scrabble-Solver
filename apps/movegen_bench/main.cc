// Move-generation throughput benchmark.
//
// Usage: scrabble_movegen_bench [--games N] [--seed S] [--lexicon PATH]
//
// Plays N seeded games in which each side picks a uniformly random legal
// placement (passing when there is none). The random mover exists only to
// drive the generator over realistic, varied positions; it is not a bot.
// Timing covers MoveGenerator::Generate() alone. The checksum (sum of all
// generated scores) is printed so optimisations can be checked for identical
// output across runs with the same seed.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "scrabble/bag.h"
#include "scrabble/gaddag.h"
#include "scrabble/lexicon.h"
#include "scrabble/move.h"
#include "scrabble/move_generator.h"
#include "scrabble/position.h"
#include "scrabble/rack.h"
#include "scrabble/rng.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  int games = 1000;
  std::uint64_t seed = 1;
  std::filesystem::path lexicon =
      std::filesystem::path(SCRABBLE_LEXICA_DIR) / "enable1.txt";
};

struct Totals {
  std::uint64_t positions = 0;
  std::uint64_t moves = 0;
  std::uint64_t max_moves = 0;
  std::int64_t checksum = 0;
  Clock::duration generate_time{};
};

void TakeTiles(scrabble::Rack& rack, const scrabble::Move& move) {
  for (int i = 0; i < move.num_tiles; ++i) {
    const scrabble::MoveTile tile = move.tiles[static_cast<std::size_t>(i)];
    if (tile.is_play_through()) continue;
    rack.Remove(tile.is_blank() ? scrabble::kBlank : tile.letter());
  }
}

void PlayGame(const scrabble::Gaddag& gaddag, std::uint64_t seed,
              scrabble::MoveGenerator& generator,
              std::vector<scrabble::Move>& moves, Totals& totals) {
  scrabble::Rng rng(seed);
  scrabble::Bag bag = scrabble::Bag::Full();
  scrabble::Position position(gaddag);
  scrabble::Rack racks[2];
  bag.DrawInto(racks[0], rng);
  bag.DrawInto(racks[1], rng);
  int passes = 0;
  for (int turn = 0; passes < 4; turn ^= 1) {
    scrabble::Rack& rack = racks[turn];
    const Clock::time_point start = Clock::now();
    generator.Generate(position, rack, moves);
    totals.generate_time += Clock::now() - start;

    ++totals.positions;
    totals.moves += moves.size();
    if (moves.size() > totals.max_moves) totals.max_moves = moves.size();
    for (const scrabble::Move& move : moves) totals.checksum += move.score;

    if (moves.empty()) {
      ++passes;
      continue;
    }
    passes = 0;
    const scrabble::Move move =
        moves[rng.Below(static_cast<std::uint32_t>(moves.size()))];
    position.Apply(move);
    TakeTiles(rack, move);
    bag.DrawInto(rack, rng);
    if (rack.IsEmpty()) break;
  }
}

bool ParseArgs(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (i + 1 >= argc) return false;
    const char* value = argv[++i];
    if (arg == "--games") {
      options.games = std::atoi(value);
      if (options.games <= 0) return false;
    } else if (arg == "--seed") {
      options.seed = std::strtoull(value, nullptr, 10);
    } else if (arg == "--lexicon") {
      options.lexicon = value;
    } else {
      return false;
    }
  }
  return true;
}

int Run(const Options& options) {
  const Clock::time_point load_start = Clock::now();
  const scrabble::Gaddag gaddag = scrabble::LoadOrBuildGaddag(options.lexicon);
  const double load_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - load_start)
          .count();

  scrabble::MoveGenerator generator(gaddag);
  std::vector<scrabble::Move> moves;
  Totals totals;
  const Clock::time_point start = Clock::now();
  for (int game = 0; game < options.games; ++game) {
    PlayGame(gaddag, options.seed + static_cast<std::uint64_t>(game),
             generator, moves, totals);
  }
  const double wall_s =
      std::chrono::duration<double>(Clock::now() - start).count();
  const double gen_s =
      std::chrono::duration<double>(totals.generate_time).count();
  const auto positions = static_cast<double>(totals.positions);
  const auto generated = static_cast<double>(totals.moves);

  std::printf("Lexicon          %s (%zu words, loaded in %.0f ms)\n",
              options.lexicon.string().c_str(), gaddag.WordCount(), load_ms);
  std::printf("Games            %d (seeds %llu..%llu)\n", options.games,
              static_cast<unsigned long long>(options.seed),
              static_cast<unsigned long long>(
                  options.seed + static_cast<std::uint64_t>(options.games) -
                  1));
  std::printf("Positions        %llu\n",
              static_cast<unsigned long long>(totals.positions));
  std::printf("Moves generated  %llu (avg %.1f, max %llu per position)\n",
              static_cast<unsigned long long>(totals.moves),
              generated / positions,
              static_cast<unsigned long long>(totals.max_moves));
  std::printf("Generate time    %.3f s (wall %.3f s incl. game driver)\n",
              gen_s, wall_s);
  std::printf("Positions/sec    %.0f\n", positions / gen_s);
  std::printf("Moves/sec        %.0f\n", generated / gen_s);
  std::printf("Time/position    %.2f us\n", gen_s * 1e6 / positions);
  std::printf("Checksum         %lld\n",
              static_cast<long long>(totals.checksum));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseArgs(argc, argv, options)) {
    std::cerr << "usage: " << argv[0]
              << " [--games N] [--seed S] [--lexicon PATH]\n";
    return 2;
  }
  try {
    return Run(options);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
