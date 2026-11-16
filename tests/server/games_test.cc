#include "games.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "codec.h"
#include "game_registry.h"
#include "scrabble/gaddag.h"
#include "scrabble/lexicon.h"

namespace scrabble::server {
namespace {

using Json = nlohmann::json;
using std::chrono::milliseconds;

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

const Gaddag& Tiny() {
  static const Gaddag gaddag =
      Gaddag::Build(std::vector<std::string>{"AB", "BA"});
  return gaddag;
}

// The data of one SSE frame ("id: ..\nevent: ..\ndata: {..}\n\n").
Json FrameData(const std::string& frame) {
  const std::size_t start = frame.find("data: ");
  EXPECT_NE(start, std::string::npos) << frame;
  return Json::parse(frame.substr(start + 6));
}

// Every event a closed session holds, waiting for the producer if needed.
std::vector<Json> AllEvents(const GameSession& session) {
  std::vector<std::string> frames;
  std::size_t next = 0;
  while (session.Read(next, milliseconds(200), frames) !=
         GameSession::ReadResult::kEnd) {
    next = frames.size();
  }
  std::vector<Json> events;
  for (const std::string& frame : frames) events.push_back(FrameData(frame));
  return events;
}

std::string SettingsError(const Json& request) {
  try {
    SelfPlaySettingsFromJson(request, 1);
  } catch (const ApiError& e) {
    EXPECT_EQ(e.status(), 400);
    EXPECT_EQ(e.code(), "bad_settings");
    return e.what();
  }
  return "";
}

TEST(SelfPlaySettingsTest, Defaults) {
  const SelfPlaySettings settings = SelfPlaySettingsFromJson(Json::object(), 7);
  EXPECT_EQ(settings.seed, 7u);
  EXPECT_EQ(settings.rules, EndRules::kNaspa);
  EXPECT_EQ(settings.players[0].name, "bot1");
  EXPECT_EQ(settings.players[1].name, "bot2");
  EXPECT_EQ(settings.players[0].leaves, LeaveKind::kLearned);
  EXPECT_FALSE(settings.players[0].simulate);
  EXPECT_FALSE(settings.players[1].endgame);
  EXPECT_EQ(settings.analysis_candidates, kDefaultAnalysisCandidates);
}

TEST(SelfPlaySettingsTest, RoundTripsThroughTheStartEvent) {
  const Json request = {
      {"seed", 123},
      {"rules", "wespa"},
      {"analysisCandidates", 5},
      {"players",
       {{{"name", "sim"},
         {"leaves", "heuristic"},
         {"simulation",
          {{"enabled", true}, {"candidates", 4}, {"iterations", 64},
           {"plies", 1}}},
         {"endgame", {{"enabled", true}, {"nodeLimit", 5000}}}},
        {{"name", "greedy"}, {"leaves", "zero"}}}}};
  const SelfPlaySettings settings = SelfPlaySettingsFromJson(request, 1);
  EXPECT_EQ(settings.seed, 123u);
  EXPECT_EQ(settings.rules, EndRules::kWespa);
  EXPECT_TRUE(settings.players[0].simulate);
  EXPECT_EQ(settings.players[0].iterations, 64);
  EXPECT_EQ(settings.players[0].endgame_nodes, 5000);
  EXPECT_EQ(settings.players[1].leaves, LeaveKind::kZero);

  const Json echoed = SelfPlaySettingsToJson(settings);
  const SelfPlaySettings again = SelfPlaySettingsFromJson(echoed, 1);
  EXPECT_EQ(SelfPlaySettingsToJson(again), echoed);

  const BotConfig config = MakeBotConfig(settings.players[0]);
  EXPECT_EQ(config.name, "sim");
  EXPECT_TRUE(config.simulation.enabled);
  EXPECT_EQ(config.simulation.threads, kSimulationThreadsPerGame);
  EXPECT_EQ(config.simulation.min_iterations, 64);
  EXPECT_EQ(config.endgame.node_limit, 5000);
}

TEST(SelfPlaySettingsTest, RejectsOutOfRangeAndMalformedValues) {
  const auto player = [](const Json& fields) {
    return Json{{"players", {fields, Json::object()}}};
  };
  EXPECT_NE(SettingsError({{"seed", -1}}).find("seed"), std::string::npos);
  EXPECT_NE(SettingsError({{"seed", 1ULL << 60}}), "");
  EXPECT_NE(SettingsError({{"seed", "7"}}), "");
  EXPECT_NE(SettingsError({{"rules", "tournament"}}), "");
  EXPECT_NE(SettingsError({{"players", Json::array()}}), "");
  EXPECT_NE(SettingsError({{"analysisCandidates", 0}}), "");
  EXPECT_NE(
      SettingsError(player({{"simulation", {{"iterations", 100000}}}}))
          .find("players[0].simulation.iterations must be an integer in "
                "16..1000"),
      std::string::npos);
  EXPECT_NE(SettingsError(player({{"simulation", {{"candidates", 0}}}})), "");
  EXPECT_NE(SettingsError(player({{"simulation", {{"plies", 9}}}})), "");
  EXPECT_NE(SettingsError(player({{"simulation", {{"enabled", 1}}}})), "");
  EXPECT_NE(SettingsError(player({{"simulation", true}})), "");
  EXPECT_NE(SettingsError(player({{"endgame", {{"nodeLimit", 1}}}})), "");
  EXPECT_NE(SettingsError(player({{"leaves", "best"}})), "");
  EXPECT_NE(SettingsError(player({{"name", "two words"}})), "");
  EXPECT_NE(SettingsError(player({{"name", ""}})), "");
  EXPECT_NE(SettingsError(player({{"name", "bot2"}})).find("different"),
            std::string::npos);
}

// Checks the invariants every move event satisfies and returns the events.
void ExpectWellFormedStream(const std::vector<Json>& events) {
  ASSERT_GE(events.size(), 3u);
  EXPECT_EQ(events.front()["type"], "start");
  EXPECT_EQ(events.back()["type"], "end");
  for (std::size_t i = 1; i + 1 < events.size(); ++i) {
    const Json& event = events[i];
    EXPECT_EQ(event["type"], "move");
    EXPECT_EQ(event["turn"], i - 1);
    EXPECT_EQ(event["board"].size(), 15u);
    EXPECT_EQ(event["scores"].size(), 2u);
    EXPECT_EQ(event["racks"].size(), 2u);
    EXPECT_TRUE(event.contains("label"));
    EXPECT_TRUE(event.contains("bag"));
  }
}

TEST(SelfPlayTest, StreamsAWholeGameThatReplaysIdentically) {
  const Gaddag* gaddag = Enable();
  if (gaddag == nullptr) GTEST_SKIP() << "ENABLE not found";
  SelfPlaySettings settings = SelfPlaySettingsFromJson(
      {{"seed", 11},
       {"rules", "wespa"},
       {"players",
        {{{"name", "solver"},
          {"endgame", {{"enabled", true}, {"nodeLimit", 2000}}}},
         {{"name", "static"}, {"leaves", "heuristic"}}}}},
      1);
  GameSession session("test", "selfplay");
  RunSelfPlay(*gaddag, settings, session);
  session.Close();
  const std::vector<Json> events = AllEvents(session);
  ExpectWellFormedStream(events);

  const Json& start = events.front();
  EXPECT_EQ(start["seed"], 11);
  EXPECT_EQ(start["bag"], 86);
  EXPECT_EQ(start["racks"][0].get<std::string>().size(), 7u);

  bool saw_endgame = false;
  int previous_bag = 86;
  for (std::size_t i = 1; i + 1 < events.size(); ++i) {
    const Json& event = events[i];
    const int player = event["player"];
    EXPECT_EQ(event["kind"], "play");
    EXPECT_EQ(event["label"], event["move"]["notation"]);
    EXPECT_LE(event["bag"].get<int>(), previous_bag);
    previous_bag = event["bag"];
    const Json& analysis = event["analysis"];
    ASSERT_TRUE(analysis.is_object());
    EXPECT_GE(analysis["totalMoves"].get<int>(), 1);
    EXPECT_FALSE(analysis["candidates"].empty());
    // The static bot plays its top static move, so it is always ranked 1.
    if (player == 1) EXPECT_EQ(analysis["playedRank"], 1);
    if (analysis["method"] == "endgame") {
      saw_endgame = true;
      EXPECT_EQ(player, 0);
      EXPECT_GE(analysis["endgame"]["depth"].get<int>(), 1);
      EXPECT_EQ(analysis["endgame"]["pv"][0], event["move"]["notation"]);
    }
  }
  EXPECT_TRUE(saw_endgame);

  const Json& end = events.back();
  EXPECT_NE(end["reason"], "cancelled");
  EXPECT_EQ(end["gcg"], session.gcg());
  EXPECT_EQ(end["turns"], events.size() - 2);

  // The exported record replays to the same boards and final scores.
  const std::vector<GameEvent> replay =
      ReplayEvents(*gaddag, end["gcg"].get<std::string>());
  ASSERT_EQ(replay.size(), events.size());
  for (std::size_t i = 1; i + 1 < events.size(); ++i) {
    const Json& recorded = replay[i].data;
    EXPECT_EQ(recorded["board"], events[i]["board"]) << i;
    EXPECT_EQ(recorded["scores"], events[i]["scores"]) << i;
    EXPECT_EQ(recorded["rack"], events[i]["rack"]) << i;
    EXPECT_EQ(recorded["racks"], events[i]["racks"]) << i;
    EXPECT_EQ(recorded["bag"], events[i]["bag"]) << i;
    EXPECT_FALSE(recorded.contains("warning")) << recorded.dump();
    EXPECT_EQ(recorded["analysis"]["method"], "static");
  }
  EXPECT_EQ(replay.back().data["scores"], end["scores"]);
  EXPECT_EQ(replay.back().data["adjustments"], end["adjustments"]);
  EXPECT_EQ(replay.back().data["reason"], end["reason"]);
}

TEST(GamesApiTest, SimulatedMovesCarryStatisticsAndGamesCancel) {
  const Gaddag* gaddag = Enable();
  if (gaddag == nullptr) GTEST_SKIP() << "ENABLE not found";
  GameRegistry registry;
  GamesApi games(*gaddag, registry);
  const ApiResponse started = games.Start(
      R"({"mode": "selfplay", "seed": 3, "players": [
            {"simulation": {"enabled": true, "candidates": 3,
                            "iterations": 16, "plies": 1}},
            {}]})");
  ASSERT_EQ(started.status, 201) << started.body.dump();
  EXPECT_EQ(started.body["seed"], 3);
  const std::string id = started.body["id"];
  EXPECT_EQ(started.body["events"], "/api/games/" + id + "/events");
  const std::shared_ptr<GameSession> session = games.Find(id);
  ASSERT_NE(session, nullptr);

