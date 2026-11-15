#include "scrabble/bag.h"

#include <cassert>
#include <cstddef>

namespace scrabble {

Bag Bag::Full() {
  Bag bag;
  for (std::size_t i = 0; i < kTileCounts.size(); ++i) {
    for (int n = 0; n < kTileCounts[i]; ++n) {
      bag.Return(static_cast<Letter>(i));
    }
  }
  return bag;
}

Letter Bag::Draw(Rng& rng) {
  assert(!IsEmpty());
  const std::uint32_t index = rng.Below(size_);
  const Letter letter = tiles_[index];
  // Swap-and-pop: the last tile fills the hole. Order within the bag carries
  // no meaning, so this keeps removal O(1).
  tiles_[index] = tiles_[--size_];
  --counts_[letter];
  return letter;
}

int Bag::DrawInto(Rack& rack, Rng& rng) {
  int drawn = 0;
  while (rack.Size() < kRackSize && !IsEmpty()) {
    rack.Add(Draw(rng));
    ++drawn;
  }
  return drawn;
}

void Bag::Return(Letter letter) {
  assert(letter < kAlphabetSize);
  assert(size_ < kTotalTiles);
  tiles_[size_++] = letter;
  ++counts_[letter];
}

void Bag::Return(const Rack& rack) {
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    for (int n = rack.Count(letter); n > 0; --n) Return(letter);
  }
}

bool Bag::RemoveSpecific(Letter letter) {
  if (letter >= kAlphabetSize || counts_[letter] == 0) return false;
  for (std::size_t i = 0; i < size_; ++i) {
    if (tiles_[i] == letter) {
      tiles_[i] = tiles_[--size_];
      --counts_[letter];
      return true;
    }
  }
  // Unreachable while the counts_/tiles_ invariant holds.
  assert(false && "Bag counts out of sync with tiles");
  return false;
}

}  // namespace scrabble
