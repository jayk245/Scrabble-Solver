#include "scrabble/endgame.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <limits>

#include "scrabble/rng.h"

namespace scrabble {

namespace {

// Larger than any reachable spread, small enough that negating it or adding
// a move score cannot overflow.
constexpr int kInfinity = 1'000'000;

// Draft recorded for a subtree whose value no horizon guess influenced: it
// satisfies every depth request, now and in later iterations.
constexpr int kSolvedDepth = 255;

// Ordering keys: the preferred move first, going-out moves next, the pass
// last. Ordinary keys (score + unloaded face value) stay far below kGoOut.
constexpr int kPreferredKey = INT_MAX;
constexpr int kGoOutKey = 1 << 20;
constexpr int kPassKey = INT_MIN;

// How often (in nodes) the clock is read. A read costs tens of
// nanoseconds; spread over this many nodes it is negligible, while the
// interval stays well under a millisecond of search.
constexpr std::int64_t kClockInterval = 1024;

constexpr std::uint16_t kNoMove = 0xFFFF;

enum Bound : std::uint8_t { kNone = 0, kExact, kLower, kUpper };

int UnloadedValue(const Move& move) {
  int value = 0;
  for (std::size_t i = 0; i < move.num_tiles; ++i) {
    const MoveTile tile = move.tiles[i];
    if (!tile.is_play_through() && !tile.is_blank()) {
      value += kTileValues[tile.letter()];
    }
  }
  return value;
}

bool GoesOut(const Move& move, const Rack& rack) {
  return move.type == MoveType::kPlace && move.TilesUsed() == rack.Size();
}

}  // namespace

// 16 bytes; a bucket of two fills half a 64-byte cache line, so a probe
// touches one line.
struct EndgameSolver::TtEntry {
  std::uint64_t key = 0;
  std::int16_t value = 0;
  std::uint16_t move = kNoMove;  // index into the node's generated moves
  std::uint8_t depth = 0;        // remaining plies searched, or kSolvedDepth
  std::uint8_t bound = kNone;
  std::uint8_t generation = 0;   // 0 never matches: zeroed memory is empty
};

// Slot 0 keeps the deepest draft seen (expensive to recompute); slot 1
// always takes the newest entry, so recent shallow results are not lost to
// a deep entry that happens to share the bucket.
struct alignas(32) EndgameSolver::TtBucket {
  std::array<TtEntry, 2> entries;
};

const Zobrist& Zobrist::Instance() {
  static const Zobrist instance;
  return instance;
}

Zobrist::Zobrist() {
  Rng rng(0x5EED'E1D6'A3E5'0001ULL);
  for (auto& square : square_) {
    for (std::uint64_t& key : square) key = rng();
  }
  for (auto& player : rack_) {
    for (auto& letter : player) {
      for (std::uint64_t& key : letter) key = rng();
    }
  }
  for (std::uint64_t& key : scoreless_) key = rng();
  side_ = rng();
}

std::uint64_t Zobrist::RackKey(int player, const Rack& rack) const {
  const auto& keys = rack_[static_cast<std::size_t>(player)];
  std::uint64_t hash = 0;
  for (Letter letter = 0; letter < kAlphabetSize; ++letter) {
    hash ^= keys[letter][static_cast<std::size_t>(rack.Count(letter))];
  }
  return hash;
}

std::uint64_t Zobrist::Hash(const Board& board,
                            const std::array<Rack, kNumPlayers>& racks,
                            int to_move, int scoreless_turns) const {
  std::uint64_t hash = 0;
  for (int row = 0; row < kBoardSize; ++row) {
    for (int col = 0; col < kBoardSize; ++col) {
      const Square& square = board.At(row, col);
      if (square.IsEmpty()) continue;
      hash ^= square_[SquareIndex(row, col)]
                     [square.letter + (square.is_blank ? kNumLetters : 0)];
    }
  }
  for (int player = 0; player < kNumPlayers; ++player) {
    hash ^= RackKey(player, racks[static_cast<std::size_t>(player)]);
  }
  hash ^= scoreless_[static_cast<std::size_t>(scoreless_turns)];
  if (to_move == 1) hash ^= side_;
  return hash;
}

std::uint64_t Zobrist::Update(std::uint64_t hash, const Move& move,
                              int player, const Rack& before,
                              const Rack& after, int scoreless_before,
                              int scoreless_after) const {
  if (move.type == MoveType::kPlace) {
    const int dr = move.direction == Direction::kDown ? 1 : 0;
    const int dc = 1 - dr;
    for (int i = 0; i < move.num_tiles; ++i) {
      const MoveTile tile = move.tiles[static_cast<std::size_t>(i)];
      if (tile.is_play_through()) continue;
      hash ^= square_[SquareIndex(move.row + i * dr, move.col + i * dc)]
                     [tile.letter() + (tile.is_blank() ? kNumLetters : 0)];
    }
    hash ^= RackKey(player, before) ^ RackKey(player, after);
  }
  hash ^= scoreless_[static_cast<std::size_t>(scoreless_before)] ^
          scoreless_[static_cast<std::size_t>(scoreless_after)];
  return hash ^ side_;
}

EndgameSolver::EndgameSolver(const Gaddag& gaddag,
                             const EndgameOptions& options)
    : options_(options),
      generator_(gaddag),
      plies_(kEndgameMaxPlies),
      pv_(kEndgameMaxPlies) {
  static_assert(sizeof(TtEntry) == 16);
  if (options_.tt_size_mb > 0) {
    const std::size_t bytes = options_.tt_size_mb << 20;
    std::size_t buckets = 1;
    while (buckets * 2 * sizeof(TtBucket) <= bytes) buckets *= 2;
    table_.resize(buckets);
    table_mask_ = buckets - 1;
  }
  // Typical endgame nodes have tens to a few hundred moves; reserving up
  // front means the first solve rarely reallocates mid-search.
  for (Ply& ply : plies_) {
    ply.moves.reserve(1024);
    ply.order.reserve(1024);
  }
}

EndgameSolver::~EndgameSolver() = default;

const EndgameSolver::TtEntry* EndgameSolver::Probe(std::uint64_t key) {
  if (table_.empty()) return nullptr;
  ++tt_probes_;
  const TtBucket& bucket = table_[key & table_mask_];
  for (const TtEntry& entry : bucket.entries) {
    if (entry.generation == generation_ && entry.key == key) {
      ++tt_hits_;
      return &entry;
    }
  }
  return nullptr;
}

void EndgameSolver::Store(std::uint64_t key, int depth, int value, int bound,
                          int move) {
  if (table_.empty()) return;
  assert(value > std::numeric_limits<std::int16_t>::min() &&
         value < std::numeric_limits<std::int16_t>::max());
  TtBucket& bucket = table_[key & table_mask_];
  TtEntry& deep = bucket.entries[0];
  TtEntry& recent = bucket.entries[1];
  TtEntry* slot = &recent;
  if (deep.generation != generation_ || deep.key == key ||
      depth >= deep.depth) {
    slot = &deep;
  }
  slot->key = key;
  slot->value = static_cast<std::int16_t>(value);
  slot->move = move >= 0 && move < kNoMove ? static_cast<std::uint16_t>(move)
                                           : kNoMove;
  slot->depth = static_cast<std::uint8_t>(depth);
  slot->bound = static_cast<std::uint8_t>(bound);
  slot->generation = generation_;
}

int EndgameSolver::GoOutValue(int opponent_rack_value) const {
  switch (end_rules_) {
    case EndRules::kNaspa:
      return 2 * opponent_rack_value;
    case EndRules::kWespa:
      // +value to the player going out, -value to the opponent.
      return opponent_rack_value + opponent_rack_value;
  }
  return 2 * opponent_rack_value;
}

bool EndgameSolver::ShouldStop() {
  if (stopped_) return true;
  if (!stoppable_) return false;
  if (limits_.max_nodes > 0 && nodes_ >= limits_.max_nodes) {
    stopped_ = true;
  } else if (limits_.max_time_ms > 0 && nodes_ >= next_clock_check_) {
    // A threshold rather than nodes_ % interval: leaves are counted in
    // batches, so nodes_ can step over any particular multiple.
    next_clock_check_ = nodes_ + kClockInterval;
    const auto elapsed = std::chrono::steady_clock::now() - start_;
    stopped_ = elapsed >= std::chrono::milliseconds(limits_.max_time_ms);
  }
  return stopped_;
}

void EndgameSolver::OrderMoves(int ply_index, int preferred) {
  Ply& ply = plies_[static_cast<std::size_t>(ply_index)];
  const Rack& rack = racks_[static_cast<std::size_t>(side_)];
  ply.order.clear();
  const int count = static_cast<int>(ply.moves.size());
  for (int i = 0; i < count; ++i) {
    const Move& move = ply.moves[static_cast<std::size_t>(i)];
    int key = 0;
    if (!options_.order_moves) {
      key = 0;
    } else if (i == preferred) {
      key = kPreferredKey;
    } else if (move.type != MoveType::kPlace) {
      key = kPassKey;
    } else if (GoesOut(move, rack)) {
      key = kGoOutKey + move.score;
    } else {
      // Unloading face value matters because whatever is left when the
      // opponent goes out costs double.
      key = move.score + UnloadedValue(move);
    }
    ply.order.push_back({key, i});
  }
  if (!options_.order_moves) return;
  // Index as tie-break makes the order total, hence deterministic.
  std::sort(ply.order.begin(), ply.order.end(),
            [](const OrderedMove& a, const OrderedMove& b) {
              return a.key != b.key ? a.key > b.key : a.index < b.index;
            });
}

void EndgameSolver::Make(const Move& move, const Rack& after,
                         int scoreless_after, Saved* saved) {
  Rack& rack = racks_[static_cast<std::size_t>(side_)];
  saved->rack = rack;
  saved->hash = hash_;
  saved->scoreless = scoreless_;
  saved->position = position_->Apply(move);
  hash_ = Zobrist::Instance().Update(hash_, move, side_, rack, after,
                                     scoreless_, scoreless_after);
  rack = after;
  scoreless_ = scoreless_after;
  side_ = 1 - side_;
}

void EndgameSolver::Unmake(const Move& move, const Saved& saved) {
  side_ = 1 - side_;
  racks_[static_cast<std::size_t>(side_)] = saved.rack;
  hash_ = saved.hash;
  scoreless_ = saved.scoreless;
  position_->Undo(move, saved.position);
}

// Fail-soft negamax. Returns the spread still to come for the side to move,
// and sets *exact when no horizon guess influenced the result (then a bound
// it implies is a bound on the true value, not just on a guess).
int EndgameSolver::Search(int ply, int depth, int alpha, int beta,
                          bool* exact) {
  ++nodes_;
  pv_length_[static_cast<std::size_t>(ply)] = 0;
  if (ShouldStop()) {
    *exact = false;
    return 0;
  }
  const Rack& own = racks_[static_cast<std::size_t>(side_)];
  const Rack& opponent = racks_[static_cast<std::size_t>(1 - side_)];
  const int own_value = own.Score();
  const int opponent_value = opponent.Score();
  if (depth <= 0 || ply >= kEndgameMaxPlies - 1) {
    // Horizon guess: the settlement if the game ended on scoreless turns
    // here. It rewards unloading heavy tiles, which is the main thing a
    // shallow search would otherwise miss.
    *exact = false;
    return opponent_value - own_value;
  }

  const int alpha_original = alpha;
  int preferred = ply == 0 ? root_preferred_ : -1;
  if (const TtEntry* entry = Probe(hash_); entry != nullptr) {
    if (entry->move != kNoMove) preferred = entry->move;
    // Never cut at the root: Solve() needs the root's best move and line.
    if (ply > 0 && entry->depth >= depth) {
      const int value = entry->value;
      if (entry->bound == kExact ||
          (entry->bound == kLower && value >= beta) ||
          (entry->bound == kUpper && value <= alpha)) {
        *exact = entry->depth == kSolvedDepth;
        return value;
      }
    }
  }

  Ply& scratch = plies_[static_cast<std::size_t>(ply)];
  generator_.Generate(*position_, own, scratch.moves,
                      {.include_exchanges = false, .include_pass = true});
  if (preferred >= static_cast<int>(scratch.moves.size())) preferred = -1;
  OrderMoves(ply, preferred);

  int best = -kInfinity;
  int best_index = -1;
  bool all_exact = true;
  for (const OrderedMove& ordered : scratch.order) {
    const Move& move = scratch.moves[static_cast<std::size_t>(ordered.index)];
    int value;
    bool child_exact = true;
    bool searched = false;
    if (GoesOut(move, own)) {
      ++nodes_;
      value = move.score + GoOutValue(opponent_value);
    } else {
      const int scoreless_after = move.score == 0 ? scoreless_ + 1 : 0;
      const Rack after = LeaveAfter(own, move);
      if (scoreless_after >= kScorelessTurnsToEnd) {
        // Both players lose their own remaining tiles.
        ++nodes_;
        value = move.score - after.Score() + opponent_value;
      } else if (depth == 1 || ply + 1 >= kEndgameMaxPlies - 1) {
        // The child is a horizon leaf, whose guess needs only rack values:
        // skip the make/unmake (a cross-check update) per leaf.
        ++nodes_;
        child_exact = false;
        value = move.score - (after.Score() - opponent_value);
      } else {
        Saved saved;
        Make(move, after, scoreless_after, &saved);
        const int s = move.score;
        if (best_index < 0 || beta - alpha == 1) {
          value = s - Search(ply + 1, depth - 1, -(beta - s), -(alpha - s),
                             &child_exact);
        } else {
          // Principal-variation search: with good ordering the first move
          // is usually best, so later ones only need proof that they are
          // no better (a null window around alpha), which prunes far more.
          value = s - Search(ply + 1, depth - 1, -(alpha + 1 - s), -(alpha - s),
                             &child_exact);
          if (value > alpha && value < beta && !stopped_) {
            value = s - Search(ply + 1, depth - 1, -(beta - s), -(alpha - s),
                               &child_exact);
          }
        }
        Unmake(move, saved);
        if (stopped_) {
          *exact = false;
          return 0;
        }
        searched = true;
      }
    }
    all_exact = all_exact && child_exact;
    if (value > best) {
      best = value;
      best_index = ordered.index;
      if (value > alpha) {
        alpha = value;
        auto& line = pv_[static_cast<std::size_t>(ply)];
        line[0] = move;
        const int child_length =
            searched ? pv_length_[static_cast<std::size_t>(ply + 1)] : 0;
        std::copy_n(pv_[static_cast<std::size_t>(ply + 1)].begin(),
                    child_length, line.begin() + 1);
        pv_length_[static_cast<std::size_t>(ply)] = child_length + 1;
        if (alpha >= beta) break;
      }
    }
  }

  *exact = all_exact;
  if (ply == 0) root_preferred_ = best_index;
  // Fail-soft bounds: at or below the original alpha every move was
  // refuted, so `best` only bounds the true value from above; at or above
  // beta the search stopped early, so it bounds it from below.
  const int bound = best <= alpha_original ? kUpper
                    : best >= beta         ? kLower
                                           : kExact;
  Store(hash_, all_exact ? kSolvedDepth : depth, best, bound, best_index);
  return best;
}

void EndgameSolver::ExtendPv(std::vector<Move>& pv) {
  // Replay the line, then follow solved table entries to the end of the
  // game; everything is undone afterwards.
  std::vector<Saved> saved;
  saved.reserve(kEndgameMaxPlies);
  std::vector<Move> played;
  played.reserve(kEndgameMaxPlies);
  bool over = false;
  const auto play = [&](const Move& move) {
    const Rack& own = racks_[static_cast<std::size_t>(side_)];
    const int scoreless_after = move.score == 0 ? scoreless_ + 1 : 0;
    over = GoesOut(move, own) || scoreless_after >= kScorelessTurnsToEnd;
    const Rack after = LeaveAfter(own, move);
    Make(move, after, scoreless_after, &saved.emplace_back());
    played.push_back(move);
  };
  for (const Move& move : pv) {
    if (over) break;
    play(move);
  }
  Ply& scratch = plies_[0];
  while (!over && static_cast<int>(played.size()) < kEndgameMaxPlies) {
    const TtEntry* entry = Probe(hash_);
    if (entry == nullptr || entry->depth != kSolvedDepth ||
        entry->bound != kExact || entry->move == kNoMove) {
      break;
    }
    generator_.Generate(*position_, racks_[static_cast<std::size_t>(side_)],
                        scratch.moves,
                        {.include_exchanges = false, .include_pass = true});
    if (entry->move >= scratch.moves.size()) break;
    const Move move = scratch.moves[entry->move];
    pv.push_back(move);
    play(move);
  }
  for (std::size_t i = played.size(); i-- > 0;) Unmake(played[i], saved[i]);
}

EndgameResult EndgameSolver::Solve(Position& position,
                                   const EndgameState& state,
                                   const EndgameLimits& limits) {
  assert(!state.racks[0].IsEmpty() && !state.racks[1].IsEmpty());
  assert(state.scoreless_turns >= 0 &&
         state.scoreless_turns < kScorelessTurnsToEnd);
  start_ = std::chrono::steady_clock::now();
  limits_ = limits;
  nodes_ = 0;
  next_clock_check_ = kClockInterval;
  tt_probes_ = 0;
  tt_hits_ = 0;
  stopped_ = false;
  stoppable_ = false;
  root_preferred_ = -1;
  if (++generation_ == 0) {
    // Wrapped: entries from 255 solves ago would look current again.
    std::fill(table_.begin(), table_.end(), TtBucket{});
    generation_ = 1;
  }

  position_ = &position;
  racks_ = state.racks;
  side_ = state.to_move;
  scoreless_ = state.scoreless_turns;
  end_rules_ = state.end_rules;
  hash_ = Zobrist::Instance().Hash(position.board(), racks_, side_,
                                   scoreless_);

  EndgameResult result;
  const int max_depth =
      std::clamp(limits.max_depth, 1, kEndgameMaxPlies - 1);
  for (int depth = 1; depth <= max_depth; ++depth) {
    // Depth 1 always runs to completion so there is always a move.
    stoppable_ = depth > 1;
    bool exact = false;
    const int value = Search(0, depth, -kInfinity, kInfinity, &exact);
    if (stopped_) break;
    result.value = state.spread + value;
    result.depth = depth;
    result.pv.assign(pv_[0].begin(), pv_[0].begin() + pv_length_[0]);
    if (exact) {
      result.complete = true;
      break;
    }
  }
  if (result.complete) ExtendPv(result.pv);
  assert(!result.pv.empty());
  result.best_move = result.pv.front();
  result.best_move.equity = result.value;
  result.nodes = nodes_;
  result.tt_probes = tt_probes_;
  result.tt_hits = tt_hits_;
  result.seconds = std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - start_)
                       .count();
  position_ = nullptr;
  return result;
}

}  // namespace scrabble
