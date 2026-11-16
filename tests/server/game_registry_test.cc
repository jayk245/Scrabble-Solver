#include "game_registry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace scrabble::server {
namespace {

using Json = nlohmann::json;
using std::chrono::milliseconds;
using Clock = GameRegistry::Clock;

// A clock tests advance by hand, so expiry needs no sleeping.
class FakeClock {
 public:
  Clock::time_point Now() const { return now_.load(); }
  void Advance(std::chrono::seconds by) { now_.store(now_.load() + by); }
  std::function<Clock::time_point()> Function() {
    return [this] { return Now(); };
  }

 private:
  std::atomic<Clock::time_point> now_{Clock::time_point{}};
};

// Blocks until cancelled: a stand-in for a long self-play game.
void UntilCancelled(GameSession& session) {
  while (!session.cancel_requested()) {
    std::this_thread::sleep_for(milliseconds(1));
  }
  session.Publish("end", {{"reason", "cancelled"}});
}

void WaitUntilClosed(const GameSession& session) {
  std::vector<std::string> frames;
  std::size_t next = 0;
  while (true) {
    frames.clear();
    const auto result = session.Read(next, milliseconds(100), frames);
    if (result == GameSession::ReadResult::kEnd) return;
    next += frames.size();
  }
}

TEST(SseFrameTest, Format) {
  EXPECT_EQ(SseFrame(3, "move", Json{{"a", 1}, {"b", "x\ny"}}),
            "id: 3\nevent: move\ndata: {\"a\":1,\"b\":\"x\\ny\"}\n\n");
}

TEST(GameSessionTest, PublishReadAndClose) {
  GameSession session("abc", "selfplay");
  std::vector<std::string> frames;
  EXPECT_EQ(session.Read(0, milliseconds(1), frames),
            GameSession::ReadResult::kTimeout);

  session.Publish("start", {{"x", 1}});
  session.Publish("move", Json::object());
  EXPECT_EQ(session.size(), 2u);
  EXPECT_EQ(session.Read(1, milliseconds(1), frames),
            GameSession::ReadResult::kData);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0], "id: 1\nevent: move\ndata: {\"type\":\"move\"}\n\n");

  EXPECT_EQ(session.Summary()["status"], "running");
  session.Close();
  session.Publish("ignored", Json::object());
  EXPECT_EQ(session.size(), 2u);
  frames.clear();
  EXPECT_EQ(session.Read(0, milliseconds(1), frames),
            GameSession::ReadResult::kData);
  EXPECT_EQ(frames.size(), 2u);
  EXPECT_EQ(session.Read(2, milliseconds(1), frames),
            GameSession::ReadResult::kEnd);
  EXPECT_EQ(session.Summary()["status"], "finished");
}

TEST(GameSessionTest, ReadersWakeWhenEventsArrive) {
  GameSession session("abc", "selfplay");
  std::thread producer([&session] {
    std::this_thread::sleep_for(milliseconds(20));
    session.Publish("start", Json::object());
  });
  std::vector<std::string> frames;
  // Far longer than the producer's delay: returns as soon as it publishes.
  EXPECT_EQ(session.Read(0, std::chrono::seconds(30), frames),
            GameSession::ReadResult::kData);
  producer.join();
}

TEST(GameRegistryTest, RunsTasksInTheBackgroundAndClosesThem) {
  GameRegistry registry;
  const auto session =
      registry.StartBackground("selfplay", [](GameSession& game) {
        game.Publish("start", Json::object());
        game.Publish("end", Json::object());
      });
  ASSERT_NE(session, nullptr);
  WaitUntilClosed(*session);
  EXPECT_EQ(session->size(), 2u);
  EXPECT_EQ(registry.Find(session->id()), session);
  EXPECT_EQ(registry.running(), 0u);
  EXPECT_EQ(registry.Find("nope"), nullptr);
}

