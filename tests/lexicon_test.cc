#include "scrabble/lexicon.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace scrabble {
namespace {

std::filesystem::path TempFile(const std::string& name) {
  return std::filesystem::path(::testing::TempDir()) / name;
}

TEST(WordListTest, NormalizesAndFilters) {
  const std::filesystem::path path = TempFile("words.txt");
  std::ofstream(path) << "care\r\n"
                         "  Zyzzyva \n"
                         "\n"
                         "a\n"                    // too short
                         "abcdefghijklmnop\n"     // 16 letters: too long
                         "abcdefghijklmno\n"      // 15 letters: kept
                         "can't\n"                // punctuation
                         "CARE\n";                // duplicate
  const WordList list = ReadWordList(path);
  EXPECT_EQ(list.words, (std::vector<std::string>{"ABCDEFGHIJKLMNO", "CARE",
                                                  "ZYZZYVA"}));
  EXPECT_EQ(list.rejected, 3u);
  std::filesystem::remove(path);
}

TEST(WordListTest, MissingFileThrows) {
  EXPECT_THROW(ReadWordList(TempFile("no_such_list.txt")), std::runtime_error);
}

TEST(LexiconTest, CachePathReplacesExtension) {
  EXPECT_EQ(GaddagCachePath("data/lexica/enable1.txt"),
            std::filesystem::path("data/lexica/enable1.gaddag"));
}

TEST(LexiconTest, LoadOrBuildWritesAndReusesCache) {
  const std::filesystem::path list = TempFile("cached.txt");
  const std::filesystem::path cache = GaddagCachePath(list);
  std::filesystem::remove(cache);
  std::ofstream(list) << "cat\ndog\n";

  const Gaddag built = LoadOrBuildGaddag(list);
  EXPECT_TRUE(built.Contains("CAT"));
  ASSERT_TRUE(std::filesystem::exists(cache));

  // A corrupt cache is rebuilt transparently rather than surfacing an error.
  std::ofstream(cache) << "garbage";
  const Gaddag rebuilt = LoadOrBuildGaddag(list);
  EXPECT_TRUE(rebuilt.Contains("DOG"));
  EXPECT_EQ(rebuilt.WordCount(), 2u);

  const Gaddag reloaded = LoadOrBuildGaddag(list);
  EXPECT_EQ(reloaded.ArcCount(), built.ArcCount());
  std::filesystem::remove(list);
  std::filesystem::remove(cache);
}

// Integration test against the real ENABLE list. Skipped unless
// scripts/fetch_lexicon.sh has been run. Goes through the on-disk cache
// (written next to the list, gitignored) because gtest_discover_tests runs
// each test in its own process and a sanitized rebuild costs ~15 s apiece.
class EnableTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    const std::filesystem::path path =
        std::filesystem::path(SCRABBLE_LEXICA_DIR) / "enable1.txt";
    if (!std::filesystem::exists(path)) return;
    list_ = std::make_unique<WordList>(ReadWordList(path));
    gaddag_ = std::make_unique<Gaddag>(LoadOrBuildGaddag(path));
  }
  static void TearDownTestSuite() {
    gaddag_.reset();
    list_.reset();
  }

  void SetUp() override {
    if (gaddag_ == nullptr) {
      GTEST_SKIP() << "ENABLE not found; run scripts/fetch_lexicon.sh";
    }
  }

  static bool InList(const std::string& word) {
    return std::binary_search(list_->words.begin(), list_->words.end(), word);
  }

  static inline std::unique_ptr<WordList> list_;
  static inline std::unique_ptr<Gaddag> gaddag_;
};

TEST_F(EnableTest, WordCountAfterFiltering) {
  // 172,823 raw entries, of which 4,272 exceed 15 letters.
  EXPECT_EQ(list_->words.size(), 168'551u);
  EXPECT_EQ(list_->rejected, 4'272u);
  EXPECT_EQ(gaddag_->WordCount(), list_->words.size());
}

TEST_F(EnableTest, ContainsEveryListedWord) {
  std::size_t missing = 0;
  for (const std::string& w : list_->words) missing += !gaddag_->Contains(w);
  EXPECT_EQ(missing, 0u);
}

TEST_F(EnableTest, SpotWords) {
  for (const char* w : {"AA", "QAT", "ZYZZYVA", "XU", "ABSORBABILITIES"}) {
    EXPECT_TRUE(InList(w)) << w;
    EXPECT_TRUE(gaddag_->Contains(w)) << w;
  }
  // ENABLE predates QI and ZA; long words are filtered as unplayable.
  for (const char* w : {"QI", "ZA", "ABSENTMINDEDNESS", "AAX", "ZYZZYVAX"}) {
    EXPECT_FALSE(InList(w)) << w;
    EXPECT_FALSE(gaddag_->Contains(w)) << w;
  }
}

}  // namespace
}  // namespace scrabble
