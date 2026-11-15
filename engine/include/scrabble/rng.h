// Fast, seedable pseudo-random number generator for simulation.
//
// xoshiro256** (Blackman & Vigna) seeded via splitmix64. It is much faster
// and far smaller than std::mt19937_64 (32 bytes of state vs ~2.5 KB), which
// matters because simulation keeps one generator per thread and copies game
// state frequently. Not cryptographically secure.

#ifndef SCRABBLE_RNG_H_
#define SCRABBLE_RNG_H_

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>

namespace scrabble {

// Satisfies std::uniform_random_bit_generator, so it also works with
// <random> distributions and std::shuffle where convenience beats speed.
//
// Output is fully determined by the seed, on every platform, which makes
// simulations and failing tests reproducible.
class Rng {
 public:
  using result_type = std::uint64_t;

  explicit constexpr Rng(std::uint64_t seed) { Seed(seed); }

  // Resets the generator to the state produced by `seed`. Seeds are expanded
  // through splitmix64 so that nearby seeds (0, 1, 2, ...) yield unrelated
  // streams and the all-zero state, which xoshiro cannot leave, is avoided.
  constexpr void Seed(std::uint64_t seed) {
    for (std::uint64_t& word : state_) word = SplitMix64(seed);
  }

  static constexpr result_type min() { return 0; }
  static constexpr result_type max() {
    return std::numeric_limits<result_type>::max();
  }

  constexpr result_type operator()() {
    const std::uint64_t result = std::rotl(state_[1] * 5, 7) * 9;
    const std::uint64_t t = state_[1] << 17;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= t;
    state_[3] = std::rotl(state_[3], 45);
    return result;
  }

  // Returns a uniformly distributed integer in [0, bound). Requires
  // bound > 0.
  //
  // Lemire's multiply-shift method: maps a 32-bit draw onto [0, bound) with
  // one multiplication and rejects the small biased sliver, so the common
  // path has no division (unlike `x % bound`, which is also biased).
  constexpr std::uint32_t Below(std::uint32_t bound) {
    assert(bound > 0);
    std::uint64_t product = std::uint64_t{Next32()} * bound;
    auto low = static_cast<std::uint32_t>(product);
    if (low < bound) {
      // (2^32 - bound) % bound: the count of values that would bias the
      // result. Computed only on this rare path to keep the division cold.
      const std::uint32_t threshold = (0u - bound) % bound;
      while (low < threshold) {
        product = std::uint64_t{Next32()} * bound;
        low = static_cast<std::uint32_t>(product);
      }
    }
    return static_cast<std::uint32_t>(product >> 32);
  }

  // Returns a uniformly distributed double in [0, 1) with 53 bits of
  // precision.
  constexpr double NextDouble() {
    return static_cast<double>((*this)() >> 11) * 0x1.0p-53;
  }

 private:
  // The high bits of xoshiro256** are its strongest, so 32-bit draws take
  // the top half.
  constexpr std::uint32_t Next32() {
    return static_cast<std::uint32_t>((*this)() >> 32);
  }

  static constexpr std::uint64_t SplitMix64(std::uint64_t& x) {
    std::uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  std::array<std::uint64_t, 4> state_{};
};

}  // namespace scrabble

#endif  // SCRABBLE_RNG_H_
