#include "scrabble/leave_evaluator.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace scrabble {

// Defined in the source generated from data/leaves/enable_leaves_default.csv
// by cmake/EmbedFile.cmake (see engine/CMakeLists.txt).
namespace embedded {
extern const unsigned char kDefaultLeaves[];
extern const std::size_t kDefaultLeaves_size;
}  // namespace embedded

namespace {

// Hand-set single-tile values, loosely after published superleave tables:
// the blank and S are the best tiles to keep, Q and V the worst.
constexpr std::array<double, kAlphabetSize> kTileLeaveValues = {
    1.0,  -2.0, 0.5,  0.5,  2.0,  -2.0, -2.0, 0.5, -0.5,  // A-I
    -3.0, -2.0, -0.5, -0.5, 0.5,  -1.0, -1.0, -7.0, 1.5,  // J-R
    7.5,  0.5,  -3.0, -5.0, -3.0, 2.0,  -1.0, 2.0,         // S-Z
    25.0,                                                  // blank
};

// Each extra copy of a letter (beyond the first) costs this much: duplicates
// cut the number of distinct words a rack can form. Blanks are exempt.
constexpr double kDuplicatePenalty = 3.0;

// Cost per unit of vowel/consonant imbalance beyond one. A blank counts as
// neither since it can be either.
constexpr double kBalancePenalty = 2.0;

constexpr Letter kQ = 16;
constexpr Letter kU = 20;

constexpr bool IsVowel(Letter letter) {
  return letter == 0 || letter == 4 || letter == 8 || letter == 14 ||
         letter == 20;  // A E I O U
}

std::string_view Trim(std::string_view text) {
  constexpr std::string_view kSpace = " \t\r\n";
  const std::size_t begin = text.find_first_not_of(kSpace);
  if (begin == std::string_view::npos) return {};
  const std::size_t end = text.find_last_not_of(kSpace);
  return text.substr(begin, end - begin + 1);
}

std::optional<double> ParseDouble(std::string_view text) {
  if (text.empty()) return std::nullopt;
  // strtod needs a terminated buffer; the copy is load-time only.
  const std::string buffer(text);
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(buffer.c_str(), &end);
  if (errno != 0 || end != buffer.c_str() + buffer.size()) return std::nullopt;
  return value;
}

}  // namespace

LeaveEvaluator LeaveEvaluator::Heuristic() {
  LeaveEvaluator evaluator;
  evaluator.kind_ = Kind::kHeuristic;
  evaluator.name_ = "heuristic";
  return evaluator;
}

LeaveEvaluator LeaveEvaluator::Learned() {
  // Embedding, rather than reading the file through a compiled-in path,
  // means a binary that has been moved or installed elsewhere still gets
  // the same default leaves, and that no bot ever fails to construct.
  std::istringstream in(std::string(
      reinterpret_cast<const char*>(embedded::kDefaultLeaves),
      embedded::kDefaultLeaves_size));
  LeaveEvaluator evaluator = Parse(in, "enable_leaves_default.csv");
  evaluator.name_ = "learned";
  return evaluator;
}

std::shared_ptr<const LeaveEvaluator> DefaultLeaves() {
  // A function-local static: initialised once, thread-safely, on first use,
  // so programs that never build a default bot never parse the table.
  static const std::shared_ptr<const LeaveEvaluator> leaves =
      std::make_shared<const LeaveEvaluator>(LeaveEvaluator::Learned());
  return leaves;
}

LeaveEvaluator LeaveEvaluator::Load(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot open leave table " + path.string());
  }
  LeaveEvaluator evaluator = Parse(in, path.string());
  evaluator.name_ = "table:" + path.string();
  return evaluator;
}

