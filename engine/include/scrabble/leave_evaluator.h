// Rack-leave valuation: how much the tiles kept after a move are worth.
//
// Static equity is score + leave value, so this is the bot's only notion of
// "future" until Monte Carlo simulation (M7) refines the top candidates.
//
// Three sources are supported:
//   * Zero: every leave is worth 0, i.e. pure score maximisation. Used as
//     the control in A/B tests.
//   * Heuristic: a small hand-set model (per-tile values, duplicate and
//     vowel/consonant balance penalties). It is a placeholder that M6
//     replaces with values learned from self-play logs.
//   * Table: values loaded from a file, keyed by canonical leave. This is
//     the hook M6 plugs learned values into.
//
// Leave table file format (CSV, UTF-8 / ASCII):
//
//   # comments and blank lines are ignored
//   leave,value          <- optional header
//   ?,25.6
//   ES,4.1
//   QU,-5.2
//
// The leave is 1-7 tiles, A-Z (either case) and '?' for a blank, in any
// order; it is canonicalised on load, so "SE" and "ES" are the same key and
// may not both appear. Leaves absent from the table are worth 0, as is the
// empty leave (going out or exchanging everything).

#ifndef SCRABBLE_LEAVE_EVALUATOR_H_
#define SCRABBLE_LEAVE_EVALUATOR_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <string>
#include <string_view>
#include <vector>

#include "scrabble/rack.h"

namespace scrabble {

// Immutable after construction and safe to share between threads (the
// arena shares one loaded table across all workers).
class LeaveEvaluator {
 public:
  enum class Kind : std::uint8_t { kZero, kHeuristic, kTable };

  // Equivalent to Zero().
  LeaveEvaluator() = default;

  static LeaveEvaluator Zero() { return LeaveEvaluator(); }
  static LeaveEvaluator Heuristic();

  // Reads a leave table (format in the file comment). Throws
  // std::runtime_error naming the file and line on unreadable files,
  // malformed lines or duplicate leaves.
  static LeaveEvaluator Load(const std::filesystem::path& path);

  // As Load(), from an open stream; `source` names it in error messages.
  static LeaveEvaluator Parse(std::istream& in, std::string_view source);

  // Value of keeping `leave`, in points. Allocation-free and does not
  // depend on the order tiles were added. The empty leave is always 0.
  double Value(const Rack& leave) const;

  Kind kind() const { return kind_; }

  // Number of table entries (0 unless kind() == kTable).
  std::size_t size() const { return size_; }

  // Short label for reports: "zero", "heuristic" or "table:<path>".
  const std::string& name() const { return name_; }

 private:
  // A rack packed into an integer: tiles in canonical order, each stored as
  // (letter + 1) in 5 bits. Seven tiles need 35 bits, and no tile encodes
  // as 0, so distinct racks (including different lengths) never collide and
  // 0 is free to mark empty hash slots.
  static std::uint64_t Key(const Rack& leave);

  double HeuristicValue(const Rack& leave) const;
  double TableValue(const Rack& leave) const;
  void Insert(std::uint64_t key, double value);

  Kind kind_ = Kind::kZero;
  std::string name_ = "zero";
  // Open-addressing hash table with linear probing. Capacity is a power of
  // two at least twice the entry count, so probes stay short; parallel
  // arrays keep the probed keys dense in cache.
  std::vector<std::uint64_t> keys_;
  std::vector<double> values_;
  std::size_t size_ = 0;
  int shift_ = 64;  // 64 - log2(capacity), for Fibonacci hashing
};

}  // namespace scrabble

#endif  // SCRABBLE_LEAVE_EVALUATOR_H_
