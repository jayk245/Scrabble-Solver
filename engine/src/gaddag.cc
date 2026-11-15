// GADDAG construction, lookup and binary serialization.
//
// Construction uses Daciuk et al.'s incremental algorithm for sorted input:
// all GADDAG strings are generated, sorted, and inserted one at a time; when
// the next string diverges from the previous one, the abandoned branch can
// never gain more children, so it is frozen and deduplicated against a
// registry of already-frozen nodes. The result is the minimal DAG without
// ever materializing the (much larger) trie.

#include "scrabble/gaddag.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scrabble {
namespace {

// "SGDG" in little-endian order. Reading a big-endian file (or any foreign
// file) fails this check instead of silently producing garbage.
constexpr std::uint32_t kFileMagic = 0x47444753u;
constexpr std::uint32_t kFileVersion = 1;

struct FileHeader {
  std::uint32_t magic;
  std::uint32_t version;
  std::uint32_t word_count;
  std::uint32_t node_count;
  std::uint32_t arc_count;
  std::uint32_t reserved;
};
static_assert(sizeof(FileHeader) == 24);

// Appends every GADDAG string for `word` (already converted to Letters).
void AppendGaddagStrings(const std::string& word,
                         std::vector<std::string>& out) {
  const std::size_t n = word.size();
  for (std::size_t split = 1; split <= n; ++split) {
    std::string s;
    s.reserve(n + 1);
    s.append(word.rbegin() + static_cast<std::ptrdiff_t>(n - split),
             word.rend());
    if (split < n) {
      s.push_back(static_cast<char>(kGaddagSeparator));
      s.append(word, split, std::string::npos);
    }
    out.push_back(std::move(s));
  }
}

}  // namespace

class GaddagBuilder {
 public:
  Gaddag Build(std::span<const std::string> words) {
    std::vector<std::string> encoded = EncodeWords(words);
    std::vector<std::string> strings;
    for (const std::string& word : encoded) AppendGaddagStrings(word, strings);
    // Byte order equals symbol order, so children are appended in ascending
    // symbol order, matching the popcount-rank layout.
    std::sort(strings.begin(), strings.end());

    path_.resize(1);
    depth_ = 1;
    const std::string* prev = nullptr;
    for (const std::string& s : strings) {
      std::size_t common = 0;
      if (prev != nullptr) {
        const std::size_t limit = std::min(prev->size(), s.size());
        while (common < limit && (*prev)[common] == s[common]) ++common;
      }
      FreezeTo(common + 1);
      for (std::size_t i = common; i < s.size(); ++i) {
        Push(static_cast<Letter>(s[i]));
      }
      path_[depth_ - 1].terminal = true;
      prev = &s;
    }
    FreezeTo(1);
    const std::uint32_t root = Register(path_[0]);

    Gaddag gaddag = Layout(root);
    gaddag.word_count_ = encoded.size();
    gaddag.node_count_ = headers_.size();
    return gaddag;
  }

 private:
  // A node on the current insertion path; still mutable because later
  // strings sharing this prefix may add children to it.
  struct PendingNode {
    Letter in_symbol = 0;
    bool terminal = false;
    std::uint32_t mask = 0;
    std::vector<std::uint32_t> children;  // registered ids, symbol order
  };

  // Validates and converts words to Letter bytes, sorted and deduplicated.
  static std::vector<std::string> EncodeWords(
      std::span<const std::string> words) {
    std::vector<std::string> encoded;
    encoded.reserve(words.size());
    for (const std::string& word : words) {
      if (word.empty()) {
        throw std::invalid_argument("GADDAG word list contains an empty word");
      }
      std::string letters(word.size(), '\0');
      for (std::size_t i = 0; i < word.size(); ++i) {
        if (word[i] < 'A' || word[i] > 'Z') {
          throw std::invalid_argument("GADDAG word is not uppercase A-Z: " +
                                      word);
        }
        letters[i] = static_cast<char>(word[i] - 'A');
      }
      encoded.push_back(std::move(letters));
    }
    std::sort(encoded.begin(), encoded.end());
    encoded.erase(std::unique(encoded.begin(), encoded.end()), encoded.end());
    if (encoded.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument("GADDAG word list is too large");
    }
    return encoded;
  }