  // Wait for the first move (player 0, simulated), then cancel.
  std::vector<std::string> frames;
  while (frames.size() < 2) {
    frames.clear();
    ASSERT_NE(session->Read(0, milliseconds(200), frames),
              GameSession::ReadResult::kEnd);
  }
  const Json move = FrameData(frames[1]);
  const Json& analysis = move["analysis"];
  EXPECT_EQ(analysis["method"], "simulation");
  EXPECT_EQ(analysis["simulation"]["plies"], 1);
  ASSERT_GE(analysis["candidates"].size(), 3u);
  int chosen = 0;
  double previous_mean = 1e9;
  for (const Json& candidate : analysis["candidates"]) {
    const Json& sim = candidate["sim"];
    EXPECT_LE(sim["mean"].get<double>(), previous_mean);
    previous_mean = sim["mean"];
    EXPECT_GE(sim["stdError"].get<double>(), 0.0);
    EXPECT_LE(sim["iterations"].get<int>(), 16);
    if (candidate["chosen"] == true) ++chosen;
  }
  EXPECT_EQ(chosen, 1);
  EXPECT_EQ(analysis["candidates"][0]["chosen"], true);

  EXPECT_EQ(games.Cancel(id).status, 200);
  const std::vector<Json> events = AllEvents(*session);
  EXPECT_EQ(events.back()["reason"], "cancelled");
  EXPECT_EQ(games.Status(id).body["status"], "cancelled");
  EXPECT_FALSE(session->gcg().empty());
  EXPECT_EQ(games.Cancel("ffff").status, 404);
  EXPECT_EQ(games.Status("ffff").status, 404);
}

