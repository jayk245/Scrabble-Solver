// Binds the API handlers to HTTP routes and serves the built web UI.
//
// Kept separate from api.h so that only this file and main.cc compile the
// (large) cpp-httplib header, and the handlers stay testable without it.

#ifndef SCRABBLE_SERVER_HTTP_SERVER_H_
#define SCRABBLE_SERVER_HTTP_SERVER_H_

#include <filesystem>
#include <string>

#include "analyze.h"
#include "api.h"
#include "games.h"

namespace scrabble::server {

struct HttpOptions {
  std::string host = "127.0.0.1";
  int port = 8080;
  // Directory holding the production frontend build (web/dist). If it has
  // no index.html, "/" explains how to build it instead.
  std::filesystem::path static_dir;
};

// Listens until the process is terminated. Returns false if the address
// could not be bound (e.g. the port is in use).
bool Serve(const Api& api, AnalyzeApi& analyze, GamesApi& games,
           const HttpOptions& options);

}  // namespace scrabble::server

#endif  // SCRABBLE_SERVER_HTTP_SERVER_H_
