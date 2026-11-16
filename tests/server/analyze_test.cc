#include "analyze.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "codec.h"
#include "scrabble/bot.h"
#include "scrabble/gaddag.h"
#include "scrabble/game.h"
#include "scrabble/lexicon.h"
#include "scrabble/rng.h"

namespace scrabble::server {
namespace {

using Json = nlohmann::json;

// The ENABLE GADDAG, or nullptr if the word list has not been fetched.
const Gaddag* Enable() {
  static const std::unique_ptr<Gaddag> gaddag = [] {
    const auto path =
        std::filesystem::path(SCRABBLE_LEXICA_DIR) / "enable1.txt";
    return std::filesystem::exists(path)
               ? std::make_unique<Gaddag>(LoadOrBuildGaddag(path))
               : nullptr;
  }();
  return gaddag.get();
}

// A board of 15 empty rows with `rows` written from the top.
Json BoardWith(const std::vector<std::string>& rows) {
  Json board = Json::array();
  for (int row = 0; row < 15; ++row) {
    std::string text(15, '.');
    if (static_cast<std::size_t>(row) < rows.size()) {
      text.replace(0, rows[static_cast<std::size_t>(row)].size(),
                   rows[static_cast<std::size_t>(row)]);
    }
    board.push_back(text);
  }
  return board;
}

class AnalyzeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    gaddag_ = Enable();
    if (gaddag_ == nullptr) GTEST_SKIP() << "ENABLE not found";
    api_ = std::make_unique<AnalyzeApi>(*gaddag_);
  }

  ApiResponse Analyze(const Json& request) {
    return api_->Analyze(request.dump());
  }

  std::string ErrorOf(const Json& request) {
    const ApiResponse response = Analyze(request);
    EXPECT_GE(response.status, 400) << response.body.dump();
    return response.body.value("error", "");
  }

  // A static self-play game from `seed`, stopped as soon as the bag is
  // empty (or, with `to_end`, when the game is over).
  Game PlayUntilBagEmpty(std::uint64_t seed, bool to_end = false) const {
    Game game(*gaddag_, seed);
    Bot bot(*gaddag_, BotConfig{});
    Rng rng(seed);
    while (!game.IsOver() && (to_end || game.bag().Size() > 0)) {
      EXPECT_TRUE(game.Play(bot.ChooseMove(game.ViewForMover(), rng)).ok());
    }
    return game;
  }

  const Gaddag* gaddag_ = nullptr;
  std::unique_ptr<AnalyzeApi> api_;
};

TEST_F(AnalyzeTest, RejectsBadAndImpossiblePositions) {
  EXPECT_EQ(ErrorOf(Json::object()), "missing_field");
  EXPECT_EQ(ErrorOf({{"rack", ""}}), "missing_field");
  EXPECT_EQ(ErrorOf({{"rack", "AB1"}}), "bad_rack");
  EXPECT_EQ(ErrorOf({{"rack", "AB"}, {"effort", "max"}}), "bad_settings");
  EXPECT_EQ(ErrorOf({{"rack", "AB"}, {"simulation", {{"iterations", 1}}}}),
            "bad_settings");
  EXPECT_EQ(ErrorOf({{"rack", "AB"}, {"simulation", {{"timeLimitMs", 60000}}}}),
            "bad_settings");
  EXPECT_EQ(ErrorOf({{"rack", "AB"}, {"scorelessTurns", 6}}), "bad_settings");

  // One Z in the bag, but one on the board and one on the rack.
  EXPECT_EQ(ErrorOf({{"board", BoardWith({"", "", "", "", "", "", "", ".......ZA"})},
                     {"rack", "Z"}}),
            "impossible_position");
  // Two Zs between the board and the opponent's rack.
  EXPECT_EQ(ErrorOf({{"board", BoardWith({"", "", "", "", "", "", "", ".......ZA"})},
                     {"rack", "AB"}, {"opponentRack", "ZEEEEEE"}}),
            "impossible_position");
  // With tiles in the bag the opponent must hold a full rack.
  EXPECT_EQ(ErrorOf({{"rack", "AB"}, {"opponentRack", "EEE"}}),
            "impossible_position");
}

