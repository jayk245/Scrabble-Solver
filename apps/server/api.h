// Request handlers for the web API, independent of any HTTP library.
//
// Each handler takes the raw request body and returns a status plus a JSON
// body, so the whole API can be unit tested without sockets; http_server.cc
// only maps routes onto these calls.
//
// The analysis endpoints are stateless: the client sends the full board
// (and rack) with every request and keeps history itself. The only shared
// state is the immutable lexicon, so handlers are safe to run concurrently
// on the server's worker threads without locking; each call builds its own
// Position and MoveGenerator, which costs microseconds next to generation.
//
// Analysis (this class; see web/src/api/types.ts for the full schema):
//   GET  /api/health     liveness and lexicon info
//   GET  /api/rules      premium layout, tile values and distribution
//   POST /api/position   unseen-tile summary for a board and rack
//   POST /api/moves      every legal placement for a rack, best first
//   POST /api/validate   legality of one move in notation, with its score
//   POST /api/play       apply a legal move and return the new board
//
// Game viewer (GamesApi in games.h; the only stateful endpoints, backed by
// the in-memory GameRegistry in game_registry.h):
//   POST   /api/games              start a bot-vs-bot game or a GCG replay;
//                                  201 {"id", "mode", "events"}
//   GET    /api/games/{id}/events  Server-Sent Events stream: "start", one
//                                  "move" per turn, then "end". The SSE id
//                                  is the event's index, so a reconnecting
//                                  EventSource resumes via Last-Event-ID.
//   GET    /api/games/{id}         status summary
//   GET    /api/games/{id}/gcg     the game record (text/plain)
//   DELETE /api/games/{id}         cancel a running game

#ifndef SCRABBLE_SERVER_API_H_
#define SCRABBLE_SERVER_API_H_

#include <cstddef>
#include <functional>
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

// Shared by the handler classes.
//
// Parses a request body that must be a JSON object; throws ApiError (400,
// "bad_json") otherwise.
nlohmann::json ParseRequestObject(std::string_view body);
// {"error": code, "message": message} with `status`.
ApiResponse ErrorResponse(int status, std::string_view code,
                          std::string_view message);
// Runs `handler`, turning ApiError into its status, JSON type errors into
// 400 and any other exception into 500, so a bad request never takes the
// server down.
ApiResponse GuardedResponse(const std::function<ApiResponse()>& handler);

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

  static ApiResponse NotFound(std::string_view path);

 private:
  const Gaddag* gaddag_;
  ServerInfo info_;
};

}  // namespace scrabble::server

#endif  // SCRABBLE_SERVER_API_H_
