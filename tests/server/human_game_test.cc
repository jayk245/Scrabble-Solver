#include "human_game.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "codec.h"
#include "game_registry.h"
#include "scrabble/gaddag.h"
#include "scrabble/lexicon.h"

namespace scrabble::server {
namespace {

using Json = nlohmann::json;
using std::chrono::milliseconds;
using Clock = HumanGames::Clock;

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

Json FrameData(const std::string& frame) {
  const std::size_t start = frame.find("data: ");
  EXPECT_NE(start, std::string::npos) << frame;
  return Json::parse(frame.substr(start + 6));
}

// Reads a session's event log as it grows.
class EventReader {
 public:
  explicit EventReader(std::shared_ptr<GameSession> session)
      : session_(std::move(session)) {}

  // Waits (up to a generous bound, for sanitizer builds) until an event
  // satisfies `done`, returning it; null JSON on timeout.
  Json WaitFor(const std::function<bool(const Json&)>& done) {
    const auto deadline = std::chrono::steady_clock::now() + milliseconds(30000);
    while (std::chrono::steady_clock::now() < deadline) {
      for (; next_ < events_.size(); ++next_) {
        if (done(events_[next_])) return events_[next_++];
      }
      std::vector<std::string> frames;
      const auto result = session_->Read(events_.size(), milliseconds(50), frames);
      for (const std::string& frame : frames) events_.push_back(FrameData(frame));
      if (result == GameSession::ReadResult::kEnd && frames.empty() &&
          next_ >= events_.size()) {
        break;
      }
    }
    return Json();
  }

  Json WaitForType(std::string_view type) {
    return WaitFor([type](const Json& e) { return e["type"] == type; });
  }

  const std::vector<Json>& events() const { return events_; }

 private:
  std::shared_ptr<GameSession> session_;
  std::vector<Json> events_;
  std::size_t next_ = 0;
};

class HumanGameTest : public ::testing::Test {
 protected:
  void SetUp() override {
    gaddag_ = Enable();
    if (gaddag_ == nullptr) GTEST_SKIP() << "ENABLE not found";
  }

  void MakeGames(HumanGamesOptions options = {}) {
    games_ = std::make_unique<HumanGames>(*gaddag_, registry_, options,
                                          [this] { return now_.load(); });
  }

  // Starts a game; returns its id.
  std::string Start(const Json& request) {
    const ApiResponse response = games_->Start(request, 77);
    EXPECT_EQ(response.status, 201) << response.body.dump();
    return response.body.value("id", "");
  }

  ApiResponse Play(const std::string& id, const Json& request) {
    return games_->Play(id, request.dump());
  }

  // The engine's top suggestion for the human.
  std::string BestMove(const std::string& id) {
    const ApiResponse hint = games_->Hint(id);
    EXPECT_EQ(hint.status, 200) << hint.body.dump();
    return hint.body["candidates"][0]["move"]["notation"];
  }

