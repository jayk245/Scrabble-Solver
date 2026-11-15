// The tile bag: the pool of unseen tiles players draw from.
//
// Tiles live in a fixed-capacity array so a Bag is trivially copyable and
// never allocates; Monte Carlo simulation copies the bag once per iteration
// and draws from the copy. Per-letter counts are maintained alongside the
// array so count queries do not scan.

#ifndef SCRABBLE_BAG_H_
#define SCRABBLE_BAG_H_

#include <array>
#include <cstdint>

#include "scrabble/rack.h"
#include "scrabble/rng.h"
#include "scrabble/tile.h"

namespace scrabble {

// Invariant: tiles_[0, size_) holds exactly the multiset described by
// counts_. The order of tiles_ is an implementation detail, but it is
// deterministic, so a given bag state plus a given Rng seed always yields
// the same draws.
class Bag {
 public:
  // An empty bag. Use Full() for a game-start bag.
  Bag() = default;

  // The standard 100-tile English distribution (kTileCounts).
  static Bag Full();

  // Removes and returns a uniformly random tile in O(1). Requires
  // !IsEmpty().
  Letter Draw(Rng& rng);

  // Draws until `rack` holds kRackSize tiles or the bag is empty. Returns
  // the number of tiles drawn.
  int DrawInto(Rack& rack, Rng& rng);

  // Puts a tile back. Requires the bag to have room (Size() < kTotalTiles).
  void Return(Letter letter);

  // Puts every tile of `rack` back. For an exchange, draw the replacement
  // tiles before returning the old ones, as the rules require.
  void Return(const Rack& rack);

  // Removes one specific tile, e.g. to set up a known position or to account
  // for tiles inferred on the opponent's rack. Returns false (and leaves the
  // bag unchanged) if no such tile is present. O(Size()).
  bool RemoveSpecific(Letter letter);

  int Size() const { return size_; }
  bool IsEmpty() const { return size_ == 0; }
  int Count(Letter letter) const { return counts_[letter]; }

 private:
  std::array<Letter, kTotalTiles> tiles_{};
  std::array<std::uint8_t, kAlphabetSize> counts_{};
  std::uint8_t size_ = 0;
};

}  // namespace scrabble

#endif  // SCRABBLE_BAG_H_
