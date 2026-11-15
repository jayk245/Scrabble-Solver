#include "scrabble/gcg.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "test_util.h"

namespace scrabble {
namespace {

using testing::ParseOrDie;
using testing::RackOrDie;

constexpr char kHeader[] =
    "#character-encoding UTF-8\n"
    "#player1 alice Alice Smith\n"
    "#player2 bob bob\n";

GcgInfo Info() {
  GcgInfo info;
  info.players[0] = {"alice", "Alice Smith"};
  info.players[1] = {"bob", ""};
  return info;
}

Game PlayScript(const Gaddag& gaddag, std::string_view bag,
                const std::vector<std::string>& moves, EndRules rules) {
  GameSetup setup;
  setup.racks = {RackOrDie("VVVVVVV"), RackOrDie("AB")};
  for (char c : bag) setup.bag.Return(*CharToLetter(c));
  Game game(gaddag, setup, 5, {.end_rules = rules});
  for (const std::string& move : moves) {
    EXPECT_TRUE(game.Play(ParseOrDie(move)).ok()) << move;
  }
  return game;
}

class GcgTest : public ::testing::Test {
 protected:
  const Gaddag gaddag_ =
      Gaddag::Build(std::vector<std::string>{"EEEEEEE", "ABE"});
  const std::vector<std::string> going_out_ = {"-VVVVVVV", "-", "8B EEEEEEE",
                                               "C6 AB(E)"};
};

TEST_F(GcgTest, GoingOutNaspa) {
  const Game game =
      PlayScript(gaddag_, "EEEEEEE", going_out_, EndRules::kNaspa);
  EXPECT_EQ(ToGcg(game, Info()),
            std::string(kHeader) +
                ">alice: VVVVVVV -VVVVVVV +0 0\n"
                ">bob: AB - +0 0\n"
                ">alice: EEEEEEE 8B EEEEEEE +66 66\n"
                ">bob: AB C6 AB. +8 8\n"
                ">bob: (VVVVVVV) +56 64\n");
}

TEST_F(GcgTest, GoingOutWespa) {
  const Game game =
      PlayScript(gaddag_, "EEEEEEE", going_out_, EndRules::kWespa);
  EXPECT_EQ(ToGcg(game, Info()),
            std::string(kHeader) +
                ">alice: VVVVVVV -VVVVVVV +0 0\n"
                ">bob: AB - +0 0\n"
                ">alice: EEEEEEE 8B EEEEEEE +66 66\n"
                ">bob: AB C6 AB. +8 8\n"
                ">bob: (VVVVVVV) +28 36\n"
                ">alice: VVVVVVV (VVVVVVV) -28 38\n");
}

TEST_F(GcgTest, SixScorelessTurnsAndPragmas) {
  const Game game = PlayScript(gaddag_, "", {"-", "-", "-", "-", "-", "-"},
                               EndRules::kNaspa);
  GcgInfo info = Info();
  info.title = "six zeros";
  info.lexicon = "TEST";
  EXPECT_EQ(ToGcg(game, info),
            std::string(kHeader) +
                "#title six zeros\n"
                "#lexicon TEST\n"
                ">alice: VVVVVVV - +0 0\n"
                ">bob: AB - +0 0\n"
                ">alice: VVVVVVV - +0 0\n"
                ">bob: AB - +0 0\n"
                ">alice: VVVVVVV - +0 0\n"
                ">bob: AB - +0 0\n"
                ">alice: VVVVVVV (VVVVVVV) -28 -28\n"
                ">bob: AB (AB) -4 -4\n");
}

TEST_F(GcgTest, InProgressGameHasNoEndLines) {
  const Game game = PlayScript(gaddag_, "EEEEEEE", {"-"}, EndRules::kNaspa);
  EXPECT_EQ(ToGcg(game, Info()),
            std::string(kHeader) + ">alice: VVVVVVV - +0 0\n");
}

TEST_F(GcgTest, RejectsBadNicknames) {
  const Game game = PlayScript(gaddag_, "", {}, EndRules::kNaspa);
  GcgInfo info = Info();
  info.players[1].nickname = "two words";
  EXPECT_THROW(ToGcg(game, info), std::invalid_argument);
  info.players[1].nickname = "";
  EXPECT_THROW(ToGcg(game, info), std::invalid_argument);
}

}  // namespace
}  // namespace scrabble