LeaveEvaluator LeaveEvaluator::Parse(std::istream& in,
                                     std::string_view source) {
  struct Entry {
    std::uint64_t key;
    double value;
    int line;
  };
  std::vector<Entry> entries;
  LeaveEvaluator evaluator;
  std::vector<bool> model_seen;
  std::string line;
  int line_number = 0;
  const auto fail = [&](const std::string& why) {
    throw std::runtime_error(std::string(source) + ":" +
                             std::to_string(line_number) + ": " + why);
  };
  while (std::getline(in, line)) {
    ++line_number;
    const std::string_view text = Trim(line);
    if (text.empty() || text.front() == '#') continue;
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos) fail("expected leave,value");
    const std::string_view leave_text = Trim(text.substr(0, comma));
    const std::string_view value_text = Trim(text.substr(comma + 1));
    if (entries.empty() && leave_text == "leave" && value_text == "value") {
      continue;  // header
    }
    if (!leave_text.empty() && leave_text.front() == '@') {
      const std::optional<double> value = ParseDouble(value_text);
      if (!value) fail("bad value '" + std::string(value_text) + "'");
      const std::string error =
          evaluator.AddModelTerm(leave_text, *value, model_seen);
      if (!error.empty()) fail(error);
      continue;
    }
    const std::optional<Rack> leave = Rack::FromString(leave_text);
    if (!leave || leave->IsEmpty()) fail("bad leave '" +
                                         std::string(leave_text) + "'");
    const std::optional<double> value = ParseDouble(value_text);
    if (!value) fail("bad value '" + std::string(value_text) + "'");
    entries.push_back({Key(*leave), *value, line_number});
  }
  if (in.bad()) fail("read error");

  evaluator.kind_ = Kind::kTable;
  evaluator.name_ = "table:" + std::string(source);
  // At most half full; std::bit_ceil keeps the capacity a power of two so
  // the slot index is a shift, not a modulo.
  const std::size_t capacity =
      std::bit_ceil(std::max<std::size_t>(16, entries.size() * 2));
  evaluator.keys_.assign(capacity, 0);
  evaluator.values_.assign(capacity, 0.0);
  evaluator.shift_ = 64 - std::countr_zero(capacity);
  for (const Entry& entry : entries) {
    const std::size_t before = evaluator.size_;
    evaluator.Insert(entry.key, entry.value);
    line_number = entry.line;
    if (evaluator.size_ == before) fail("duplicate leave");
  }
  return evaluator;
}

double LeaveEvaluator::Value(const Rack& leave) const {
  if (leave.IsEmpty()) return 0.0;
  switch (kind_) {
    case Kind::kZero:
      return 0.0;
    case Kind::kHeuristic:
      return HeuristicValue(leave);
    case Kind::kTable:
      return TableValue(leave);
  }
  return 0.0;
}

std::uint64_t LeaveEvaluator::Key(const Rack& leave) {
  std::uint64_t key = 0;
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    for (int n = leave.Count(letter); n > 0; --n) {
      key = (key << 5) | (letter + 1u);
    }
  }
  return key;
}

double LeaveEvaluator::HeuristicValue(const Rack& leave) const {
  double value = 0.0;
  int vowels = 0;
  int consonants = 0;
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    const int count = leave.Count(letter);
    if (count == 0) continue;
    value += count * kTileLeaveValues[letter];
    if (letter == kBlank) continue;
    value -= (count - 1) * kDuplicatePenalty;
    (IsVowel(letter) ? vowels : consonants) += count;
  }
  const int imbalance = vowels > consonants ? vowels - consonants
                                            : consonants - vowels;
  if (imbalance > 1) value -= (imbalance - 1) * kBalancePenalty;
  return value;
}

double LeaveEvaluator::TableValue(const Rack& leave) const {
  const std::uint64_t key = Key(leave);
  const std::size_t mask = keys_.size() - 1;
  // Fibonacci hashing: the multiply spreads the packed letters (which
  // differ mostly in their low bits) across the high bits we keep.
  for (std::size_t slot = (key * 0x9E3779B97F4A7C15ULL) >> shift_;;
       slot = (slot + 1) & mask) {
    if (keys_[slot] == key) return values_[slot];
    if (keys_[slot] == 0) break;
  }
  if (!has_model_ || leave.Size() > kMaxModelLeave) return 0.0;
  return ModelValue(leave);
}

