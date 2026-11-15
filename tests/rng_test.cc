#include "scrabble/rng.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <random>

namespace scrabble {
namespace {

static_assert(std::uniform_random_bit_generator<Rng>);

TEST(RngTest, SameSeedSameStream) {
  Rng a(42);
  Rng b(42);
  for (int i = 0; i < 1000; ++i) EXPECT_EQ(a(), b());
}

TEST(RngTest, DifferentSeedsDiverge) {
  Rng a(1);
  Rng b(2);
  int equal = 0;
  for (int i = 0; i < 100; ++i) equal += (a() == b());
  EXPECT_EQ(equal, 0);
}

TEST(RngTest, ReseedRestartsStream) {
  Rng rng(7);
  const std::uint64_t first = rng();
  rng();
  rng.Seed(7);
  EXPECT_EQ(rng(), first);
}

// Pins the output against an independent reference implementation so an
// accidental algorithm change, which would silently invalidate recorded
// seeds, fails loudly.
TEST(RngTest, MatchesReferenceOutput) {
  Rng rng(0);
  EXPECT_EQ(rng(), 0x99EC5F36CB75F2B4u);
  EXPECT_EQ(rng(), 0xBF6E1F784956452Au);
  EXPECT_EQ(rng(), 0x1A5F849D4933E6E0u);
}

TEST(RngTest, UsableInConstantExpressions) {
  constexpr std::uint64_t kFirst = [] {
    Rng rng(0);
    return rng();
  }();
  EXPECT_EQ(kFirst, 0x99EC5F36CB75F2B4u);
}

TEST(RngTest, BelowStaysInRange) {
  Rng rng(123);
  for (std::uint32_t bound : {1u, 2u, 3u, 7u, 100u, 0x80000001u, 0xFFFFFFFFu}) {
    for (int i = 0; i < 10000; ++i) EXPECT_LT(rng.Below(bound), bound);
  }
}

TEST(RngTest, BelowOneIsAlwaysZero) {
  Rng rng(5);
  for (int i = 0; i < 100; ++i) EXPECT_EQ(rng.Below(1), 0u);
}

TEST(RngTest, BelowIsRoughlyUniform) {
  constexpr std::uint32_t kBuckets = 10;
  constexpr int kDraws = 100000;
  std::array<int, kBuckets> histogram{};
  Rng rng(99);
  for (int i = 0; i < kDraws; ++i) ++histogram[rng.Below(kBuckets)];
  // Expected 10000 per bucket, sd ~95; 5% tolerance is > 5 sigma.
  for (int count : histogram) {
    EXPECT_GT(count, 9500);
    EXPECT_LT(count, 10500);
  }
}

TEST(RngTest, NextDoubleInUnitInterval) {
  Rng rng(11);
  for (int i = 0; i < 10000; ++i) {
    const double x = rng.NextDouble();
    EXPECT_GE(x, 0.0);
    EXPECT_LT(x, 1.0);
  }
}

TEST(RngTest, WorksWithStandardDistributions) {
  Rng rng(3);
  std::uniform_int_distribution<int> dist(1, 6);
  for (int i = 0; i < 1000; ++i) {
    const int roll = dist(rng);
    EXPECT_GE(roll, 1);
    EXPECT_LE(roll, 6);
  }
}

}  // namespace
}  // namespace scrabble
