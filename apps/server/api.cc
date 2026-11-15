#include "api.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codec.h"
#include "scrabble/board.h"
#include "scrabble/move.h"
#include "scrabble/move_generator.h"
#include "scrabble/move_validation.h"
#include "scrabble/position.h"
#include "scrabble/rack.h"

namespace scrabble::server {
namespace {

using Json = nlohmann::json;

ApiResponse Error(int status, std::string_view code, std::string_view message) {
  return {status, Json{{"error", code}, {"message", message}}};
}

Json ParseObject(std::string_view body) {
  // Exceptions are disabled for the parse so malformed input takes the same
  // error path as every other client mistake.
  Json value = Json::parse(body, /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (value.is_discarded() || !value.is_object()) {
    throw ApiError(400, "bad_json", "request body must be a JSON object");
  }
  return value;
}

// An absent board means the empty board, which keeps ad-hoc requests short.
Board BoardField(const Json& request) {
  const auto it = request.find("board");
  return it == request.end() || it->is_null() ? Board{} : BoardFromJson(*it);
}

std::optional<Rack> OptionalRackField(const Json& request) {
  const auto it = request.find("rack");
  if (it == request.end() || it->is_null()) return std::nullopt;
  return RackFromJson(*it);
}

Rack RequiredRackField(const Json& request) {
  std::optional<Rack> rack = OptionalRackField(request);
  if (!rack) throw ApiError(400, "missing_field", "\"rack\" is required");
  return *rack;
}

Move MoveField(const Json& request) {
  const auto it = request.find("move");
  if (it == request.end() || !it->is_string()) {
    throw ApiError(400, "missing_field",
                   "\"move\" is required and must be a notation string");
  }
  return MoveFromNotation(it->get_ref<const std::string&>());
}

// Exactly the tiles a move takes from a rack. Used when the client sends no
// rack (e.g. replaying a pasted game whose racks are unknown) so validation
// still checks geometry and words but not tile ownership.
Rack RackForMove(const Move& move) {
  Rack rack;
  for (int i = 0; i < move.num_tiles; ++i) {
    const MoveTile tile = move.tiles[static_cast<std::size_t>(i)];
    if (move.type == MoveType::kPlace) {
      if (tile.is_play_through()) continue;
      rack.Add(tile.is_blank() ? kBlank : tile.letter());
    } else {
      rack.Add(tile.letter());
    }
  }
  return rack;
}

std::size_t LimitField(const Json& request) {
  const auto it = request.find("limit");
  if (it == request.end() || it->is_null()) return kDefaultMoveLimit;
  if (!it->is_number_integer() || it->get<long long>() < 1) {
    throw ApiError(400, "bad_limit", "\"limit\" must be a positive integer");
  }
  return std::min(it->get<std::size_t>(), kMaxMoveLimit);
}

// Runs a handler, converting client errors into 4xx responses. Anything
// else is a server bug and becomes a 500 rather than killing the process.
template <typename Handler>
ApiResponse Guard(Handler&& handler) {
  try {
    return handler();
  } catch (const ApiError& e) {
    return Error(e.status(), e.code(), e.what());
  } catch (const Json::exception& e) {
    // Wrong JSON types inside an otherwise well-formed request.
    return Error(400, "bad_request", e.what());
  } catch (const std::exception& e) {
    return Error(500, "internal_error", e.what());
  }
}

}  // namespace

ApiResponse Api::Health() const {
  return {200, Json{{"status", "ok"},
                    {"version", info_.version},
                    {"lexicon", info_.lexicon_name},
                    {"words", gaddag_->WordCount()}}};
}

ApiResponse Api::Rules() const { return {200, RulesToJson()}; }

ApiResponse Api::PositionSummary(std::string_view body) const {
  return Guard([&] {
    const Json request = ParseObject(body);
    const Board board = BoardField(request);
    const Rack rack = OptionalRackField(request).value_or(Rack{});
    return ApiResponse{200, PositionSummaryToJson(board, rack)};
  });
}

ApiResponse Api::Moves(std::string_view body) const {
  return Guard([&] {
    const Json request = ParseObject(body);
    const Board board = BoardField(request);
    const Rack rack = RequiredRackField(request);
    const std::size_t limit = LimitField(request);

    const auto start = std::chrono::steady_clock::now();
    const Position position(*gaddag_, board);
    MoveGenerator generator(*gaddag_);
    std::vector<Move> moves;
    generator.Generate(position, rack, moves);
    // Stable, so equal scores keep the generator's deterministic order.
    std::stable_sort(moves.begin(), moves.end(),
                     [](const Move& a, const Move& b) {
                       return a.score > b.score;
                     });
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();

    Json list = Json::array();
    const std::size_t count = std::min(limit, moves.size());
    for (std::size_t i = 0; i < count; ++i) {
      list.push_back(MoveToJson(moves[i], &rack));
    }
    return ApiResponse{
        200, Json{{"moves", std::move(list)},
                  {"total", moves.size()},
                  {"truncated", count < moves.size()},
                  {"elapsedMs", elapsed_ms},
                  {"position", PositionSummaryToJson(board, rack)}}};
  });
}

ApiResponse Api::Validate(std::string_view body) const {
  return Guard([&] {
    const Json request = ParseObject(body);
    const Board board = BoardField(request);
    Move move = MoveField(request);
    const std::optional<Rack> rack = OptionalRackField(request);
    const Rack effective = rack.value_or(RackForMove(move));
    const MoveError error = ValidateMove(board, *gaddag_, effective, move);
    if (error != MoveError::kOk) {
      // An illegal move is a successful answer to "is this legal?", hence
      // 200 rather than an error status.
      return ApiResponse{200, Json{{"valid", false},
                                   {"error", MoveErrorCode(error)},
                                   {"message", MoveErrorMessage(error)}}};
    }
    return ApiResponse{
        200, Json{{"valid", true},
                  {"move", MoveToJson(move, rack ? &*rack : nullptr)}}};
  });
}

ApiResponse Api::Play(std::string_view body) const {
  return Guard([&] {
    const Json request = ParseObject(body);
    Board board = BoardField(request);
    Move move = MoveField(request);
    const std::optional<Rack> rack = OptionalRackField(request);
    const MoveError error =
        ValidateMove(board, *gaddag_, rack.value_or(RackForMove(move)), move);
    if (error != MoveError::kOk) {
      return Error(422, MoveErrorCode(error), MoveErrorMessage(error));
    }
    if (move.type == MoveType::kPlace) PlaceTiles(board, move);
    Json response = {{"board", BoardToJson(board)},
                     {"move", MoveToJson(move, rack ? &*rack : nullptr)}};
    const Rack leave = rack ? LeaveAfter(*rack, move) : Rack{};
    if (rack) response["rack"] = leave.ToString();
    response["position"] = PositionSummaryToJson(board, leave);
    return ApiResponse{200, std::move(response)};
  });
}

ApiResponse Api::NotImplemented(std::string_view what) {
  return Error(501, "not_implemented",
               std::string(what) + " is planned but not implemented yet");
}

ApiResponse Api::NotFound(std::string_view path) {
  return Error(404, "not_found", "no API endpoint " + std::string(path));
}

}  // namespace scrabble::server
