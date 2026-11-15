#include "scrabble/move.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace scrabble {
namespace {

Letter L(char c) { return *CharToLetter(c); }

// Parses `text`, checks it re-renders identically, and returns the move.
Move RoundTrip(const std::string& text) {
  const std::optional<Move> move = Move::Parse(text);
  EXPECT_TRUE(move.has_value()) << text;
  if (!move) return Move{};
  EXPECT_EQ(move->ToString(), text);
  EXPECT_EQ(Move::Parse(move->ToString()), move);
  return *move;
}

TEST(MoveTest, AcrossPlacement) {
  const Move move = RoundTrip("8H HELLO");
  EXPECT_EQ(move.type, MoveType::kPlace);
  EXPECT_EQ(move.direction, Direction::kAcross);
  EXPECT_EQ(move.row, 7);
  EXPECT_EQ(move.col, 7);
  EXPECT_EQ(move.num_tiles, 5);
  EXPECT_EQ(move.TilesUsed(), 5);
  EXPECT_EQ(move.tiles[0], MoveTile::Placed(L('H')));
}

TEST(MoveTest, DownPlacement) {
  const Move move = RoundTrip("H8 HELLO");
  EXPECT_EQ(move.direction, Direction::kDown);
  EXPECT_EQ(move.row, 7);
  EXPECT_EQ(move.col, 7);
}

TEST(MoveTest, TwoDigitRowAndEdges) {
  const Move move = RoundTrip("15A QI");
  EXPECT_EQ(move.row, 14);
  EXPECT_EQ(move.col, 0);
  RoundTrip("O1 ZA");
  RoundTrip("1A ABCDEFG");
}

TEST(MoveTest, Blanks) {
  const Move move = RoundTrip("8H HeLLo");
  EXPECT_FALSE(move.tiles[0].is_blank());
  EXPECT_TRUE(move.tiles[1].is_blank());
  EXPECT_EQ(move.tiles[1].letter(), L('E'));
  EXPECT_TRUE(move.tiles[4].is_blank());
}

TEST(MoveTest, PlayThrough) {
  const Move move = RoundTrip("8D QU(I)T");
  EXPECT_EQ(move.num_tiles, 4);
  EXPECT_EQ(move.TilesUsed(), 3);
  EXPECT_TRUE(move.tiles[2].is_play_through());
  EXPECT_EQ(move.tiles[2].letter(), L('I'));

  const Move multi = RoundTrip("A1 (RE)TAIN(e)RS");
  EXPECT_EQ(multi.num_tiles, 9);
  EXPECT_EQ(multi.TilesUsed(), 6);
  EXPECT_TRUE(multi.tiles[6].is_play_through());
  EXPECT_TRUE(multi.tiles[6].is_blank());
}

TEST(MoveTest, DotPlaceholderPlayThrough) {
  const Move move = RoundTrip("8D QU.T");
  EXPECT_TRUE(move.tiles[2].is_play_through());
  EXPECT_EQ(move.tiles[2].letter(), MoveTile::kUnknownLetter);
  EXPECT_EQ(move.TilesUsed(), 3);
}

TEST(MoveTest, Exchange) {
  const Move move = RoundTrip("-ABC?");
  EXPECT_EQ(move.type, MoveType::kExchange);
  EXPECT_EQ(move.TilesUsed(), 4);
  EXPECT_EQ(move.tiles[3].letter(), kBlank);

  EXPECT_EQ(Move::Parse("exch ABC?"), move);
  EXPECT_EQ(Move::Parse("EXCH ABC?"), move);
  EXPECT_EQ(Move::Exchange(*Rack::FromString("?CBA")), move);
}

TEST(MoveTest, Pass) {
  const Move move = RoundTrip("-");
  EXPECT_EQ(move.type, MoveType::kPass);
  EXPECT_EQ(move.TilesUsed(), 0);
  EXPECT_EQ(Move::Parse("pass"), Move::Pass());
  EXPECT_EQ(Move::Parse("PASS"), Move::Pass());
}

TEST(MoveTest, ToleratesExtraWhitespace) {
  EXPECT_EQ(Move::Parse("  8H   HELLO \n"), Move::Parse("8H HELLO"));
}

TEST(MoveTest, BuiltMoveRendersNotation) {
  Move move = Move::Place(7, 3, Direction::kAcross);
  move.AddTile(MoveTile::Placed(L('Q')));
  move.AddTile(MoveTile::Placed(L('U'), /*is_blank=*/true));
  move.AddTile(MoveTile::PlayThrough(L('I')));
  move.AddTile(MoveTile::Placed(L('T')));
  EXPECT_EQ(move.ToString(), "8D Qu(I)T");
}

TEST(MoveTest, RejectsInvalid) {
  for (const char* text : {
           "",             // empty
           "   ",          // whitespace only
           "8H",           // no word
           "HELLO",        // no coordinate
           "8H HELLO X",   // trailing token
           "0H HELLO",     // row out of range
           "16H HELLO",    // row out of range
           "08H HELLO",    // leading zero
           "8P HELLO",     // column out of range
           "88 HELLO",     // no column
           "HH HELLO",     // no row
           "8H8 HELLO",    // garbage coordinate
           "8H HEL1O",     // non-letter
           "8H HEL?O",     // undesignated blank on board
           "8H (HELLO",    // unclosed group
           "8H HEL)LO",    // unmatched close
           "8H HE()LLO",   // empty group
           "8H H((E))LLO", // nested group
           "8H (.)ELLO",   // dot inside group
           "8H (HELLO)",   // no tiles from the rack
           "8H ABCDEFGH",  // 8 rack tiles
           "8K HELLOS",    // runs off the board (K..O is 5 squares)
           "A11 HELLOS",   // runs off the board downwards
           "-ABCDEFGH",    // exchange of 8
           "-AB1",         // bad exchange tile
           "exch",         // exchange with no tiles
           "--",           // malformed
       }) {
    EXPECT_FALSE(Move::Parse(text).has_value()) << '"' << text << '"';
  }
}

TEST(MoveTest, AcceptsLongPlayThroughWord) {
  // 15 squares across with 7 rack tiles is legal.
  RoundTrip("1A (ABCD)EF(GH)IJ(KL)MN(O)");
}

}  // namespace
}  // namespace scrabble
