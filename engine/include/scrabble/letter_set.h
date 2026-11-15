// Fixed-size set of the letters A-Z, packed into a 32-bit mask.
//
// Used wherever the engine reasons about "which letters are allowed here":
// board cross-checks, GADDAG child letters, and rack contents during move
// generation. All operations are constexpr single-instruction bit tricks so
// they can sit in the innermost search loops.

#ifndef SCRABBLE_LETTER_SET_H_
#define SCRABBLE_LETTER_SET_H_

#include <bit>
#include <cassert>
#include <cstdint>

#include "scrabble/tile.h"

namespace scrabble {

class LetterSet {
 public:
  // Bits 0-25 map to A-Z; the upper bits are always zero, so two sets with
  // the same members compare equal bitwise.
  static constexpr std::uint32_t kAllBits = (1u << kNumLetters) - 1;

  constexpr LetterSet() = default;

  // Builds a set from a raw mask. Bits above Z are discarded rather than
  // asserted on so callers can pass masks that also carry other flags.
  constexpr explicit LetterSet(std::uint32_t bits) : bits_(bits & kAllBits) {}

  static constexpr LetterSet All() { return LetterSet(kAllBits); }

  // Precondition for the per-letter operations: letter < kNumLetters.
  constexpr bool Contains(Letter letter) const {
    assert(letter < kNumLetters);
    return (bits_ >> letter) & 1u;
  }
  constexpr void Add(Letter letter) {
    assert(letter < kNumLetters);
    bits_ |= Bit(letter);
  }
  constexpr void Remove(Letter letter) {
    assert(letter < kNumLetters);
    bits_ &= ~Bit(letter);
  }

  constexpr LetterSet Union(LetterSet other) const {
    return LetterSet(bits_ | other.bits_);
  }
  constexpr LetterSet Intersect(LetterSet other) const {
    return LetterSet(bits_ & other.bits_);
  }

  constexpr bool Empty() const { return bits_ == 0; }
  constexpr int Count() const { return std::popcount(bits_); }
  constexpr std::uint32_t bits() const { return bits_; }

  constexpr LetterSet operator|(LetterSet other) const { return Union(other); }
  constexpr LetterSet operator&(LetterSet other) const {
    return Intersect(other);
  }
  constexpr bool operator==(const LetterSet&) const = default;

 private:
  static constexpr std::uint32_t Bit(Letter letter) { return 1u << letter; }

  std::uint32_t bits_ = 0;
};

}  // namespace scrabble

#endif  // SCRABBLE_LETTER_SET_H_
