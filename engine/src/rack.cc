#include "scrabble/rack.h"

#include <optional>
#include <string>
#include <string_view>

namespace scrabble {

std::optional<Rack> Rack::FromString(std::string_view text) {
  if (text.size() > static_cast<std::size_t>(kRackSize)) return std::nullopt;
  Rack rack;
  for (char c : text) {
    const std::optional<Letter> letter = CharToLetter(c);
    if (!letter) return std::nullopt;
    rack.Add(*letter);
  }
  return rack;
}

std::string Rack::ToString() const {
  std::string out;
  out.reserve(size_);
  // Letter indices already sort A-Z with the blank last.
  for (std::size_t i = 0; i < counts_.size(); ++i) {
    out.append(counts_[i], LetterToChar(static_cast<Letter>(i)));
  }
  return out;
}

}  // namespace scrabble
