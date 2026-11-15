#include "api.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "scrabble/gaddag.h"
#include "scrabble/lexicon.h"

namespace scrabble::server {
namespace {

using Json = nlohmann::json;

const Gaddag& SmallLexicon() {
  static const Gaddag gaddag = Gaddag::Build(
      std::vector<std::string>{"HELLO", "HE", "EH", "LO", "OH", "HO"});
  return gaddag;
}

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

class ApiTest : public ::testing::Test {
 protected:
  Api api_{SmallLexicon(), {.version = "test", .lexicon_name = "small"}};
};

std::set<std::string> Notations(const Json& moves) {
  std::set<std::string> out;
  for (const Json& move : moves) out.insert(move["notation"]);
  return out;
}

TEST_F(ApiTest, Health) {
  const ApiResponse response = api_.Health();
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.body["status"], "ok");
  EXPECT_EQ(response.body["lexicon"], "small");
  EXPECT_EQ(response.body["words"], 6);
}

TEST_F(ApiTest, MalformedRequestsAreClientErrors) {
  const auto error = [](const ApiResponse& response) {
    EXPECT_EQ(response.status, 400) << response.body.dump();
    return response.body.value("error", "");
  };
  EXPECT_EQ(error(api_.Moves("not json")), "bad_json");
  EXPECT_EQ(error(api_.Moves("[1, 2]")), "bad_json");
  EXPECT_EQ(error(api_.Moves("{}")), "missing_field");
  EXPECT_EQ(error(api_.Moves(R"({"rack": "HELLO", "board": ["..."]})")),
            "bad_board");
  EXPECT_EQ(error(api_.Moves(R"({"rack": "HELLO", "limit": 0})")),
            "bad_limit");
  EXPECT_EQ(error(api_.Moves(R"({"rack": "HELLO", "limit": "ten"})")),
            "bad_limit");
  EXPECT_EQ(error(api_.Validate(R"({"rack": "HELLO"})")), "missing_field");
  EXPECT_EQ(error(api_.Validate(R"({"move": "8H"})")), "bad_notation");
  EXPECT_EQ(error(api_.PositionSummary(R"({"rack": 7})")), "bad_rack");
}

TEST_F(ApiTest, MovesOnEmptyBoardAreSortedWithLeaves) {
  const ApiResponse response = api_.Moves(R"({"rack": "HELLOXY"})");
  ASSERT_EQ(response.status, 200) << response.body.dump();
  const Json& moves = response.body["moves"];
  EXPECT_EQ(response.body["total"], moves.size());
  EXPECT_EQ(response.body["truncated"], false);
  EXPECT_EQ(response.body["position"]["unseenCount"], 93);
  const std::set<std::string> notations = Notations(moves);
  EXPECT_TRUE(notations.contains("8D HELLO"));
  EXPECT_TRUE(notations.contains("H4 HELLO"));
  EXPECT_TRUE(notations.contains("8G EH"));
  for (std::size_t i = 1; i < moves.size(); ++i) {
    EXPECT_GE(moves[i - 1]["score"], moves[i]["score"]);
  }
  EXPECT_EQ(moves[0]["leave"], "XY");
}

TEST_F(ApiTest, MovesRespectsLimit) {
  const ApiResponse response =
      api_.Moves(R"({"rack": "HELLO", "limit": 1})");
  ASSERT_EQ(response.status, 200);
  EXPECT_EQ(response.body["moves"].size(), 1u);
  EXPECT_EQ(response.body["truncated"], true);
  EXPECT_GT(response.body["total"], 1);
}

TEST_F(ApiTest, ValidateReportsMoveErrors) {
  const ApiResponse off_centre =
      api_.Validate(R"({"rack": "HELLO", "move": "9H HELLO"})");
  EXPECT_EQ(off_centre.status, 200);
  EXPECT_EQ(off_centre.body["valid"], false);
  EXPECT_EQ(off_centre.body["error"], "first_move_off_center");
  EXPECT_EQ(off_centre.body["message"],
            "first move must cover the centre square");

  const ApiResponse missing =
      api_.Validate(R"({"rack": "HELO", "move": "8H HELLO"})");
  EXPECT_EQ(missing.body["error"], "not_in_rack");
}

TEST_F(ApiTest, ValidateScoresLegalMoves) {
  const ApiResponse response =
      api_.Validate(R"({"rack": "HLLO?XY", "move": "8H HeLLO"})");
  ASSERT_EQ(response.body["valid"], true) << response.body.dump();
  EXPECT_EQ(response.body["move"]["notation"], "8H HeLLO");
  // (H 4 + e 0 + L 1 + L 1 + O 1 on the DL) x 2 for the centre.
  EXPECT_EQ(response.body["move"]["score"], 16);
  EXPECT_EQ(response.body["move"]["leave"], "XY");
}

TEST_F(ApiTest, ValidateWithoutRackChecksOnlyBoardAndWords) {
  EXPECT_EQ(api_.Validate(R"({"move": "8D HELLO"})").body["valid"], true);
  EXPECT_EQ(api_.Validate(R"({"move": "8D HELLX"})").body["error"],
            "invalid_word");
}

TEST_F(ApiTest, PlayAppliesMoveAndReturnsLeave) {
  const ApiResponse first =
      api_.Play(R"({"rack": "HELLOXY", "move": "8D HELLO"})");
  ASSERT_EQ(first.status, 200) << first.body.dump();
  EXPECT_EQ(first.body["board"][7], "...HELLO.......");
  EXPECT_EQ(first.body["rack"], "XY");
  EXPECT_EQ(first.body["move"]["score"], 24);
  EXPECT_EQ(first.body["position"]["tilesOnBoard"], 5);

  Json next = {{"board", first.body["board"]}, {"move", "D8 (H)O"}};
  const ApiResponse second = api_.Play(next.dump());
  ASSERT_EQ(second.status, 200) << second.body.dump();
  EXPECT_EQ(second.body["board"][8], "...O...........");
  EXPECT_FALSE(second.body.contains("rack"));
}

TEST_F(ApiTest, PlayRejectsIllegalMoves) {
  const ApiResponse response = api_.Play(R"({"move": "8A HELLO"})");
  EXPECT_EQ(response.status, 422);
  EXPECT_EQ(response.body["error"], "first_move_off_center");
}

TEST_F(ApiTest, ReservedEndpoints) {
  EXPECT_EQ(Api::NotImplemented("streams").status, 501);
  EXPECT_EQ(Api::NotFound("/api/nope").status, 404);
}

TEST(ApiEnableTest, RetainsBingosOnEmptyBoard) {
  const Gaddag* gaddag = Enable();
  if (gaddag == nullptr) GTEST_SKIP() << "ENABLE word list not fetched";
  const Api api(*gaddag, {.version = "test", .lexicon_name = "enable1"});
  const ApiResponse response = api.Moves(R"({"rack": "RETAINS"})");
  ASSERT_EQ(response.status, 200);
  const std::set<std::string> notations = Notations(response.body["moves"]);
  for (const char* expected :
       {"8D RETAINS", "8H RETAINS", "8D NASTIER", "8B STAINER", "H8 RETINAS",
        "H2 RETAINS"}) {
    EXPECT_TRUE(notations.contains(expected)) << expected;
  }
  const Json& best = response.body["moves"][0];
  EXPECT_EQ(best["bingo"], true);
  EXPECT_EQ(best["tilesUsed"], 7);
  EXPECT_EQ(best["leave"], "");
}

}  // namespace
}  // namespace scrabble::server
