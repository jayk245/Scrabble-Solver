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

// Every line ToGcg() writes parses back to the history it came from.
TEST_F(GcgTest, ParseRoundTripsExport) {
  for (const EndRules rules : {EndRules::kNaspa, EndRules::kWespa}) {
    const Game game = PlayScript(gaddag_, "EEEEEEE", going_out_, rules);
    const GcgGame parsed = ParseGcg(ToGcg(game, Info()));
    EXPECT_EQ(parsed.info.players[0].nickname, "alice");
    EXPECT_EQ(parsed.info.players[0].full_name, "Alice Smith");
    EXPECT_EQ(parsed.info.players[1].full_name, "bob");

    const std::vector<MoveRecord>& history = game.history();
    ASSERT_GE(parsed.events.size(), history.size());
    for (std::size_t i = 0; i < history.size(); ++i) {
      const GcgEvent& event = parsed.events[i];
      EXPECT_EQ(event.type, GcgEventType::kMove);
      EXPECT_EQ(event.player, history[i].player);
      EXPECT_EQ(event.rack, history[i].rack_before);
      EXPECT_EQ(event.score, history[i].score);
      EXPECT_EQ(event.cumulative, history[i].cumulative);
      EXPECT_EQ(event.line, static_cast<int>(i) + 4);
      ASSERT_TRUE(event.move.has_value());
      EXPECT_EQ(event.move->type, history[i].move.type);
    }
    // "C6 AB." keeps its play-through square unresolved.
    const Move& last = *parsed.events[3].move;
    EXPECT_EQ(last.num_tiles, 3);
    EXPECT_EQ(last.tiles[2].letter(), MoveTile::kUnknownLetter);

    const GcgEvent& out = parsed.events[history.size()];
    EXPECT_EQ(out.type, GcgEventType::kEndPoints);
    EXPECT_EQ(out.player, 1);
    EXPECT_EQ(out.tiles, RackOrDie("VVVVVVV"));
    EXPECT_FALSE(out.rack.has_value());
    EXPECT_EQ(out.cumulative, game.score(1));
    if (rules == EndRules::kWespa) {
      ASSERT_EQ(parsed.events.size(), history.size() + 2);
      const GcgEvent& penalty = parsed.events.back();
      EXPECT_EQ(penalty.type, GcgEventType::kRackPenalty);
      EXPECT_EQ(penalty.score, -28);
      EXPECT_EQ(penalty.cumulative, game.score(0));
    }
  }
}

TEST(GcgParseTest, ReadsOtherToolsConventions) {
  const GcgGame game = ParseGcg(
      "#character-encoding UTF-8\r\n"
      "#player1 jo Jo March\r\n"
      "#player2 beth Beth\r\n"
      "#description ignored pragma\r\n"
      "#lexicon CSW21\r\n"
      "\r\n"
      ">jo: ADEHILN 8D HALIDE +30 30\r\n"
      "#note first note\r\n"
      "#note second note\r\n"
      ">beth: CEIORTU 9C OUtRICE +70 70\r\n"
      ">beth: CEIORTU -- -70 0\r\n"
      ">jo: ADNQSTU (challenge) +5 35\r\n"
      ">jo: ADNQSTU -7 +0 35\r\n"
      ">beth: CEIORTU - +0 0\r\n"
      ">jo: DGT (DGT) -5 30\r\n"
      ">beth: (time) -10 -10\r\n"
      ">jo: 10D .Q +11 41\r\n");
  EXPECT_EQ(game.info.players[0].full_name, "Jo March");
  EXPECT_EQ(game.info.lexicon, "CSW21");
  ASSERT_EQ(game.events.size(), 9u);

  EXPECT_EQ(game.events[0].note, "first note\nsecond note");
  EXPECT_EQ(game.events[1].move->ToString(), "9C OUtRICE");
  EXPECT_EQ(game.events[2].type, GcgEventType::kWithdrawn);
  EXPECT_EQ(game.events[2].score, -70);
  EXPECT_EQ(game.events[3].type, GcgEventType::kChallengeBonus);
  EXPECT_EQ(game.events[3].player, 0);
  EXPECT_EQ(game.events[4].exchange_count, 7);
  EXPECT_FALSE(game.events[4].move.has_value());
  EXPECT_EQ(game.events[5].move->type, MoveType::kPass);
  EXPECT_EQ(game.events[6].type, GcgEventType::kRackPenalty);
  EXPECT_EQ(game.events[6].tiles, RackOrDie("DGT"));
  EXPECT_EQ(game.events[7].type, GcgEventType::kTimePenalty);
  EXPECT_EQ(game.events[7].cumulative, -10);
  // No rack field: a coordinate first means a placement.
  EXPECT_FALSE(game.events[8].rack.has_value());
  EXPECT_EQ(game.events[8].move->direction, Direction::kAcross);
  EXPECT_EQ(game.events[8].line, 17);
}

TEST(GcgParseTest, UndeclaredPlayersTakeSeatsInOrder) {
  const GcgGame game = ParseGcg(
      ">p: AB 8H AB +8 8\n"
      ">q: CD - +0 0\n");
  EXPECT_EQ(game.info.players[0].nickname, "p");
  EXPECT_EQ(game.info.players[1].nickname, "q");
  EXPECT_EQ(game.events[1].player, 1);
}

TEST(GcgParseTest, ReportsTheOffendingLine) {
  const auto error_line = [](std::string_view text) {
    try {
      ParseGcg(text);
    } catch (const GcgParseError& e) {
      return e.line();
    }
    return 0;
  };
  EXPECT_THROW(ParseGcg(""), GcgParseError);
  EXPECT_EQ(error_line("#player1 a A\n>a: AB 8H AB +8\n"), 2);
  EXPECT_EQ(error_line("#player1 a A\n>a: AB 8H AB +x 8\n"), 2);
  EXPECT_EQ(error_line("#player1 a A\n\n>a: A!B 8H AB +8 8\n"), 3);
  EXPECT_EQ(error_line("#player1 a A\n>a: AB 8Z AB +8 8\n"), 2);
  EXPECT_EQ(error_line("#player1 a A\n>a: AB -9 +0 0\n"), 2);
  EXPECT_EQ(error_line("#player1 a A\nhello\n"), 2);
  EXPECT_EQ(error_line(">a: AB - +0 0\n>b: AB - +0 0\n>c: AB - +0 0\n"), 3);
  EXPECT_EQ(error_line("#player1\n"), 1);
  EXPECT_THROW(ParseGcg("just text"), GcgParseError);
}

}  // namespace
}  // namespace scrabble
