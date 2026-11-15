// Word-list loading and GADDAG caching.
//
// Word lists are plain text, one word per line. They are normalized here
// (trimmed, uppercased, filtered to playable lengths) so the GADDAG builder
// can assume clean input.

#ifndef SCRABBLE_LEXICON_H_
#define SCRABBLE_LEXICON_H_

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "scrabble/gaddag.h"

namespace scrabble {

// Words outside this range can never be played on a 15x15 board.
inline constexpr std::size_t kMinWordLength = 2;
inline constexpr std::size_t kMaxWordLength = 15;

struct WordList {
  std::vector<std::string> words;  // uppercase A-Z, sorted, no duplicates
  std::size_t rejected = 0;        // non-blank lines that failed validation
};

// Reads a newline-separated word list. Surrounding whitespace (including a
// trailing CR) is ignored, letters may be either case, and blank lines are
// skipped. Lines that are not 2-15 letters A-Z are counted in `rejected`
// rather than treated as errors, since published lists contain long words
// that are valid but unplayable. Throws std::runtime_error if the file
// cannot be read.
WordList ReadWordList(const std::filesystem::path& path);

// Where LoadOrBuildGaddag() caches the compiled graph: the word list's path
// with its extension replaced by ".gaddag".
std::filesystem::path GaddagCachePath(
    const std::filesystem::path& word_list_path);

// Returns the GADDAG for a word list, loading the cache if it is at least as
// new as the list and rebuilding (then rewriting the cache) otherwise. A
// stale, corrupt or unwritable cache is not an error: it only costs a
// rebuild. Throws std::runtime_error if the word list itself is unreadable.
Gaddag LoadOrBuildGaddag(const std::filesystem::path& word_list_path);

}  // namespace scrabble

#endif  // SCRABBLE_LEXICON_H_