double LeaveEvaluator::ModelValue(const Rack& leave) const {
  double value = 0.0;
  int vowels = 0;
  int consonants = 0;
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    const int count = leave.Count(letter);
    value += tile_terms_[letter][static_cast<std::size_t>(count)];
    if (count == 0 || letter == kBlank) continue;
    (IsVowel(letter) ? vowels : consonants) += count;
  }
  value += length_terms_[static_cast<std::size_t>(leave.Size())];
  value += balance_terms_[static_cast<std::size_t>(vowels)]
                         [static_cast<std::size_t>(consonants)];
  if (leave.Contains(kQ) && !leave.Contains(kU)) value += q_without_u_;
  return value;
}

std::string LeaveEvaluator::AddModelTerm(std::string_view name, double value,
                                         std::vector<bool>& seen) {
  // Each term gets a slot in `seen` so a repeated term is caught: tile
  // terms first, then length, balance and Q-without-U terms.
  constexpr std::size_t kTileSlots = kAlphabetSize * kRackSize;
  constexpr std::size_t kLengthSlots = kRackSize;
  constexpr std::size_t kBalanceSlots = kRackSize * kRackSize;
  seen.resize(kTileSlots + kLengthSlots + kBalanceSlots + 1);
  const auto digit = [](char c) { return c >= '0' && c <= '9'; };
  double* term = nullptr;
  std::size_t slot = 0;
  if (name == "@qnou") {
    term = &q_without_u_;
    slot = kTileSlots + kLengthSlots + kBalanceSlots;
  } else if (name.size() == 5 && name.substr(1, 3) == "len" &&
             digit(name[4])) {
    const int size = name[4] - '0';
    if (size < 1 || size > kMaxModelLeave) {
      return "bad length term '" + std::string(name) + "'";
    }
    const auto n = static_cast<std::size_t>(size);
    term = &length_terms_[n];
    slot = kTileSlots + n;
  } else if (name.size() == 5 && (name[1] == 'v' || name[1] == 'V') &&
             digit(name[2]) && (name[3] == 'c' || name[3] == 'C') &&
             digit(name[4])) {
    const int vowels = name[2] - '0';
    const int consonants = name[4] - '0';
    if (vowels + consonants < 1 || vowels + consonants > kMaxModelLeave) {
      return "bad balance term '" + std::string(name) + "'";
    }
    const auto v = static_cast<std::size_t>(vowels);
    const auto c = static_cast<std::size_t>(consonants);
    term = &balance_terms_[v][c];
    slot = kTileSlots + kLengthSlots + v * kRackSize + c;
  } else if (name.size() == 3 && digit(name[2])) {
    const std::optional<Letter> letter = CharToLetter(name[1]);
    const int count = name[2] - '0';
    if (!letter || count < 1 || count > kMaxModelLeave) {
      return "bad tile term '" + std::string(name) + "'";
    }
    const auto k = static_cast<std::size_t>(count);
    term = &tile_terms_[*letter][k];
    slot = std::size_t{*letter} * kRackSize + k;
  } else {
    return "unknown model term '" + std::string(name) + "'";
  }
  if (seen[slot]) return "duplicate model term '" + std::string(name) + "'";
  seen[slot] = true;
  *term = value;
  has_model_ = true;
  return {};
}

void LeaveEvaluator::Insert(std::uint64_t key, double value) {
  assert(key != 0);
  const std::size_t mask = keys_.size() - 1;
  for (std::size_t slot = (key * 0x9E3779B97F4A7C15ULL) >> shift_;;
       slot = (slot + 1) & mask) {
    if (keys_[slot] == key) return;  // duplicate: caller reports it
    if (keys_[slot] == 0) {
      keys_[slot] = key;
      values_[slot] = value;
      ++size_;
      return;
    }
  }
}

}  // namespace scrabble
