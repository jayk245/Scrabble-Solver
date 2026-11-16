// Encoding of the engine's move analysis, shared by every endpoint that
// explains a decision: the game viewer's move events (games.h), the
// human-vs-bot game's hints and post-game review (human_game.h) and the
// position solver (analyze.h). Keeping one encoder means the client renders
// all of them with the same components (see MoveAnalysis in
// web/src/api/types.ts).

#ifndef SCRABBLE_SERVER_ANALYSIS_H_
#define SCRABBLE_SERVER_ANALYSIS_H_

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "scrabble/endgame.h"
#include "scrabble/leave_evaluator.h"
#include "scrabble/move.h"
#include "scrabble/rack.h"
#include "scrabble/simulation.h"

namespace scrabble::server {

enum class LeaveKind : std::uint8_t { kLearned, kHeuristic, kZero };

std::string_view LeaveKindName(LeaveKind kind);

// The shared, immutable leave table for `kind` (built once per process).
std::shared_ptr<const LeaveEvaluator> LeavesFor(LeaveKind kind);

// Two decimals is far below any meaningful equity difference and keeps
// responses compact.
double Round2(double value);

// True if two moves are the same play. Equity is whatever the last
// evaluator wrote, so it is ignored.
bool SameMove(Move a, Move b);

// Zero-based rank of `move` in `ranked` (sorted by equity, best first), or
// -1 if absent. Moves of equal equity share the best rank among them, so
// the rank does not depend on how the sort ordered ties.
int RankOf(const std::vector<Move>& ranked, const Move& move);

// {"move", "equity", "staticRank": 1-based | null, "chosen"}.
nlohmann::json StaticCandidateJson(const Move& move, const Rack& rack, int rank,
                                   bool chosen);

// The analysis of one decision:
//
//   {"method": "static" | "simulation" | "endgame",
//    "totalMoves": n, "playedRank": r | null, "playedEquity": e | null,
//    "candidates": [{"move", "equity", "staticRank", "chosen",
//                    "sim"?: {"mean", "stdError", "stddev", "iterations",
//                             "pruned", "diffFromBest", "diffStdError"}}],
//    "simulation"?: {"iterations", "rollouts", "seconds", "stop", "plies"},
//    "endgame"?: {"value", "complete", "depth", "nodes", "seconds", "pv"}}
//
// `ranked` is every legal move in static order and `played` the move that
// was (or, for the solver, should be) played; "chosen" marks it. Candidates
// are the simulated moves (best mean first) when `simulation` is given,
// otherwise the top `limit` static moves. With an endgame result the
// solver's move is listed first even if the static ranking puts it lower.
nlohmann::json AnalysisJson(const std::vector<Move>& ranked, const Rack& rack,
                            const Move& played, int limit,
                            const SimulationResult* simulation, int plies,
                            const EndgameResult* endgame);

}  // namespace scrabble::server

#endif  // SCRABBLE_SERVER_ANALYSIS_H_
