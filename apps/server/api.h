// Request handlers for the web API, independent of any HTTP library.
//
// Each handler takes the raw request body and returns a status plus a JSON
// body, so the whole API can be unit tested without sockets; http_server.cc
// only maps routes onto these calls.
//
// Endpoints are stateless: the client sends the full board (and rack) with
// every request and keeps history itself. The only shared state is the
// immutable lexicon, so handlers are safe to run concurrently on the
// server's worker threads without locking; each call builds its own
// Position and MoveGenerator, which costs microseconds next to generation.
//
// Implemented (see web/src/api/types.ts for the full schema):
//   GET  /api/health     liveness and lexicon info
//   GET  /api/rules      premium layout, tile values and distribution
//   POST /api/position   unseen-tile summary for a board and rack
//   POST /api/moves      every legal placement for a rack, best first
//   POST /api/validate   legality of one move in notation, with its score
//   POST /api/play       apply a legal move and return the new board
//
// Planned for the self-play milestone (not implemented; the server answers
// 501 so clients can detect it):
//   POST /api/games              start a bot-vs-bot game or a GCG replay;
//                                returns {"id": ...}
//   GET  /api/games/{id}/events  Server-Sent Events stream of the game:
//                                "start", then one "move" per turn, then
//                                "end"; reconnecting clients resume with
//                                Last-Event-ID = turn number.

#ifndef SCRABBLE_SERVER_API_H_
#define SCRABBLE_SERVER_API_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "scrabble/gaddag.h"

namespace scrabble::server {

struct ApiResponse {
  int status = 200;
  nlohmann::json body;
};

// Describes the loaded lexicon and build for /api/health.
struct ServerInfo {
  std::string version;
  std::string lexicon_name;  // e.g. "enable1"
};

// /api/moves returns at most this many moves unless the request asks for
// more; a rack with a blank can have tens of thousands of placements, which
// the UI cannot usefully display. "total" always reports the full count.
inline constexpr std::size_t kDefaultMoveLimit = 1000;
inline constexpr std::size_t kMaxMoveLimit = 100000;

class Api {
 public:
  // `gaddag` must outlive the Api.
  Api(const Gaddag& gaddag, ServerInfo info)
      : gaddag_(&gaddag), info_(std::move(info)) {}

  ApiResponse Health() const;
  ApiResponse Rules() const;

  // POST handlers. A body that is not valid JSON, or a malformed field,
  // yields 400 with {"error": code, "message": text}.
  ApiResponse PositionSummary(std::string_view body) const;
  ApiResponse Moves(std::string_view body) const;
  ApiResponse Validate(std::string_view body) const;
  ApiResponse Play(std::string_view body) const;

  // Response for reserved endpoints that are not implemented yet.
  static ApiResponse NotImplemented(std::string_view what);
  static ApiResponse NotFound(std::string_view path);

 private:
  const Gaddag* gaddag_;
  ServerInfo info_;
};

}  // namespace scrabble::server

#endif  // SCRABBLE_SERVER_API_H_