TEST_F(AnalyzeTest, StaticRankingOfAnOpeningRack) {
  const ApiResponse response =
      Analyze({{"rack", "AEINRST"}, {"effort", "static"}, {"limit", 5}});
  ASSERT_EQ(response.status, 200) << response.body.dump();
  const Json& body = response.body;
  EXPECT_EQ(body["method"], "static");
  EXPECT_EQ(body["effort"], "static");
  ASSERT_EQ(body["candidates"].size(), 5u);
  EXPECT_EQ(body["candidates"][0]["chosen"], true);
  EXPECT_TRUE(body["candidates"][0]["move"]["bingo"].get<bool>());
  EXPECT_EQ(body["position"]["unseenCount"], 93);
  EXPECT_EQ(body["position"]["bagCount"], 86);
  EXPECT_EQ(body["position"]["opponentRackSize"], 7);
  EXPECT_EQ(body["position"]["opponentRackKnown"], false);
}

// A tiny simulation budget keeps this fast under the sanitizers while
// still exercising the rollout statistics.
TEST_F(AnalyzeTest, SimulatesCandidatesWithAKnownOpponentRack) {
  const ApiResponse response =
      Analyze({{"board", BoardWith({"", "", "", "", "", "", "", ".......CAT"})},
               {"rack", "QUIZERS"},
               {"opponentRack", "AEIOUXY"},
               {"score", 10},
               {"opponentScore", 10},
               {"effort", "quick"},
               {"simulation", {{"candidates", 3}, {"iterations", 16},
                               {"plies", 1}}}});
  ASSERT_EQ(response.status, 200) << response.body.dump();
  const Json& body = response.body;
  EXPECT_EQ(body["method"], "simulation");
  EXPECT_EQ(body["position"]["opponentRackKnown"], true);
  EXPECT_EQ(body["position"]["bagCount"], 100 - 3 - 7 - 7);
  EXPECT_EQ(body["simulation"]["plies"], 1);
  ASSERT_GE(body["candidates"].size(), 3u);
  int chosen = 0;
  for (const Json& candidate : body["candidates"]) {
    ASSERT_TRUE(candidate.contains("sim")) << candidate.dump();
    EXPECT_GE(candidate["sim"]["iterations"].get<int>(), 1);
    chosen += candidate["chosen"].get<bool>() ? 1 : 0;
  }
  EXPECT_EQ(chosen, 1);
  EXPECT_EQ(body["candidates"][0]["chosen"], true);
}

// "played" evaluates a move the engine would not choose: statically it gets
// its rank and equity, and in a simulation it is always among the
// simulated candidates, so a review can measure what it gave up.
TEST_F(AnalyzeTest, EvaluatesThePlayedMove) {
  const Json board = BoardWith({"", "", "", "", "", "", "", ".......CAT"});
  const ApiResponse ranked = Analyze({{"board", board},
                                      {"rack", "QUIZERS"},
                                      {"effort", "static"},
                                      {"limit", 30},
                                      {"played", "I6 QU(A)I"}});
  ASSERT_EQ(ranked.status, 200) << ranked.body.dump();
  const Json& played = ranked.body["played"];
  ASSERT_TRUE(played.is_object()) << ranked.body.dump();
  EXPECT_EQ(played["move"]["notation"], "I6 QU(A)I");
  EXPECT_EQ(played["chosen"], false);
  EXPECT_GT(played["staticRank"].get<int>(), 1);
  EXPECT_LT(played["equity"].get<double>(),
            ranked.body["candidates"][0]["equity"].get<double>());
  EXPECT_FALSE(played.contains("sim"));

  const ApiResponse simulated =
      Analyze({{"board", board},
               {"rack", "QUIZERS"},
               {"effort", "quick"},
               {"simulation", {{"candidates", 2}, {"iterations", 16},
                               {"plies", 1}}},
               {"played", "I6 QU(A)I"}});
  ASSERT_EQ(simulated.status, 200) << simulated.body.dump();
  EXPECT_EQ(simulated.body["method"], "simulation");
  ASSERT_TRUE(simulated.body["played"].contains("sim"))
      << simulated.body.dump();
  bool listed = false;
  for (const Json& candidate : simulated.body["candidates"]) {
    listed = listed || candidate["move"]["notation"] == "I6 QU(A)I";
  }
  EXPECT_TRUE(listed);

  // A move the rack cannot make is reported rather than refused.
  const ApiResponse illegal = Analyze({{"board", board},
                                       {"rack", "QUIZERS"},
                                       {"effort", "static"},
                                       {"played", "8A XYST"}});
  ASSERT_EQ(illegal.status, 200) << illegal.body.dump();
  EXPECT_TRUE(illegal.body["played"].is_null());
  EXPECT_FALSE(illegal.body["playedError"].get<std::string>().empty());
  EXPECT_EQ(ErrorOf({{"rack", "AB"}, {"played", 7}}), "bad_settings");
}

