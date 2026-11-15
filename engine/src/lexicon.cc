// Word-list parsing and the build-or-load cache policy for GADDAGs.

#include "scrabble/lexicon.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace scrabble {
namespace {

std::string_view Trim(std::string_view s) {
  constexpr std::string_view kWhitespace = " \t\r\n\f\v";
  const std::size_t begin = s.find_first_not_of(kWhitespace);
  if (begin == std::string_view::npos) return {};
  const std::size_t end = s.find_last_not_of(kWhitespace);
  return s.substr(begin, end - begin + 1);
}

// Returns the uppercased word, or an empty string if `raw` is not playable.
std::string NormalizeWord(std::string_view raw) {
  if (raw.size() < kMinWordLength || raw.size() > kMaxWordLength) return {};
  std::string word(raw);
  for (char& c : word) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    } else if (c < 'A' || c > 'Z') {
      return {};
    }
  }
  return word;
}

bool CacheIsFresh(const std::filesystem::path& cache,
                  const std::filesystem::path& source) {
  std::error_code ec;
  const auto cache_time = std::filesystem::last_write_time(cache, ec);
  if (ec) return false;
  const auto source_time = std::filesystem::last_write_time(source, ec);
  return !ec && cache_time >= source_time;
}

}  // namespace

WordList ReadWordList(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot open word list: " + path.string());
  }
  WordList list;
  std::string line;
  while (std::getline(in, line)) {
    const std::string_view trimmed = Trim(line);
    if (trimmed.empty()) continue;
    std::string word = NormalizeWord(trimmed);
    if (word.empty()) {
      ++list.rejected;
    } else {
      list.words.push_back(std::move(word));
    }
  }
  if (in.bad()) {
    throw std::runtime_error("error reading word list: " + path.string());
  }
  std::sort(list.words.begin(), list.words.end());
  list.words.erase(std::unique(list.words.begin(), list.words.end()),
                   list.words.end());
  return list;
}

std::filesystem::path GaddagCachePath(
    const std::filesystem::path& word_list_path) {
  std::filesystem::path cache = word_list_path;
  cache.replace_extension(".gaddag");
  return cache;
}

Gaddag LoadOrBuildGaddag(const std::filesystem::path& word_list_path) {
  const std::filesystem::path cache = GaddagCachePath(word_list_path);
  if (CacheIsFresh(cache, word_list_path)) {
    try {
      return Gaddag::Load(cache);
    } catch (const std::runtime_error&) {
      // Fall through and rebuild; the cache is overwritten below.
    }
  }
  const WordList list = ReadWordList(word_list_path);
  Gaddag gaddag = Gaddag::Build(list.words);
  try {
    gaddag.Save(cache);
  } catch (const std::runtime_error&) {
    // A read-only data directory should not prevent the engine from running.
  }
  return gaddag;
}

}  // namespace scrabble