  // Reuses PendingNode storage across insertions to avoid reallocating the
  // children vectors for every string.
  void Push(Letter symbol) {
    if (depth_ == path_.size()) path_.emplace_back();
    PendingNode& node = path_[depth_++];
    node.in_symbol = symbol;
    node.terminal = false;
    node.mask = 0;
    node.children.clear();
  }

  void FreezeTo(std::size_t target_depth) {
    while (depth_ > target_depth) {
      const PendingNode& node = path_[depth_ - 1];
      const std::uint32_t id = Register(node);
      PendingNode& parent = path_[depth_ - 2];
      parent.mask |= 1u << node.in_symbol;
      parent.children.push_back(id);
      --depth_;
    }
  }

  // Returns the id of a frozen node equivalent to `node`, creating one if
  // none exists. Two nodes are equivalent iff they agree on terminality,
  // child symbols and child ids; children are already canonical, so this
  // comparison is exact.
  std::uint32_t Register(const PendingNode& node) {
    const std::uint32_t header =
        node.mask | (node.terminal ? Gaddag::kTerminalBit : 0u);
    key_.resize(sizeof(header) + node.children.size() * sizeof(std::uint32_t));
    std::memcpy(key_.data(), &header, sizeof(header));
    if (!node.children.empty()) {
      std::memcpy(key_.data() + sizeof(header), node.children.data(),
                  node.children.size() * sizeof(std::uint32_t));
    }
    const auto next_id = static_cast<std::uint32_t>(headers_.size());
    const auto [it, inserted] = registry_.try_emplace(key_, next_id);
    if (inserted) {
      headers_.push_back(header);
      child_begin_.push_back(static_cast<std::uint32_t>(child_ids_.size()));
      child_ids_.insert(child_ids_.end(), node.children.begin(),
                        node.children.end());
    }
    return it->second;
  }

  // Assigns each frozen node a contiguous block of child arcs in BFS order,
  // which keeps the heavily used top of the graph in a few cache lines.
  Gaddag Layout(std::uint32_t root) {
    const std::size_t num_nodes = headers_.size();
    child_begin_.push_back(static_cast<std::uint32_t>(child_ids_.size()));
    const auto children_of = [this](std::uint32_t id) {
      return std::span<const std::uint32_t>(child_ids_.data() + child_begin_[id],
                                            child_begin_[id + 1] -
                                                child_begin_[id]);
    };

    std::vector<std::uint32_t> block(num_nodes, 0);
    std::vector<bool> visited(num_nodes, false);
    std::vector<std::uint32_t> order;
    order.reserve(num_nodes);
    order.push_back(root);
    visited[root] = true;
    std::uint64_t next_arc = 1;  // arc 0 describes the root
    for (std::size_t i = 0; i < order.size(); ++i) {
      const std::uint32_t id = order[i];
      const auto children = children_of(id);
      if (!children.empty()) {
        block[id] = static_cast<std::uint32_t>(next_arc);
        next_arc += children.size();
      }
      for (const std::uint32_t child : children) {
        if (!visited[child]) {
          visited[child] = true;
          order.push_back(child);
        }
      }
    }
    if (next_arc >= Gaddag::kInvalidNode) {
      throw std::invalid_argument("GADDAG exceeds 32-bit arc indices");
    }

    Gaddag gaddag;
    gaddag.arcs_.resize(static_cast<std::size_t>(next_arc));
    gaddag.arcs_[0] = {headers_[root], block[root]};
    for (const std::uint32_t id : order) {
      const auto children = children_of(id);
      for (std::size_t k = 0; k < children.size(); ++k) {
        const std::uint32_t child = children[k];
        gaddag.arcs_[block[id] + k] = {headers_[child], block[child]};
      }
    }
    return gaddag;
  }

