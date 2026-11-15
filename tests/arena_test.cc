#include "scrabble/arena.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "test_util.h"

namespace scrabble {
namespace {

using testing::EnableGaddag;

TEST(ArenaTest, GameSeedsAreDistinct) {
  EXPECT_NE(ArenaGameSeed(1, 0), ArenaGameSeed(1, 1));
  EXPECT_NE(ArenaGameSeed(1, 0), ArenaGameSeed(2, 0));
  EXPECT_EQ(ArenaGameSeed(7, 3), ArenaGameSeed(7, 3));
}

TEST(ArenaTest, SummarizeCountsAndAverages) {
  std::vector<ArenaGame> games(4);
  games[0].score = {400, 300};
  games[0].bingos = {2, 0};
  games[1].score = {350, 350};
  games[2].score = {300, 400};
  games[2].bingos = {0, 1};
  games[3].score = {450, 350};
  games[3].bingos = {1, 1};
  for (ArenaGame& game : games) game.turns = 20;
  const ArenaSummary s = Summarize(games);
  EXPECT_EQ(s.games, 4);
  EXPECT_EQ(s.wins, 2);
  EXPECT_EQ(s.losses, 1);
  EXPECT_EQ(s.ties, 1);
  EXPECT_DOUBLE_EQ(s.win_rate, 2.5 / 4);
  EXPECT_DOUBLE_EQ(s.mean_score_a, 375.0);
  EXPECT_DOUBLE_EQ(s.mean_score_b, 350.0);
  EXPECT_DOUBLE_EQ(s.mean_spread, 25.0);
  // Spreads 100, 0, -100, 100 deviate from their mean of 25 by 75, 25, 125
  // and 75.
  const double variance = (75.0 * 75 + 25.0 * 25 + 125.0 * 125 + 75.0 * 75) / 3;
  EXPECT_NEAR(s.mean_spread_ci, 1.96 * std::sqrt(variance / 4), 1e-9);
  EXPECT_DOUBLE_EQ(s.bingos_per_game_a, 0.75);
  EXPECT_DOUBLE_EQ(s.bingos_per_game_b, 0.5);
  EXPECT_DOUBLE_EQ(s.turns_per_game, 20.0);
  EXPECT_EQ(Summarize({}).games, 0);
}

class EnableArenaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    gaddag_ = EnableGaddag();
    if (gaddag_ == nullptr) GTEST_SKIP() << "ENABLE not found";
  }

  ArenaConfig Config() const {
    ArenaConfig config;
    config.games = 4 + 2 * testing::kScale;
    config.seed = 77;
    config.bot_a.name = "heuristic";
    config.bot_b.name = "zero";
    config.bot_b.leaves =
        std::make_shared<const LeaveEvaluator>(LeaveEvaluator::Zero());
    return config;
  }

  const Gaddag* gaddag_ = nullptr;
};

TEST_F(EnableArenaTest, ResultsAndLogIndependentOfThreadCount) {
  ArenaConfig config = Config();
  std::ostringstream log1;
  config.threads = 1;
  config.log = &log1;
  const ArenaResult one = RunArena(*gaddag_, config);

  std::ostringstream log4;
  config.threads = 4;
  config.log = &log4;
  const ArenaResult four = RunArena(*gaddag_, config);

  EXPECT_EQ(one.games, four.games);
  EXPECT_EQ(log1.str(), log4.str());
  EXPECT_FALSE(log1.str().empty());

  config.seed = 78;
  config.log = nullptr;
  EXPECT_NE(RunArena(*gaddag_, config).games, one.games);
}

TEST_F(EnableArenaTest, ConfigsAlternateSeatsAndGamesFinish) {
  const ArenaResult result = RunArena(*gaddag_, Config());
  ASSERT_EQ(result.games.size(), static_cast<std::size_t>(Config().games));
  for (std::size_t i = 0; i < result.games.size(); ++i) {
    const ArenaGame& game = result.games[i];
    EXPECT_EQ(game.index, static_cast<int>(i));
    EXPECT_EQ(game.a_seat, static_cast<int>(i % 2));
    EXPECT_NE(game.end, GameStatus::kInProgress);
    EXPECT_GT(game.turns, 0);
  }
  EXPECT_EQ(result.summary.games, Config().games);
}

TEST_F(EnableArenaTest, LogHasOneConsistentLinePerMove) {
  ArenaConfig config = Config();
  config.threads = 3;
  std::ostringstream log;
  config.log = &log;
  const ArenaResult result = RunArena(*gaddag_, config);

  int expected_lines = 0;
  for (const ArenaGame& game : result.games) expected_lines += game.turns;
  std::istringstream in(log.str());
  std::string line;
  int lines = 0;
  while (std::getline(in, line)) {
    ++lines;
    ASSERT_EQ(line.front(), '{');
    ASSERT_EQ(line.back(), '}');
    for (const char* field :
         {"\"game\":", "\"seed\":77", "\"turn\":", "\"player\":", "\"bot\":",
          "\"rack\":", "\"move\":", "\"type\":", "\"score\":", "\"equity\":",
          "\"leave\":", "\"bag\":", "\"spread\":", "\"final_spread\":"}) {
      ASSERT_NE(line.find(field), std::string::npos) << field << " in " << line;
    }
  }
  EXPECT_EQ(lines, expected_lines);

  // Game 0's first line: A moves first, and its final spread is A's.
  std::istringstream first(log.str());
  std::getline(first, line);
  EXPECT_NE(line.find("\"game\":0,"), std::string::npos);
  EXPECT_NE(line.find("\"turn\":0,\"player\":0,\"bot\":\"A\""),
            std::string::npos);
  EXPECT_NE(line.find("\"spread\":0,\"final_spread\":" +
                      std::to_string(result.games[0].spread()) + "}"),
            std::string::npos)
      << line;
}

TEST_F(EnableArenaTest, WritesOneGcgPerGame) {
  ArenaConfig config = Config();
  config.games = 3;
  config.threads = 2;
  config.gcg_dir =
      std::filesystem::path(::testing::TempDir()) / "arena_gcg_test";
  std::filesystem::remove_all(config.gcg_dir);
  RunArena(*gaddag_, config);
  int files = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(config.gcg_dir)) {
    EXPECT_EQ(entry.path().extension(), ".gcg");
    ++files;
  }
  EXPECT_EQ(files, 3);
  EXPECT_TRUE(
      std::filesystem::exists(config.gcg_dir / "game_000002.gcg"));
  std::filesystem::remove_all(config.gcg_dir);
}

}  // namespace
}  // namespace scrabble
