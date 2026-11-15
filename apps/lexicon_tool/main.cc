// Compiles a word list into a binary GADDAG and reports its size and timing.
//
// Usage: scrabble_lexicon_tool <word_list.txt> [output.gaddag]
//
// The output defaults to the cache path LoadOrBuildGaddag() looks for, so
// running this once makes later engine start-ups load instead of build.

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

#include "scrabble/gaddag.h"
#include "scrabble/lexicon.h"

namespace {

using Clock = std::chrono::steady_clock;

double MillisecondsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

int Run(const std::filesystem::path& input,
        const std::filesystem::path& output) {
  auto start = Clock::now();
  const scrabble::WordList list = scrabble::ReadWordList(input);
  const double read_ms = MillisecondsSince(start);

  start = Clock::now();
  const scrabble::Gaddag built = scrabble::Gaddag::Build(list.words);
  const double build_ms = MillisecondsSince(start);

  start = Clock::now();
  built.Save(output);
  const double save_ms = MillisecondsSince(start);

  start = Clock::now();
  const scrabble::Gaddag loaded = scrabble::Gaddag::Load(output);
  const double load_ms = MillisecondsSince(start);

  // Verifies the round trip and doubles as a lookup micro-benchmark.
  start = Clock::now();
  std::size_t missing = 0;
  for (const std::string& word : list.words) {
    missing += loaded.Contains(word) ? 0 : 1;
  }
  const double verify_ms = MillisecondsSince(start);

  std::printf("Word list     %s\n", input.string().c_str());
  std::printf("Words         %zu (rejected %zu lines)\n", loaded.WordCount(),
              list.rejected);
  std::printf("Nodes         %zu\n", loaded.NodeCount());
  std::printf("Arcs          %zu\n", loaded.ArcCount());
  std::printf("Memory        %zu bytes (%.2f MiB)\n", loaded.MemoryBytes(),
              static_cast<double>(loaded.MemoryBytes()) / (1024.0 * 1024.0));
  std::printf("Read time     %.1f ms\n", read_ms);
  std::printf("Build time    %.1f ms\n", build_ms);
  std::printf("Save time     %.1f ms\n", save_ms);
  std::printf("Load time     %.1f ms\n", load_ms);
  std::printf("Verify time   %.1f ms (%.0f ns/word)\n", verify_ms,
              verify_ms * 1e6 / static_cast<double>(list.words.size()));
  std::printf("Output        %s\n", output.string().c_str());

  if (missing != 0) {
    std::fprintf(stderr, "error: %zu words missing after reload\n", missing);
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "usage: " << argv[0] << " <word_list.txt> [output.gaddag]\n";
    return 2;
  }
  const std::filesystem::path input = argv[1];
  const std::filesystem::path output =
      argc == 3 ? std::filesystem::path(argv[2])
                : scrabble::GaddagCachePath(input);
  try {
    return Run(input, output);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