  const Gaddag* gaddag_ = nullptr;
  GameRegistry registry_;
  std::atomic<Clock::time_point> now_{Clock::time_point{}};
  std::unique_ptr<HumanGames> games_;
};

TEST(HumanGameSettingsTest, ParsesAndRejects) {
  const HumanGameSettings defaults = HumanGameSettingsFromJson(Json::object(), 9);
  EXPECT_EQ(defaults.strength, BotStrength::kNormal);
  EXPECT_TRUE(defaults.human_first);
  EXPECT_EQ(defaults.seed, 9u);
  EXPECT_FALSE(defaults.seed_given);

  const HumanGameSettings custom = HumanGameSettingsFromJson(
      {{"name", "jay"}, {"strength", "expert"}, {"humanFirst", false},
       {"seed", 3}, {"rules", "wespa"}},
      9);
  EXPECT_EQ(custom.human_name, "jay");
  EXPECT_EQ(custom.strength, BotStrength::kExpert);
  EXPECT_FALSE(custom.human_first);
  EXPECT_EQ(custom.seed, 3u);
  EXPECT_TRUE(custom.seed_given);
  EXPECT_EQ(custom.rules, EndRules::kWespa);

  for (const Json& bad : {Json{{"strength", "godlike"}}, Json{{"name", "bot"}},
                          Json{{"name", "two words"}}, Json{{"seed", -1}},
                          Json{{"humanFirst", "yes"}}}) {
    EXPECT_THROW(HumanGameSettingsFromJson(bad, 1), ApiError) << bad.dump();
  }
  EXPECT_TRUE(BotConfigFor(BotStrength::kHard).simulation.enabled);
  EXPECT_TRUE(BotConfigFor(BotStrength::kExpert).endgame.enabled);
  EXPECT_FALSE(BotConfigFor(BotStrength::kEasy).simulation.enabled);
}

// Illegal moves are rejected with the engine's reason and change nothing;
// a legal one is applied, and the bot's reply arrives as an event that
// reveals nothing about its rack.
TEST_F(HumanGameTest, ValidatesMovesAndHidesTheBotsRack) {
  MakeGames();
  const std::string id = Start({{"name", "jay"}, {"strength", "normal"}});
  EventReader reader(registry_.Find(id));
  const Json start = reader.WaitForType("start");
  ASSERT_FALSE(start.is_null());
  EXPECT_EQ(start["humanSeat"], 0);
  EXPECT_EQ(start["toMove"], 0);
  EXPECT_TRUE(start["racks"][1].is_null());
  // A random seed would let the client reconstruct the bag.
  EXPECT_FALSE(start.contains("seed"));
  const std::string rack = start["racks"][0];
  ASSERT_EQ(rack.size(), 7u);

  // A two-letter phony from the rack, placed legally through the centre.
  std::string phony;
  for (std::size_t i = 0; i < rack.size() && phony.empty(); ++i) {
    for (std::size_t j = 0; j < rack.size() && phony.empty(); ++j) {
      if (i == j || rack[i] == '?' || rack[j] == '?') continue;
      const std::string word = {rack[i], rack[j]};
      if (!gaddag_->Contains(word)) phony = word;
    }
  }
  ASSERT_FALSE(phony.empty());
  ApiResponse response = Play(id, {{"move", "8H " + phony}});
  EXPECT_EQ(response.status, 422);
  EXPECT_EQ(response.body["error"], "invalid_word");
  response = Play(id, {{"move", "1A " + phony}});
  EXPECT_EQ(response.status, 422);
  EXPECT_EQ(response.body["error"], "first_move_off_center");
  response = Play(id, {{"move", "8H QQQQQQQ"}});
  EXPECT_EQ(response.status, 422);
  EXPECT_EQ(response.body["error"], "not_in_rack");
  response = Play(id, {{"move", "nonsense"}});
  EXPECT_EQ(response.status, 400);
  EXPECT_EQ(response.body["error"], "bad_notation");
  EXPECT_EQ(Play(id, Json::object()).status, 400);
  EXPECT_EQ(games_->Play("ffffffffffff", "{}").status, 404);

  const std::string best = BestMove(id);
  response = Play(id, {{"move", best}});
  ASSERT_EQ(response.status, 200) << response.body.dump();
  EXPECT_EQ(response.body["turn"], 0);
  EXPECT_EQ(response.body["rack"].get<std::string>().size(), 7u);

  const Json human_move = reader.WaitForType("move");
  EXPECT_EQ(human_move["player"], 0);
  EXPECT_EQ(human_move["rack"], rack);
  EXPECT_TRUE(human_move.contains("drawn"));
  EXPECT_TRUE(human_move["move"].contains("leave"));

  const Json bot_move = reader.WaitForType("move");
  ASSERT_FALSE(bot_move.is_null());
  EXPECT_EQ(bot_move["player"], 1);
  EXPECT_EQ(bot_move["toMove"], 0);
  EXPECT_TRUE(bot_move["rack"].is_null());
  EXPECT_TRUE(bot_move["racks"][1].is_null());
  EXPECT_FALSE(bot_move.contains("drawn"));
  EXPECT_FALSE(bot_move.contains("analysis"));
  EXPECT_FALSE(bot_move["move"].contains("leave"));
  // No GCG (which lists every rack) until the game ends.
  EXPECT_EQ(registry_.Find(id)->gcg(), "");

  // The end event reveals both racks and the GCG.
  response = Play(id, {{"resign", true}});
  EXPECT_EQ(response.status, 200);
  const Json end = reader.WaitForType("end");
  EXPECT_EQ(end["reason"], "resigned");
  EXPECT_EQ(end["winner"], 1);
  EXPECT_EQ(end["seed"], 77);
  EXPECT_EQ(end["racks"][1].get<std::string>().size(), 7u);
  EXPECT_NE(end["gcg"].get<std::string>().find(">bot:"), std::string::npos);
  ASSERT_EQ(end["review"].size(), 1u);
  EXPECT_EQ(end["review"][0]["equityLost"], 0.0);
  EXPECT_EQ(end["hintsUsed"], 1);
  EXPECT_EQ(Play(id, {{"move", "-"}}).body["error"], "game_over");
}

// With no thinking slot free the bot can never reply, which pins the game
// on the bot's turn: the human must wait, and cancelling still ends it.
TEST_F(HumanGameTest, EnforcesTurnOrder) {
  MakeGames({.max_thinking = 0});
  const std::string id =
      Start({{"humanFirst", false}, {"seed", 5}, {"strength", "easy"}});
  EventReader reader(registry_.Find(id));
  const Json start = reader.WaitForType("start");
  EXPECT_EQ(start["humanSeat"], 1);
  EXPECT_EQ(start["toMove"], 0);
  EXPECT_EQ(start["seed"], 5);
  EXPECT_TRUE(start["racks"][0].is_null());

  EXPECT_EQ(Play(id, {{"move", "-"}}).body["error"], "not_your_turn");
  EXPECT_EQ(games_->Hint(id).status, 409);

  EXPECT_TRUE(games_->Cancel(id));
  const Json end = reader.WaitForType("end");
  EXPECT_EQ(end["reason"], "cancelled");
  EXPECT_TRUE(end["winner"].is_null());
  EXPECT_EQ(registry_.Find(id)->Summary()["status"], "cancelled");
}

// A whole game against the greedy bot, the human always taking the
// engine's top suggestion: every human move is reviewed with no equity
// lost, and the game ends normally with consistent scores.
TEST_F(HumanGameTest, PlaysAGameToTheEnd) {
  MakeGames();
  const std::string id = Start({{"strength", "easy"}, {"seed", 11}});
  EventReader reader(registry_.Find(id));
  ASSERT_FALSE(reader.WaitForType("start").is_null());
  int human_moves = 0;
  Json end;
  // The human (seat 0) moves first; after each move, wait for the bot's
  // reply or the end of the game.
  while (end.is_null() && human_moves < 100) {
    const ApiResponse response = Play(id, {{"move", BestMove(id)}});
    ASSERT_EQ(response.status, 200) << response.body.dump();
    ++human_moves;
    const Json next = reader.WaitFor([](const Json& e) {
      return e["type"] == "end" || (e["type"] == "move" && e["player"] == 1);
    });
    ASSERT_FALSE(next.is_null());
    if (next["type"] == "end") {
      end = next;
    } else if (next["toMove"].is_null()) {
      end = reader.WaitForType("end");
    }
  }
  ASSERT_FALSE(end.is_null());
  EXPECT_TRUE(end["reason"] == "went_out" || end["reason"] == "scoreless_turns")
      << end["reason"];
  ASSERT_EQ(end["review"].size(), static_cast<std::size_t>(human_moves));
  for (const Json& entry : end["review"]) {
    EXPECT_EQ(entry["equityLost"], 0.0) << entry.dump();
    EXPECT_EQ(entry["rank"], 1);
  }
  const int spread = end["scores"][0].get<int>() - end["scores"][1].get<int>();
  EXPECT_EQ(end["winner"], spread > 0 ? 0 : spread < 0 ? 1 : -1);
  EXPECT_FALSE(registry_.Find(id)->gcg().empty());
}

TEST_F(HumanGameTest, AbandonsIdleGamesAndLimitsOpenOnes) {
  MakeGames({.max_open = 1, .idle_ttl = std::chrono::seconds(60)});
  const std::string id = Start(Json::object());
  EXPECT_EQ(games_->Start(Json::object(), 1).status, 503);

  now_.store(now_.load() + std::chrono::seconds(61));
  games_->Sweep();
  EventReader reader(registry_.Find(id));
  EXPECT_EQ(reader.WaitForType("end")["reason"], "abandoned");
  // The finished game no longer counts against the limit.
  EXPECT_NE(Start(Json::object()), "");
}

}  // namespace
}  // namespace scrabble::server
