// The position solver behind POST /api/analyze: "what should I play here?"
//
// The client describes a position between two players (board, the mover's
// rack, both scores and, if known, the opponent's rack) and gets the
// engine's ranked candidates:
//
//   * Tiles in the bag: the top static moves re-ranked by Monte Carlo
//     simulation (method "simulation"), or the static ranking alone for
//     effort "static".
//   * Bag empty: the opponent's rack is exactly the unseen tiles, so the
//     exact endgame solver decides (method "endgame"); its value is the
//     final spread under best play by both sides.
//
// The unseen pool is the full distribution minus the board and the mover's
// rack. A known opponent rack is removed from the bag and fixed in every
// rollout (PlayerView::known_opponent_rack); otherwise the opponent holds
// min(7, unseen) random unseen tiles. A position that uses more of a letter
// than the distribution holds is rejected (422), since the bag size would
// be meaningless.
//
// Cost is bounded per request: effort presets map to simulation and
// endgame budgets with wall-clock limits of at most kMaxAnalyzeMs, each
// request uses up to kAnalyzeThreads threads, and requests that search
// (anything but effort "static") run one at a time; a request that cannot
// start within kAnalyzeQueueMs gets 503 "busy".

#ifndef SCRABBLE_SERVER_ANALYZE_H_
#define SCRABBLE_SERVER_ANALYZE_H_

#include <cstdint>
#include <mutex>
#include <string_view>

#include "api.h"
#include "scrabble/gaddag.h"

namespace scrabble::server {

inline constexpr int kAnalyzeThreads = 4;
inline constexpr std::int64_t kMaxAnalyzeMs = 5000;
inline constexpr std::int64_t kAnalyzeQueueMs = 10000;
inline constexpr int kMaxAnalyzeCandidates = 20;
inline constexpr int kMaxAnalyzeIterations = 2000;
inline constexpr int kMaxAnalyzePlies = 4;
inline constexpr int kDefaultStaticCandidates = 10;
inline constexpr int kMaxStaticCandidates = 30;

// Handler for POST /api/analyze. Thread-safe.
class AnalyzeApi {
 public:
  // `gaddag` must outlive the AnalyzeApi.
  explicit AnalyzeApi(const Gaddag& gaddag) : gaddag_(&gaddag) {}

  // Request (only "rack" is required):
  //
  //   {"board": [...15 rows], "rack": "AEINRST", "opponentRack": "EIOU?",
  //    "score": 120, "opponentScore": 95, "scorelessTurns": 0,
  //    "rules": "naspa" | "wespa",
  //    "effort": "static" | "quick" | "normal" | "deep",
  //    "simulation": {"candidates", "iterations", "plies", "timeLimitMs"},
  //    "endgame": {"timeLimitMs", "nodeLimit"},
  //    "limit": 10, "seed": 1}
  //
  // "simulation" and "endgame" override the preset's budget within the
  // caps above. Response: a MoveAnalysis (analysis.h) whose "chosen"
  // candidate is the engine's move, plus
  //
  //   "effort", "elapsedMs",
  //   "position": {"unseen", "unseenCount", "bagCount", "opponentRackSize",
  //                "opponentRackKnown", "tilesOnBoard"}
  //
  // Errors: 400 for malformed fields, 422 "impossible_position" for tile
  // counts the distribution cannot supply or an opponent rack that does
  // not fit the bag, 422 "game_over" when the opponent has no tiles left,
  // 503 "busy" when another search is running too long.
  ApiResponse Analyze(std::string_view body);

 private:
  const Gaddag* gaddag_;
  // Held for the duration of each search, so searches run one at a time.
  std::timed_mutex search_;
};

}  // namespace scrabble::server

#endif  // SCRABBLE_SERVER_ANALYZE_H_
