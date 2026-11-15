// GADDAG lexicon (Gordon 1994) used by move generation and word validation.
//
// Every word w = x1..xn is stored as n paths: for each split point i in
// 1..n-1, REV(x1..xi) ^ x(i+1)..xn, plus REV(x1..xn) with no separator. Move
// generation starts at an anchor square, walks left through the reversed
// prefix, and after crossing the separator `^` walks right through the
// suffix. The graph is minimized (equivalent sub-graphs are shared), so it is
// a DAG rather than a trie.
//
// Memory layout: the whole graph is a single std::vector<Arc>. An Arc is an
// 8-byte record describing the node it points to: a 27-bit mask of the
// node's outgoing symbols, a terminal flag, and the index of the node's first
// child arc. A node's children are stored contiguously in symbol order, so
//
//   Child(node, s) = arcs_[node].first_child + popcount(mask & ((1 << s) - 1))
//
// This was chosen over Gordon-style arc lists because (a) lookup is one
// load, a bit test and a popcount with no scanning, and (b) a node handle is
// the index of the arc that describes it, so a child's handle is computed
// arithmetically and the child's record is fetched only when it is itself
// queried. There are no pointers, so Save/Load are raw array copies.

#ifndef SCRABBLE_GADDAG_H_
#define SCRABBLE_GADDAG_H_

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "scrabble/letter_set.h"
#include "scrabble/tile.h"

namespace scrabble {

// GADDAG edge symbols are the letters A-Z (0-25) plus this separator. It
// shares its value with kBlank but the two never appear in the same context.
inline constexpr Letter kGaddagSeparator = 26;
inline constexpr int kGaddagAlphabetSize = kNumLetters + 1;

class Gaddag {
 public:
  // Opaque handle to a node. Handles are only meaningful for the Gaddag that
  // produced them and stay valid for its lifetime.
  using Node = std::uint32_t;
  static constexpr Node kInvalidNode = 0xFFFFFFFFu;

  // Constructs an empty lexicon whose root has no children.
  Gaddag();

  // Builds a minimized GADDAG. Each word must be non-empty and consist only
  // of 'A'-'Z'; duplicates are ignored. Throws std::invalid_argument on a
  // malformed word.
  static Gaddag Build(std::span<const std::string> words);

  // Reads a file written by Save(). Throws std::runtime_error if the file is
  // missing, truncated, from an incompatible version, or structurally
  // invalid (arcs pointing out of bounds).
  static Gaddag Load(const std::filesystem::path& path);

  // Writes the graph in native byte order. Throws std::runtime_error on I/O
  // failure.
  void Save(const std::filesystem::path& path) const;

  Node Root() const { return 0; }

  // Follows the edge labelled `symbol` (a Letter or kGaddagSeparator) out of
  // `node`. Returns kInvalidNode if there is no such edge. Precondition:
  // `node` is a valid handle (not kInvalidNode).
  Node Child(Node node, Letter symbol) const {
    assert(node < arcs_.size());
    assert(symbol < kGaddagAlphabetSize);
    const Arc arc = arcs_[node];
    const std::uint32_t bit = 1u << symbol;
    const auto rank =
        static_cast<std::uint32_t>(std::popcount(arc.header & (bit - 1)));
    return (arc.header & bit) != 0 ? arc.first_child + rank : kInvalidNode;
  }

  // True if the path from the root to `node` spells a complete GADDAG
  // string, i.e. the letters placed so far form a word.
  bool IsTerminal(Node node) const {
    assert(node < arcs_.size());
    return (arcs_[node].header & kTerminalBit) != 0;
  }

  // Letters (excluding the separator) with an outgoing edge from `node`.
  // Intersect with a rack and a cross-check set to prune move generation.
  LetterSet ChildLetters(Node node) const {
    assert(node < arcs_.size());
    return LetterSet(arcs_[node].header);
  }

  bool HasSeparator(Node node) const {
    assert(node < arcs_.size());
    return (arcs_[node].header & kSeparatorBit) != 0;
  }

  // True if `word` (either case) is in the lexicon. Non-letter input yields
  // false rather than an error so callers can pass raw user text.
  bool Contains(std::string_view word) const;

  // Number of distinct words the graph was built from.
  std::size_t WordCount() const { return word_count_; }
  // Number of distinct nodes after minimization.
  std::size_t NodeCount() const { return node_count_; }
  // Number of stored arcs, including the root's descriptor arc.
  std::size_t ArcCount() const { return arcs_.size(); }
  // Bytes occupied by the graph itself (excluding the vector header).
  std::size_t MemoryBytes() const { return arcs_.size() * sizeof(Arc); }

 private:
  static constexpr std::uint32_t kSeparatorBit = 1u << kGaddagSeparator;
  static constexpr std::uint32_t kChildMask = (1u << kGaddagAlphabetSize) - 1;
  static constexpr std::uint32_t kTerminalBit = 1u << kGaddagAlphabetSize;

  // Describes the node an edge leads to. Two 32-bit fields rather than
  // bitfields keep the layout identical across compilers, which the binary
  // format depends on.
  struct Arc {
    std::uint32_t header;       // bits 0-26: child symbols, bit 27: terminal
    std::uint32_t first_child;  // index of the first child arc; 0 if none
  };
  static_assert(sizeof(Arc) == 8);

  friend class GaddagBuilder;

  std::vector<Arc> arcs_;
  std::size_t word_count_ = 0;
  std::size_t node_count_ = 0;
};

}  // namespace scrabble

#endif  // SCRABBLE_GADDAG_H_
