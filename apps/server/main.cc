// HTTP server for the web UI: loads the lexicon once, then serves the JSON
// API and the production frontend build.
//
// Usage: scrabble_server [--host HOST] [--port PORT] [--lexicon PATH]
//                        [--static-dir DIR]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "api.h"
#include "game_registry.h"
#include "games.h"
#include "http_server.h"
#include "scrabble/gaddag.h"
#include "scrabble/lexicon.h"

namespace {

struct Options {
  scrabble::server::HttpOptions http{
      .static_dir = std::filesystem::path(SCRABBLE_WEB_DIST_DIR)};
  std::filesystem::path lexicon =
      std::filesystem::path(SCRABBLE_LEXICA_DIR) / "enable1.txt";
};

bool ParseArgs(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (i + 1 >= argc) return false;
    const char* value = argv[++i];
    if (arg == "--host") {
      options.http.host = value;
    } else if (arg == "--port") {
      options.http.port = std::atoi(value);
      if (options.http.port <= 0 || options.http.port > 65535) return false;
    } else if (arg == "--lexicon") {
      options.lexicon = value;
    } else if (arg == "--static-dir") {
      options.http.static_dir = value;
    } else {
      return false;
    }
  }
  return true;
}

int Run(const Options& options) {
  const auto start = std::chrono::steady_clock::now();
  const scrabble::Gaddag gaddag = scrabble::LoadOrBuildGaddag(options.lexicon);
  const double load_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();
  const std::string lexicon_name = options.lexicon.stem().string();
  std::fprintf(stderr, "Loaded %s (%zu words) in %.0f ms\n",
               lexicon_name.c_str(), gaddag.WordCount(), load_ms);

  const scrabble::server::Api api(
      gaddag, {.version = SCRABBLE_VERSION, .lexicon_name = lexicon_name});
  // Declared after the lexicon so that game threads, which read it, are
  // joined (by the registry's destructor) before it is destroyed.
  scrabble::server::GameRegistry registry;
  scrabble::server::GamesApi games(gaddag, registry);
  if (!scrabble::server::Serve(api, games, options.http)) {
    std::fprintf(stderr, "error: cannot listen on %s:%d\n",
                 options.http.host.c_str(), options.http.port);
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseArgs(argc, argv, options)) {
    std::cerr << "usage: " << argv[0]
              << " [--host HOST] [--port PORT] [--lexicon PATH]"
                 " [--static-dir DIR]\n";
    return 2;
  }
  try {
    return Run(options);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
