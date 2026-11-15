// A player's rack, stored as a multiset of tile counts.
//
// Counts rather than an ordered list because move generation only ever asks
// "do I hold an X?" and "take one X", both O(1) here, and because two racks
// holding the same tiles in a different order compare equal for free, which
// makes racks usable directly as leave-table keys.

#ifndef SCRABBLE_RACK_H_
#define SCRABBLE_RACK_H_

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "scrabble/tile.h"

namespace scrabble {

// Invariant: Size() equals the sum of all counts and never exceeds
// kRackSize. Trivially copyable (28 bytes), so search code can save and
// restore it by value instead of undoing changes.
class Rack {
 public:
  Rack() = default;

  // Parses tiles such as "AEINST?". Letters may be either case; '?' is a
  // blank. Returns std::nullopt on any other character or if the string
  // holds more than kRackSize tiles. Order is irrelevant.
  static std::optional<Rack> FromString(std::string_view text);

  // Requires Size() < kRackSize and `letter` to be A-Z or kBlank.
  void Add(Letter letter) {
    assert(letter < kAlphabetSize);
    assert(size_ < kRackSize);
    ++counts_[letter];
    ++size_;
  }

  // Requires Contains(letter).
  void Remove(Letter letter) {
    assert(Contains(letter));
    --counts_[letter];
    --size_;
  }

  int Count(Letter letter) const { return counts_[letter]; }
  bool Contains(Letter letter) const { return counts_[letter] != 0; }
  int Size() const { return size_; }
  bool IsEmpty() const { return size_ == 0; }
  void Clear() { *this = Rack{}; }

  // Sum of tile face values; blanks count zero. Used for endgame
  // adjustments (the player going out gains the opponent's rack value).
  int Score() const {
    int total = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
      total += counts_[i] * kTileValues[i];
    }
    return total;
  }

  // Canonical form: letters in alphabetical order, blanks last, e.g.
  // "AEINST?". Equal racks always produce equal strings.
  std::string ToString() const;

  bool operator==(const Rack&) const = default;

 private:
  std::array<std::uint8_t, kAlphabetSize> counts_{};
  std::uint8_t size_ = 0;
};

}  // namespace scrabble

#endif  // SCRABBLE_RACK_H_
