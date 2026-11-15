#include "scrabble/bag.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace scrabble {
namespace {

static_assert(std::is_trivially_copyable_v<Bag>);

Letter L(char c) { return *CharToLetter(c); }

TEST(BagTest, FullBagHasStandardDistribution) {
  const Bag bag = Bag::Full();
  EXPECT_EQ(bag.Size(), 100);
  for (Letter l = 0; l < kAlphabetSize; ++l) {
    EXPECT_EQ(bag.Count(l), kTileCounts[l]) << LetterToChar(l);
  }
}

TEST(BagTest, DefaultIsEmpty) {
  Bag bag;
  EXPECT_TRUE(bag.IsEmpty());
  EXPECT_EQ(bag.Size(), 0);
}

TEST(BagTest, DrainingYieldsExactDistribution) {
  Bag bag = Bag::Full();
  Rng rng(2024);
  std::array<int, kAlphabetSize> drawn{};
  while (!bag.IsEmpty()) ++drawn[bag.Draw(rng)];
  for (std::size_t i = 0; i < drawn.size(); ++i) {
    EXPECT_EQ(drawn[i], kTileCounts[i]);
  }
}

std::vector<Letter> DrawAll(std::uint64_t seed) {
  Bag bag = Bag::Full();
  Rng rng(seed);
  std::vector<Letter> order;
  while (!bag.IsEmpty()) order.push_back(bag.Draw(rng));
  return order;
}

TEST(BagTest, DrawsAreDeterministicPerSeed) {
  EXPECT_EQ(DrawAll(17), DrawAll(17));
  EXPECT_NE(DrawAll(17), DrawAll(18));
}

TEST(BagTest, CopyDrawsIndependently) {
  Bag original = Bag::Full();
  Bag copy = original;
  Rng rng(1);
  copy.Draw(rng);
  EXPECT_EQ(original.Size(), 100);
  EXPECT_EQ(copy.Size(), 99);
}

TEST(BagTest, DrawIntoFillsRack) {
  Bag bag = Bag::Full();
  Rng rng(9);
  Rack rack = *Rack::FromString("AB");
  EXPECT_EQ(bag.DrawInto(rack, rng), 5);
  EXPECT_EQ(rack.Size(), kRackSize);
  EXPECT_EQ(bag.Size(), 95);
  EXPECT_EQ(bag.DrawInto(rack, rng), 0);  // already full
}

TEST(BagTest, DrawIntoStopsWhenBagEmpties) {
  Bag bag;
  bag.Return(L('Q'));
  bag.Return(L('I'));
  Rng rng(9);
  Rack rack;
  EXPECT_EQ(bag.DrawInto(rack, rng), 2);
  EXPECT_EQ(rack.ToString(), "IQ");
  EXPECT_TRUE(bag.IsEmpty());
}

TEST(BagTest, ExchangeConservesTiles) {
  Bag bag = Bag::Full();
  Rng rng(4);
  Rack rack;
  bag.DrawInto(rack, rng);

  // Draw replacements first, then return the old tiles.
  const Rack old_tiles = rack;
  Rack fresh;
  bag.DrawInto(fresh, rng);
  bag.Return(old_tiles);

  EXPECT_EQ(bag.Size(), 93);
  for (Letter l = 0; l < kAlphabetSize; ++l) {
    EXPECT_EQ(bag.Count(l) + fresh.Count(l), kTileCounts[l]);
  }
}

TEST(BagTest, RemoveSpecific) {
  Bag bag = Bag::Full();
  EXPECT_TRUE(bag.RemoveSpecific(L('Z')));
  EXPECT_EQ(bag.Count(L('Z')), 0);
  EXPECT_EQ(bag.Size(), 99);
  EXPECT_FALSE(bag.RemoveSpecific(L('Z')));
  EXPECT_EQ(bag.Size(), 99);

  EXPECT_TRUE(bag.RemoveSpecific(kBlank));
  EXPECT_TRUE(bag.RemoveSpecific(kBlank));
  EXPECT_FALSE(bag.RemoveSpecific(kBlank));

  // The remaining tiles must still drain to exactly the adjusted counts.
  Rng rng(5);
  std::array<int, kAlphabetSize> drawn{};
  while (!bag.IsEmpty()) ++drawn[bag.Draw(rng)];
  EXPECT_EQ(drawn[L('Z')], 0);
  EXPECT_EQ(drawn[kBlank], 0);
  EXPECT_EQ(drawn[L('E')], 12);
}

}  // namespace
}  // namespace scrabble
