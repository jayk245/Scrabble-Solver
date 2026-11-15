#include "codec.h"

#include <gtest/gtest.h>

#include <numeric>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "scrabble/gaddag.h"
#include "scrabble/move_validation.h"
#include "scrabble/position.h"

namespace scrabble::server {
namespace {

using Json = nlohmann::json;

Json EmptyBoardJson() {
  return Json(std::vector<std::string>(kBoardSize, std::string(15, '.')));
}

const Gaddag& Lexicon() {
  static const Gaddag gaddag =
      Gaddag::Build(std::vector<std::string>{"HELLO", "HE", "EH"});
  return gaddag;
}

// Parses and validates `notation`, failing the test if it is illegal.
Move Legal(const Board& board, std::string_view rack, std::string_view text) {
  Move move = MoveFromNotation(text);
  const MoveError error =
      ValidateMove(board, Lexicon(), *Rack::FromString(rack), move);
  EXPECT_EQ(error, MoveError::kOk) << text << ": " << MoveErrorMessage(error);
  return move;
}

// Runs `parse` and returns the ApiError code it throws ("" if none).
template <typename Parse>
std::string ErrorCode(Parse&& parse) {
  try {
    parse();
  } catch (const ApiError& e) {
    EXPECT_EQ(e.status(), 400);
    return e.code();
  }
  return "";
}

TEST(CodecTest, BoardRoundTripPreservesTilesAndBlanks) {
  Json json = EmptyBoardJson();
  json[7] = "...HeLLO.......";
  json[14] = "Z.............q";
  const Board board = BoardFromJson(json);
  EXPECT_EQ(board.TileCount(), 7);
  EXPECT_EQ(board.At(7, 4).letter, *CharToLetter('E'));
  EXPECT_TRUE(board.At(7, 4).is_blank);
  EXPECT_FALSE(board.At(7, 3).is_blank);
  EXPECT_TRUE(board.At(14, 14).is_blank);
  EXPECT_EQ(BoardToJson(board), json);
}

TEST(CodecTest, BoardRejectsMalformedInput) {
  EXPECT_EQ(ErrorCode([] { BoardFromJson(Json::object()); }), "bad_board");
  EXPECT_EQ(ErrorCode([] {
              BoardFromJson(Json(std::vector<std::string>(14, "")));
            }),
            "bad_board");
  for (const std::string row : {"..............", "...............A",
                                "......?........", "......1........"}) {
    Json json = EmptyBoardJson();
    json[3] = row;
    EXPECT_EQ(ErrorCode([&] { BoardFromJson(json); }), "bad_board") << row;
  }
  Json json = EmptyBoardJson();
  json[0] = 5;
  EXPECT_EQ(ErrorCode([&] { BoardFromJson(json); }), "bad_board");
}

TEST(CodecTest, BoardErrorNamesTheSquare) {
  Json json = EmptyBoardJson();
  json[2] = "..#............";
  try {
    BoardFromJson(json);
    FAIL() << "expected ApiError";
  } catch (const ApiError& e) {
    EXPECT_STREQ(e.what(), "board row 3 column C has invalid character '#'");
  }
}

TEST(CodecTest, RackParsing) {
  EXPECT_EQ(RackFromJson("retain?").ToString(), "AEINRT?");
  EXPECT_TRUE(RackFromJson("").IsEmpty());
  EXPECT_EQ(ErrorCode([] { RackFromJson("ABCDEFGH"); }), "bad_rack");
  EXPECT_EQ(ErrorCode([] { RackFromJson("AB1"); }), "bad_rack");
  EXPECT_EQ(ErrorCode([] { RackFromJson(42); }), "bad_rack");
}

TEST(CodecTest, NotationErrors) {
  EXPECT_EQ(ErrorCode([] { MoveFromNotation("8Z HELLO"); }), "bad_notation");
  EXPECT_EQ(ErrorCode([] { MoveFromNotation(""); }), "bad_notation");
  EXPECT_EQ(MoveFromNotation("8H HELLO").num_tiles, 5);
}

TEST(CodecTest, PlacementJsonListsSquaresAndLeave) {
  const Rack rack = *Rack::FromString("HLLO?XY");
  const Move move = Legal(Board{}, "HLLO?XY", "8H HeLLO");
  const Json json = MoveToJson(move, &rack);
  EXPECT_EQ(json["notation"], "8H HeLLO");
  EXPECT_EQ(json["type"], "place");
  EXPECT_EQ(json["direction"], "across");
  EXPECT_EQ(json["row"], 7);
  EXPECT_EQ(json["col"], 7);
  EXPECT_EQ(json["word"], "HeLLO");
  EXPECT_EQ(json["score"], move.score);
  EXPECT_EQ(json["tilesUsed"], 5);
  EXPECT_EQ(json["bingo"], false);
  EXPECT_EQ(json["leave"], "XY");
  ASSERT_EQ(json["tiles"].size(), 5u);
  EXPECT_EQ(json["tiles"][1],
            (Json{{"row", 7}, {"col", 8}, {"letter", "e"}, {"blank", true},
                  {"playThrough", false}}));
  EXPECT_FALSE(MoveToJson(move).contains("leave"));
}

TEST(CodecTest, PlayThroughTilesAreMarked) {
  Board board;
  PlaceTiles(board, Legal(board, "HELLO", "8H HELLO"));
  const Move move = Legal(board, "E", "H8 (H)E");
  const Json json = MoveToJson(move);
  EXPECT_EQ(json["direction"], "down");
  EXPECT_EQ(json["word"], "HE");
  EXPECT_EQ(json["tiles"][0]["playThrough"], true);
  EXPECT_EQ(json["tiles"][1]["row"], 8);
  EXPECT_EQ(json["tiles"][1]["col"], 7);
  EXPECT_EQ(json["tiles"][1]["playThrough"], false);
}

TEST(CodecTest, ExchangeAndPassJson) {
  const Rack rack = *Rack::FromString("AB?");
  const Json exchange = MoveToJson(MoveFromNotation("-A?"), &rack);
  EXPECT_EQ(exchange["type"], "exchange");
  EXPECT_EQ(exchange["word"], "A?");
  EXPECT_EQ(exchange["leave"], "B");
  EXPECT_FALSE(exchange.contains("row"));
  EXPECT_EQ(MoveToJson(Move::Pass())["notation"], "-");
}

TEST(CodecTest, UnseenTilesOnEmptyBoard) {
  const Unseen unseen = ComputeUnseen(Board{}, Rack{});
  EXPECT_EQ(unseen.total, kTotalTiles);
  EXPECT_EQ(unseen.counts[*CharToLetter('E')], 12);
  EXPECT_TRUE(unseen.overused.empty());

  const Json summary =
      PositionSummaryToJson(Board{}, *Rack::FromString("RETAINS"));
  EXPECT_EQ(summary["unseenCount"], 93);
  EXPECT_EQ(summary["bagCount"], 86);
  EXPECT_EQ(summary["unseen"]["E"], 11);
  EXPECT_EQ(summary["unseen"]["?"], 2);
}

TEST(CodecTest, UnseenCountsBlanksAndClampsOverusedLetters) {
  Json json = EmptyBoardJson();
  json[7] = "ZZ.....e.......";
  const Unseen unseen =
      ComputeUnseen(BoardFromJson(json), *Rack::FromString("?"));
  EXPECT_EQ(unseen.counts[*CharToLetter('Z')], 0);
  EXPECT_EQ(unseen.counts[kBlank], 0);
  EXPECT_EQ(unseen.counts[*CharToLetter('E')], 12);  // the blank is not an E
  EXPECT_EQ(unseen.overused, "Z");
  EXPECT_EQ(unseen.total, kTotalTiles - 1 - 2);  // one Z clamped
}

TEST(CodecTest, BagCountNeverNegative) {
  // Every lettered tile on the board leaves only the two blanks unseen.
  Board board;
  int square = 0;
  for (Letter letter = 0; letter < kNumLetters; ++letter) {
    for (int n = 0; n < kTileCounts[letter]; ++n, ++square) {
      board.Place(square / kBoardSize, square % kBoardSize, letter);
    }
  }
  const Json summary = PositionSummaryToJson(board, Rack{});
  EXPECT_EQ(summary["unseenCount"], 2);
  EXPECT_EQ(summary["bagCount"], 0);
}

TEST(CodecTest, RulesMatchEngineConstants) {
  const Json rules = RulesToJson();
  EXPECT_EQ(rules["boardSize"], kBoardSize);
  EXPECT_EQ(rules["premiums"][7], "T..d...*...d..T");
  EXPECT_EQ(rules["premiums"][1], ".D...t...t...D.");
  EXPECT_EQ(rules["tileValues"]["Q"], 10);
  EXPECT_EQ(rules["tileValues"]["?"], 0);
  int total = 0;
  for (const auto& [letter, count] : rules["tileCounts"].items()) {
    total += count.get<int>();
  }
  EXPECT_EQ(total, kTotalTiles);
}

TEST(CodecTest, MoveErrorCodesAreDistinct) {
  std::set<std::string_view> codes;
  const int last = static_cast<int>(MoveError::kInvalidCrossWord);
  for (int i = 0; i <= last; ++i) {
    const std::string_view code = MoveErrorCode(static_cast<MoveError>(i));
    EXPECT_NE(code, "unknown");
    codes.insert(code);
  }
  EXPECT_EQ(codes.size(), static_cast<std::size_t>(last + 1));
}

}  // namespace
}  // namespace scrabble::server
