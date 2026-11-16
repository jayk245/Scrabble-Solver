#include "analysis.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

#include "codec.h"

namespace scrabble::server {
namespace {

using Json = nlohmann::json;

std::string_view StopName(SimulationStop stop) {
  switch (stop) {
    case SimulationStop::kSingleCandidate: return "single_candidate";
    case SimulationStop::kIterations: return "iterations";
    case SimulationStop::kTime: return "time";
  }
  return "iterations";
}

Json SimulatedCandidates(const std::vector<Move>& ranked, const Rack& rack,
                         const Move& played,
                         const SimulationResult& simulation) {
  std::vector<const CandidateStats*> order;
  order.reserve(simulation.candidates.size());
  for (const CandidateStats& stats : simulation.candidates) {
    order.push_back(&stats);
  }
  std::stable_sort(order.begin(), order.end(),
                   [](const CandidateStats* a, const CandidateStats* b) {
                     return a->mean > b->mean;
                   });
  Json candidates = Json::array();
  for (const CandidateStats* stats : order) {
    Json candidate =
        StaticCandidateJson(stats->move, rack, RankOf(ranked, stats->move),
                            SameMove(stats->move, played));
    candidate["sim"] = {{"mean", Round2(stats->mean)},
                        {"stdError", Round2(stats->std_error)},
                        {"stddev", Round2(stats->stddev)},
                        {"iterations", stats->iterations},
                        {"pruned", stats->pruned},
                        {"diffFromBest", Round2(stats->diff_from_best)},
                        {"diffStdError", Round2(stats->diff_std_error)}};
    candidates.push_back(std::move(candidate));
  }
  return candidates;
}

}  // namespace

std::string_view LeaveKindName(LeaveKind kind) {
  switch (kind) {
    case LeaveKind::kLearned: return "learned";
    case LeaveKind::kHeuristic: return "heuristic";
    case LeaveKind::kZero: return "zero";
  }
  return "learned";
}

std::shared_ptr<const LeaveEvaluator> LeavesFor(LeaveKind kind) {
  // Built once and shared by every bot, like DefaultLeaves().
  static const auto heuristic =
      std::make_shared<const LeaveEvaluator>(LeaveEvaluator::Heuristic());
  static const auto zero =
      std::make_shared<const LeaveEvaluator>(LeaveEvaluator::Zero());
  switch (kind) {
    case LeaveKind::kLearned: return DefaultLeaves();
    case LeaveKind::kHeuristic: return heuristic;
    case LeaveKind::kZero: return zero;
  }
  return DefaultLeaves();
}

double Round2(double value) { return std::round(value * 100.0) / 100.0; }

bool SameMove(Move a, Move b) {
  a.equity = 0.0;
  b.equity = 0.0;
  return a == b;
}

int RankOf(const std::vector<Move>& ranked, const Move& move) {
  for (std::size_t i = 0; i < ranked.size(); ++i) {
    if (!SameMove(ranked[i], move)) continue;
    std::size_t rank = i;
    while (rank > 0 && ranked[rank - 1].equity == ranked[i].equity) --rank;
    return static_cast<int>(rank);
  }
  return -1;
}

Json StaticCandidateJson(const Move& move, const Rack& rack, int rank,
                         bool chosen) {
  return {{"move", MoveToJson(move, &rack)},
          {"equity", Round2(move.equity)},
          {"staticRank", rank < 0 ? Json(nullptr) : Json(rank + 1)},
          {"chosen", chosen}};
}

Json AnalysisJson(const std::vector<Move>& ranked, const Rack& rack,
                  const Move& played, int limit,
                  const SimulationResult* simulation, int plies,
                  const EndgameResult* endgame) {
  const int played_rank = RankOf(ranked, played);
  Json analysis = {
      {"method", simulation != nullptr ? "simulation"
                 : endgame != nullptr  ? "endgame"
                                       : "static"},
      {"totalMoves", ranked.size()},
      {"playedRank", played_rank < 0 ? Json(nullptr) : Json(played_rank + 1)},
      {"playedEquity",
       played_rank < 0
           ? Json(nullptr)
           : Json(Round2(ranked[static_cast<std::size_t>(played_rank)]
                             .equity))},
  };

  Json candidates = Json::array();
  if (simulation != nullptr) {
    candidates = SimulatedCandidates(ranked, rack, played, *simulation);
    analysis["simulation"] = {{"iterations", simulation->iterations},
                              {"rollouts", simulation->rollouts},
                              {"seconds", simulation->seconds},
                              {"stop", StopName(simulation->stop)},
                              {"plies", plies}};
  } else {
    const auto count = static_cast<std::size_t>(std::max(limit, 0));
    // The solver's move is the answer, so it leads the list; the static
    // alternatives follow in their own order.
    if (endgame != nullptr && played_rank >= 0 && count > 0) {
      candidates.push_back(StaticCandidateJson(
          ranked[static_cast<std::size_t>(played_rank)], rack, played_rank,
          /*chosen=*/true));
    }
    for (std::size_t i = 0; i < ranked.size() && candidates.size() < count;
         ++i) {
      const bool chosen = SameMove(ranked[i], played);
      if (endgame != nullptr && chosen && played_rank >= 0) continue;
      candidates.push_back(
          StaticCandidateJson(ranked[i], rack, static_cast<int>(i), chosen));
    }
  }
  analysis["candidates"] = std::move(candidates);

  if (endgame != nullptr) {
    Json pv = Json::array();
    for (const Move& move : endgame->pv) pv.push_back(move.ToString());
    analysis["endgame"] = {{"value", endgame->value},
                           {"complete", endgame->complete},
                           {"depth", endgame->depth},
                           {"nodes", endgame->nodes},
                           {"seconds", endgame->seconds},
                           {"pv", std::move(pv)}};
  }
  return analysis;
}

}  // namespace scrabble::server
