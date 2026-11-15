#include "scrabble/gaddag.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace scrabble {
namespace {

constexpr char kSep = '^';

// The GADDAG strings Gordon's construction defines for `word`, spelled with
// '^' for the separator. Independent of the implementation under test.
std::set<std::string> ExpectedPaths(const std::vector<std::string>& words) {
  std::set<std::string> paths;
  for (const std::string& w : words) {
    for (std::size_t split = 1; split <= w.size(); ++split) {
      std::string prefix = w.substr(0, split);
      std::reverse(prefix.begin(), prefix.end());
      paths.insert(split == w.size() ? prefix
                                     : prefix + kSep + w.substr(split));
    }
  }
  return paths;
}

// Walks a '^'-spelled path; returns kInvalidNode if any edge is missing.
Gaddag::Node Walk(const Gaddag& g, const std::string& path) {
  Gaddag::Node node = g.Root();
  for (char c : path) {
    const Letter symbol =
        c == kSep ? kGaddagSeparator : static_cast<Letter>(c - 'A');
    node = g.Child(node, symbol);
    if (node == Gaddag::kInvalidNode) break;
  }
  return node;
}

// Every root-to-terminal path in the graph, spelled with '^'.
void Enumerate(const Gaddag& g, Gaddag::Node node, std::string& prefix,
               std::set<std::string>& out) {
  if (g.IsTerminal(node)) out.insert(prefix);
  for (Letter s = 0; s < kGaddagAlphabetSize; ++s) {
    const Gaddag::Node child = g.Child(node, s);
    if (child == Gaddag::kInvalidNode) continue;
    prefix.push_back(s == kGaddagSeparator ? kSep
                                           : static_cast<char>('A' + s));
    Enumerate(g, child, prefix, out);
    prefix.pop_back();
  }
}

std::set<std::string> AllPaths(const Gaddag& g) {
  std::set<std::string> out;
  std::string prefix;
  Enumerate(g, g.Root(), prefix, out);
  return out;
}

std::filesystem::path TempFile(const std::string& name) {
  return std::filesystem::path(::testing::TempDir()) / name;
}

const std::vector<std::string> kSmallWords = {"CARE", "CAR",  "ARE",
                                              "CARES", "SCARE", "RACE",
                                              "ACE",  "AA",   "ZYZZYVA"};

TEST(GaddagTest, EmptyLexicon) {
  const Gaddag g;
  EXPECT_EQ(g.WordCount(), 0u);
  EXPECT_FALSE(g.Contains("A"));
  EXPECT_FALSE(g.Contains(""));
  EXPECT_TRUE(g.ChildLetters(g.Root()).Empty());
  EXPECT_FALSE(g.IsTerminal(g.Root()));
  EXPECT_TRUE(AllPaths(g).empty());
}

TEST(GaddagTest, ContainsEveryWord) {
  const Gaddag g = Gaddag::Build(kSmallWords);
  EXPECT_EQ(g.WordCount(), kSmallWords.size());
  for (const std::string& w : kSmallWords) EXPECT_TRUE(g.Contains(w)) << w;
}

TEST(GaddagTest, RejectsNonWordsAndPrefixes) {
  const Gaddag g = Gaddag::Build(kSmallWords);
  for (const char* w : {"CA", "C", "CARS", "SCAR", "ACES", "ERAC", "A", "ZYZ",
                        "ZYZZYVAS", "", "QI", "C^ARE", "CA?E", "CAR E"}) {
    EXPECT_FALSE(g.Contains(w)) << w;
  }
}

TEST(GaddagTest, ContainsIsCaseInsensitive) {
  const Gaddag g = Gaddag::Build(kSmallWords);
  EXPECT_TRUE(g.Contains("care"));
  EXPECT_TRUE(g.Contains("ZyZzYvA"));
}

TEST(GaddagTest, HasEveryRotationOfCare) {
  const Gaddag g = Gaddag::Build(std::vector<std::string>{"CARE"});
  for (const char* path : {"C^ARE", "AC^RE", "RAC^E", "ERAC"}) {
    const Gaddag::Node node = Walk(g, path);
    ASSERT_NE(node, Gaddag::kInvalidNode) << path;
    EXPECT_TRUE(g.IsTerminal(node)) << path;
  }
  // Partial paths exist but do not terminate.
  const Gaddag::Node partial = Walk(g, "AC^R");
  ASSERT_NE(partial, Gaddag::kInvalidNode);
  EXPECT_FALSE(g.IsTerminal(partial));
  // A separator after the full reversed word is not part of the encoding.
  EXPECT_EQ(Walk(g, "ERAC^"), Gaddag::kInvalidNode);
  EXPECT_EQ(Walk(g, "CA^RE"), Gaddag::kInvalidNode);
}

TEST(GaddagTest, PathSetMatchesDefinitionExactly) {
  const Gaddag g = Gaddag::Build(kSmallWords);
  EXPECT_EQ(AllPaths(g), ExpectedPaths(kSmallWords));
}

TEST(GaddagTest, ChildLettersAndSeparator) {
  const Gaddag g = Gaddag::Build(std::vector<std::string>{"CARE"});
  LetterSet expected;
  for (char c : {'C', 'A', 'R', 'E'}) {
    expected.Add(static_cast<Letter>(c - 'A'));
  }
  EXPECT_EQ(g.ChildLetters(g.Root()), expected);
  EXPECT_FALSE(g.HasSeparator(g.Root()));

  const Gaddag::Node c = g.Child(g.Root(), static_cast<Letter>('C' - 'A'));
  EXPECT_TRUE(g.HasSeparator(c));
  EXPECT_TRUE(g.ChildLetters(c).Empty());
}

TEST(GaddagTest, SharesEquivalentNodes) {
  // "AB" yields A^B and BA. Unminimized that is 7 nodes; the two terminal
  // leaves are identical and must merge: root, A, A^, B, leaf.
  const Gaddag g = Gaddag::Build(std::vector<std::string>{"AB"});
  EXPECT_EQ(g.NodeCount(), 5u);
  EXPECT_EQ(g.ArcCount(), 6u);  // root descriptor + 5 edges
  EXPECT_EQ(g.MemoryBytes(), 6u * 8u);
}

TEST(GaddagTest, IgnoresDuplicates) {
  const Gaddag g =
      Gaddag::Build(std::vector<std::string>{"CAT", "CAT", "DOG", "CAT"});
  EXPECT_EQ(g.WordCount(), 2u);
  EXPECT_EQ(AllPaths(g), ExpectedPaths({"CAT", "DOG"}));
}

TEST(GaddagTest, BuildRejectsMalformedWords) {
  EXPECT_THROW(Gaddag::Build(std::vector<std::string>{"cat"}),
               std::invalid_argument);
  EXPECT_THROW(Gaddag::Build(std::vector<std::string>{""}),
               std::invalid_argument);
  EXPECT_THROW(Gaddag::Build(std::vector<std::string>{"CA T"}),
               std::invalid_argument);
}

// Cross-checks the graph against std::set on a random lexicon whose words
// share many prefixes and suffixes (small alphabet), stressing minimization.
TEST(GaddagTest, MatchesReferenceSetOnRandomLexicon) {
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> length(1, 7);
  std::uniform_int_distribution<int> letter(0, 3);
  const auto random_word = [&] {
    std::string w(static_cast<std::size_t>(length(rng)), 'A');
    for (char& c : w) c = static_cast<char>('A' + letter(rng));
    return w;
  };

  std::vector<std::string> words;
  for (int i = 0; i < 400; ++i) words.push_back(random_word());
  const std::set<std::string> reference(words.begin(), words.end());
  const Gaddag g = Gaddag::Build(words);

  EXPECT_EQ(g.WordCount(), reference.size());
  EXPECT_EQ(AllPaths(g), ExpectedPaths({reference.begin(), reference.end()}));
  for (int i = 0; i < 2000; ++i) {
    const std::string probe = random_word();
    EXPECT_EQ(g.Contains(probe), reference.count(probe) == 1) << probe;
  }
}

TEST(GaddagSerializationTest, RoundTripPreservesGraph) {
  const Gaddag original = Gaddag::Build(kSmallWords);
  const std::filesystem::path path = TempFile("round_trip.gaddag");
  original.Save(path);
  const Gaddag loaded = Gaddag::Load(path);

  EXPECT_EQ(loaded.WordCount(), original.WordCount());
  EXPECT_EQ(loaded.NodeCount(), original.NodeCount());
  EXPECT_EQ(loaded.ArcCount(), original.ArcCount());
  EXPECT_EQ(AllPaths(loaded), AllPaths(original));
  EXPECT_EQ(std::filesystem::file_size(path), 24 + original.MemoryBytes());
  std::filesystem::remove(path);
}

TEST(GaddagSerializationTest, RoundTripEmptyLexicon) {
  const std::filesystem::path path = TempFile("empty.gaddag");
  Gaddag().Save(path);
  const Gaddag loaded = Gaddag::Load(path);
  EXPECT_EQ(loaded.ArcCount(), 1u);
  EXPECT_FALSE(loaded.Contains("A"));
  std::filesystem::remove(path);
}

TEST(GaddagSerializationTest, LoadRejectsMissingFile) {
  EXPECT_THROW(Gaddag::Load(TempFile("does_not_exist.gaddag")),
               std::runtime_error);
}

TEST(GaddagSerializationTest, LoadRejectsForeignFile) {
  const std::filesystem::path path = TempFile("foreign.gaddag");
  std::ofstream(path) << "this is not a gaddag file at all";
  EXPECT_THROW(Gaddag::Load(path), std::runtime_error);
  std::filesystem::remove(path);
}

TEST(GaddagSerializationTest, LoadRejectsTruncatedFile) {
  const std::filesystem::path path = TempFile("truncated.gaddag");
  Gaddag::Build(kSmallWords).Save(path);
  std::filesystem::resize_file(path, std::filesystem::file_size(path) - 4);
  EXPECT_THROW(Gaddag::Load(path), std::runtime_error);
  std::filesystem::remove(path);
}

TEST(GaddagSerializationTest, LoadRejectsOutOfBoundsChild) {
  const std::filesystem::path path = TempFile("corrupt.gaddag");
  Gaddag::Build(kSmallWords).Save(path);
  {
    // Point the root's first child far past the end of the arc array.
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(24 + 4);
    const std::uint32_t bogus = 0x00FFFFFFu;
    f.write(reinterpret_cast<const char*>(&bogus), sizeof(bogus));
  }
  EXPECT_THROW(Gaddag::Load(path), std::runtime_error);
  std::filesystem::remove(path);
}

}  // namespace
}  // namespace scrabble