TEST(GameRegistryTest, ATaskThatThrowsStillCloses) {
  GameRegistry registry;
  const auto session = registry.StartBackground(
      "selfplay", [](GameSession&) { throw std::runtime_error("boom"); });
  ASSERT_NE(session, nullptr);
  WaitUntilClosed(*session);
}

TEST(GameRegistryTest, LimitsRunningGamesAndCancels) {
  GameRegistry registry({.max_games = 8, .max_running = 1});
  const auto first = registry.StartBackground("selfplay", UntilCancelled);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(registry.running(), 1u);
  EXPECT_EQ(registry.StartBackground("selfplay", UntilCancelled), nullptr);
  // Completed games do not count against the running limit.
  EXPECT_NE(registry.AddCompleted("replay", [](GameSession&) {}), nullptr);

  EXPECT_TRUE(registry.Cancel(first->id()));
  EXPECT_FALSE(registry.Cancel("nope"));
  WaitUntilClosed(*first);
  EXPECT_EQ(first->Summary()["status"], "cancelled");
  EXPECT_NE(registry.StartBackground("selfplay", UntilCancelled), nullptr);
  // The destructor cancels and joins the game still running.
}

TEST(GameRegistryTest, CancellingAFinishedGameKeepsItsStatus) {
  GameRegistry registry;
  const auto session = registry.AddCompleted("replay", [](GameSession&) {});
  EXPECT_TRUE(registry.Cancel(session->id()));
  EXPECT_EQ(session->Summary()["status"], "finished");
}

TEST(GameRegistryTest, FinishedGamesExpire) {
  FakeClock clock;
  GameRegistry registry({.finished_ttl = std::chrono::seconds(60)},
                        clock.Function());
  const auto session = registry.AddCompleted("replay", [](GameSession&) {});
  const std::string id = session->id();
  registry.Sweep();  // first seen finished now
  clock.Advance(std::chrono::seconds(59));
  EXPECT_NE(registry.Find(id), nullptr);
  clock.Advance(std::chrono::seconds(1));
  EXPECT_EQ(registry.Find(id), nullptr);
  EXPECT_EQ(registry.size(), 0u);
  // A reader holding the session keeps it alive after eviction.
  EXPECT_EQ(session->id(), id);
}

TEST(GameRegistryTest, UnwatchedRunningGamesAreCancelled) {
  FakeClock clock;
  GameRegistry registry({.idle_ttl = std::chrono::seconds(30)},
                        clock.Function());
  const auto watched = registry.StartBackground("selfplay", UntilCancelled);
  const auto unwatched = registry.StartBackground("selfplay", UntilCancelled);
  ASSERT_NE(watched, nullptr);
  ASSERT_NE(unwatched, nullptr);
  watched->AddReader();
  clock.Advance(std::chrono::seconds(31));
  registry.Sweep();
  EXPECT_FALSE(watched->cancel_requested());
  EXPECT_TRUE(unwatched->cancel_requested());
  watched->RemoveReader();
  registry.Cancel(watched->id());
}

TEST(GameRegistryTest, EvictsTheOldestFinishedGameWhenFull) {
  GameRegistry registry({.max_games = 2});
  const auto a = registry.AddCompleted("replay", [](GameSession&) {});
  const auto b = registry.AddCompleted("replay", [](GameSession&) {});
  const auto c = registry.AddCompleted("replay", [](GameSession&) {});
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(registry.size(), 2u);
  EXPECT_EQ(registry.Find(a->id()), nullptr);
  EXPECT_NE(registry.Find(b->id()), nullptr);
  EXPECT_NE(registry.Find(c->id()), nullptr);
}

TEST(GameRegistryTest, IdsAreUniqueHex) {
  GameRegistry registry({.max_games = 100});
  std::set<std::string> ids;
  for (int i = 0; i < 50; ++i) {
    const std::string id =
        registry.AddCompleted("replay", [](GameSession&) {})->id();
    EXPECT_EQ(id.find_first_not_of("0123456789abcdef"), std::string::npos);
    ids.insert(id);
  }
  EXPECT_EQ(ids.size(), 50u);
}

}  // namespace
}  // namespace scrabble::server