TEST(GamesApiTest, RejectsBadRequestsAndReportsWhenBusy) {
  GameRegistry registry({.max_running = 0});
  GamesApi games(Tiny(), registry);
  const auto error = [&games](std::string_view body) {
    const ApiResponse response = games.Start(body);
    return std::to_string(response.status) + " " +
           response.body.value("error", "");
  };
  EXPECT_EQ(error("nope"), "400 bad_json");
  EXPECT_EQ(error(R"({"mode": "chess"})"), "400 bad_settings");
  EXPECT_EQ(error(R"({"mode": "replay"})"), "400 missing_field");
  EXPECT_EQ(error(R"({"mode": "replay", "gcg": "hello"})"), "400 bad_gcg");
  EXPECT_EQ(error(R"({"mode": "replay", "gcg": "#player1 a A\n"})"),
            "400 bad_gcg");
  EXPECT_EQ(error(R"({"mode": "replay", "gcg": ">a: AB 8A AB +4 4\n"})"),
            "422 illegal_gcg_move");
  EXPECT_EQ(error(R"({"mode": "replay", "gcg": ">a: AB H8 .B +4 4\n"})"),
            "422 illegal_gcg_move");
  EXPECT_EQ(error(R"({"mode": "replay", "gcg": ">a: AB -- +0 0\n"})"),
            "422 illegal_gcg_move");
  EXPECT_EQ(error(R"({"mode": "selfplay"})"), "503 too_many_games");
  // Replays are computed up front and are not limited by max_running.
  EXPECT_EQ(error(R"({"mode": "replay", "gcg": ">a: AB 8H AB +8 8\n"})"),
            "201 ");
}

TEST(ReplayTest, AnnotatesRecordsFromOtherSources) {
  const Gaddag* gaddag = Enable();
  if (gaddag == nullptr) GTEST_SKIP() << "ENABLE not found";
  const std::string gcg =
      "#player1 ann Ann\n"
      "#player2 ben Ben\n"
      ">ann: EHLLOXY 8D HELLO +24 24\n"
      "#note a fine start\n"
      ">ben: ADEIRTU H8 .ADT +10 10\n"
      ">ben: ADEIRTU -- -10 0\n"
      ">ann: ?ABCLXY -XY +0 24\n"
      ">ben: ADEIRTU (challenge) +5 5\n"
      ">ann: ?ABCDEF 9E AB +99 123\n";
  const std::vector<GameEvent> events = ReplayEvents(*gaddag, gcg);
  // start, six move events, end.
  ASSERT_EQ(events.size(), 8u);
  const Json& start = events[0].data;
  EXPECT_EQ(start["players"][1]["fullName"], "Ben");
  EXPECT_EQ(start["racks"][0], "EHLLOXY");
  EXPECT_EQ(start["racks"][1], "ADEIRTU");

  const Json& hello = events[1].data;
  EXPECT_EQ(hello["move"]["leave"], "XY");
  EXPECT_EQ(hello["note"], "a fine start");
  EXPECT_FALSE(hello.contains("warning"));
  EXPECT_EQ(hello["racks"][0], "ABCLXY?");
  // 100 tiles - 5 on the board - two full racks.
  EXPECT_EQ(hello["bag"], 81);
  ASSERT_TRUE(hello["analysis"].is_object());
  EXPECT_FALSE(hello["analysis"]["playedRank"].is_null());

  const Json& phony = events[2].data;
  EXPECT_TRUE(phony.contains("warning"));
  EXPECT_EQ(phony["move"]["notation"], "H8 (O)ADT");

  const Json& withdrawn = events[3].data;
  EXPECT_EQ(withdrawn["kind"], "withdrawn");
  EXPECT_TRUE(withdrawn["move"].is_null());
  EXPECT_EQ(withdrawn["board"], hello["board"]);
  EXPECT_EQ(withdrawn["scores"], Json::array({24, 0}));

  EXPECT_EQ(events[4].data["move"]["type"], "exchange");
  EXPECT_EQ(events[5].data["kind"], "adjustment");
  EXPECT_EQ(events[5].data["label"], "(challenge) +5");

  // The recorded score is kept, with the discrepancy reported.
  const Json& last = events[6].data;
  EXPECT_EQ(last["score"], 99);
  EXPECT_NE(last["warning"].get<std::string>().find("recorded +99"),
            std::string::npos);
  EXPECT_TRUE(last["racks"][0].is_null());

  const Json& end = events.back().data;
  EXPECT_EQ(events.back().type, "end");
  EXPECT_EQ(end["reason"], "record_end");
  EXPECT_EQ(end["scores"], Json::array({123, 5}));
  EXPECT_EQ(end["winner"], 0);
  EXPECT_EQ(end["gcg"], gcg);
}

}  // namespace
}  // namespace scrabble::server