  std::vector<PendingNode> path_;
  std::size_t depth_ = 0;

  // Frozen nodes, indexed by registration id. Children of node i are
  // child_ids_[child_begin_[i] .. child_begin_[i + 1]).
  std::vector<std::uint32_t> headers_;
  std::vector<std::uint32_t> child_begin_;
  std::vector<std::uint32_t> child_ids_;
  std::unordered_map<std::string, std::uint32_t> registry_;
  std::string key_;  // scratch buffer for registry lookups
};

Gaddag::Gaddag() : arcs_(1, Arc{0, 0}), node_count_(1) {}

Gaddag Gaddag::Build(std::span<const std::string> words) {
  return GaddagBuilder().Build(words);
}

bool Gaddag::Contains(std::string_view word) const {
  if (word.empty()) return false;
  // REV(word) with no separator is the one GADDAG string that ends exactly
  // at the word's first letter, so a single backwards walk suffices.
  Node node = Root();
  for (auto it = word.rbegin(); it != word.rend(); ++it) {
    const std::optional<Letter> letter = CharToLetter(*it);
    if (!letter.has_value() || *letter >= kNumLetters) return false;
    node = Child(node, *letter);
    if (node == kInvalidNode) return false;
  }
  return IsTerminal(node);
}

void Gaddag::Save(const std::filesystem::path& path) const {
  const FileHeader header = {
      .magic = kFileMagic,
      .version = kFileVersion,
      .word_count = static_cast<std::uint32_t>(word_count_),
      .node_count = static_cast<std::uint32_t>(node_count_),
      .arc_count = static_cast<std::uint32_t>(arcs_.size()),
      .reserved = 0,
  };
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));
  out.write(reinterpret_cast<const char*>(arcs_.data()),
            static_cast<std::streamsize>(MemoryBytes()));
  out.flush();
  if (!out) {
    throw std::runtime_error("failed to write GADDAG file: " + path.string());
  }
}

Gaddag Gaddag::Load(const std::filesystem::path& path) {
  const std::string name = path.string();
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open GADDAG file: " + name);

  FileHeader header{};
  in.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (!in || header.magic != kFileMagic) {
    throw std::runtime_error("not a GADDAG file: " + name);
  }
  if (header.version != kFileVersion) {
    throw std::runtime_error("unsupported GADDAG version " +
                             std::to_string(header.version) + ": " + name);
  }
  if (header.arc_count == 0 || header.arc_count == kInvalidNode) {
    throw std::runtime_error("corrupt GADDAG arc count: " + name);
  }

  Gaddag gaddag;
  gaddag.arcs_.resize(header.arc_count);
  in.read(reinterpret_cast<char*>(gaddag.arcs_.data()),
          static_cast<std::streamsize>(gaddag.MemoryBytes()));
  if (!in || in.peek() != std::ifstream::traits_type::eof()) {
    throw std::runtime_error("truncated or oversized GADDAG file: " + name);
  }

  // One linear pass so a corrupt file fails here instead of reading out of
  // bounds deep inside move generation.
  const std::uint64_t arc_count = header.arc_count;
  for (const Arc& arc : gaddag.arcs_) {
    if ((arc.header & ~(kChildMask | kTerminalBit)) != 0) {
      throw std::runtime_error("corrupt GADDAG arc header: " + name);
    }
    const auto num_children =
        static_cast<std::uint64_t>(std::popcount(arc.header & kChildMask));
    if (num_children != 0 &&
        (arc.first_child == 0 || arc.first_child + num_children > arc_count)) {
      throw std::runtime_error("corrupt GADDAG child index: " + name);
    }
  }

  gaddag.word_count_ = header.word_count;
  gaddag.node_count_ = header.node_count;
  return gaddag;
}

}  // namespace scrabble