// A client that has gone away is not searched for; static answers need no
// search slot and are unaffected.
TEST_F(AnalyzeTest, SkipsCancelledSearches) {
  const auto gone = [] { return true; };
  const ApiResponse skipped = api_->Analyze(
      Json{{"rack", "AEINRST"}, {"effort", "quick"}}.dump(), gone);
  EXPECT_EQ(skipped.status, 499);
  EXPECT_EQ(skipped.body["error"], "cancelled");
  const ApiResponse quick = api_->Analyze(
      Json{{"rack", "AEINRST"}, {"effort", "static"}}.dump(), gone);
  EXPECT_EQ(quick.status, 200) << quick.body.dump();
}

// With the bag empty the opponent's rack is the unseen tiles, so the
// endgame solver answers, whether or not the client names that rack.
TEST_F(AnalyzeTest, SolvesEmptyBagPositionsWithTheEndgameSolver) {
  const Game game = PlayUntilBagEmpty(21);
  ASSERT_FALSE(game.IsOver());
  const int mover = game.to_move();
  const Json request = {
      {"board", BoardToJson(game.board())},
      {"rack", game.rack(mover).ToString()},
      {"score", game.score(mover)},
      {"opponentScore", game.score(1 - mover)},
      {"effort", "quick"},
      {"endgame", {{"nodeLimit", 20000}}}};
  const ApiResponse response = Analyze(request);
  ASSERT_EQ(response.status, 200) << response.body.dump();
  const Json& body = response.body;
  EXPECT_EQ(body["method"], "endgame");
  EXPECT_EQ(body["position"]["bagCount"], 0);
  EXPECT_EQ(body["position"]["opponentRackSize"],
            game.rack(1 - mover).Size());
  ASSERT_TRUE(body.contains("endgame"));
  EXPECT_GE(body["endgame"]["depth"].get<int>(), 1);
  const Json& best = body["candidates"][0];
  EXPECT_EQ(best["chosen"], true);
  EXPECT_EQ(body["endgame"]["pv"][0], best["move"]["notation"]);

  Json known = request;
  known["opponentRack"] = game.rack(1 - mover).ToString();
  const ApiResponse with_rack = Analyze(known);
  ASSERT_EQ(with_rack.status, 200) << with_rack.body.dump();
  EXPECT_EQ(with_rack.body["endgame"]["value"], body["endgame"]["value"]);
  EXPECT_EQ(with_rack.body["candidates"][0]["move"]["notation"],
            best["move"]["notation"]);
}

TEST_F(AnalyzeTest, ReportsAFinishedGame) {
  const Game game = PlayUntilBagEmpty(21, /*to_end=*/true);
  ASSERT_EQ(game.status(), GameStatus::kWentOut);
  const int loser = 1 - game.went_out();
  EXPECT_EQ(ErrorOf({{"board", BoardToJson(game.board())},
                     {"rack", game.rack(loser).ToString()}}),
            "game_over");
}

}  // namespace
}  // namespace scrabble::server
