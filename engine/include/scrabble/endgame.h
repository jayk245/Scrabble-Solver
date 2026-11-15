// Exact endgame search for the empty-bag phase.
//
// Once the bag is empty nothing more is drawn, so each player's rack is
// known to the other (it is exactly their unseen tiles). The rest of the
// game is then a finite two-player, zero-sum game of perfect information,
// which EndgameSolver searches with negamax, alpha-beta pruning, iterative
// deepening and a transposition table.
//
// Rules (identical to Game, so solver values are what Game scores):
//  * Every legal placement and the pass are moves. Exchanges are illegal
//    with an empty bag and are never generated.
//  * A placement that empties the mover's rack ends the game. Under either
//    EndRules the spread then swings by twice the opponent's rack value
//    (NASPA: +2x to the player going out; WESPA: +x to them and -x to the
//    opponent).
//  * Every scoreless turn (a pass, or a placement scoring zero, e.g. a word
//    of blanks) increments the scoreless-turn counter and every scoring turn
//    resets it. When it reaches kScorelessTurnsToEnd the game ends and each
//    player loses their own rack value. Two consecutive passes therefore do
//    not end the game by themselves: the position recurs with a higher
//    counter, and the counter alone bounds how long passing can go on. The
//    counter is part of the search state (and of the hash) for this reason.
//
// Values are spreads: final score of the player to move minus the
// opponent's, after end-of-game adjustments. Internally a node's value is
// the spread still to come from that node, which does not depend on how the
// node was reached; that is what makes transposition-table entries valid
// across move orders.
//
// Search outline:
//  * Iterative deepening by plies. A node at the depth horizon is scored by
//    a static guess (the scoreless-ending settlement: opponent's rack value
//    minus the mover's). Each node also reports whether its value is exact,
//    i.e. no horizon guess influenced it; when the root is exact the solve
//    is complete and deepening stops.
//  * Move ordering: the transposition-table (or previous iteration's) move,
//    then going-out moves, then placements by score plus the face value
//    they unload, then the pass. Moves after the first are searched with a
//    null window first (principal-variation search) and re-searched only
//    if they turn out better.
//  * Transposition table: Zobrist keys over board squares, both racks, side
//    to move and the scoreless counter. Two-entry buckets, one
//    depth-preferred and one always-replace. Exactly solved subtrees are
//    stored with an unbounded draft so later iterations reuse them.
//  * Make/unmake through Position::Apply()/Undo(); racks, hash and counter
//    are saved by value. Move lists live in per-ply buffers, so a node
//    performs no heap allocation once the buffers have grown.

#ifndef SCRABBLE_ENDGAME_H_
#define SCRABBLE_ENDGAME_H_

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "scrabble/board.h"
#include "scrabble/gaddag.h"
#include "scrabble/game.h"
#include "scrabble/move.h"
#include "scrabble/move_generator.h"
#include "scrabble/position.h"
#include "scrabble/rack.h"
#include "scrabble/tile.h"

namespace scrabble {

// Deepest line the solver can represent. A real endgame is far shorter:
// at most 2 * kRackSize placements, with at most kScorelessTurnsToEnd - 1
// scoreless turns between consecutive scoring ones.
inline constexpr int kEndgameMaxPlies = 128;

// The state an endgame search starts from. The board is passed separately
// as a Position.
struct EndgameState {
  // Indexed by player. Both must be non-empty (otherwise the game is over).
  std::array<Rack, kNumPlayers> racks;
  int to_move = 0;
  // Score of `to_move` minus the opponent's, before the search.
  int spread = 0;
  // Consecutive scoreless turns so far; less than kScorelessTurnsToEnd.
  int scoreless_turns = 0;
  EndRules end_rules = EndRules::kNaspa;
};

// When to stop early. A stopped solve returns the result of the last fully
// searched depth (depth 1 is always completed, so a move is always found).
struct EndgameLimits {
  std::int64_t max_nodes = 0;    // 0 = unlimited
  std::int64_t max_time_ms = 0;  // 0 = unlimited
  int max_depth = kEndgameMaxPlies;
};

struct EndgameOptions {
  // Transposition-table size, rounded down to a power-of-two bucket count.
  // 0 disables the table (for testing; the search stays exact).
  std::size_t tt_size_mb = 16;
  // false searches moves in generation order (for testing; values are the
  // same either way, only the node count changes).
  bool order_moves = true;
};

struct EndgameResult {
  // Best move found, with `score` set and `equity` = `value`.
  Move best_move;
  // Final spread for the player to move (EndgameState::spread included)
  // under best play by both sides. Exact iff `complete`; otherwise the
  // depth-limited estimate from depth `depth`.
  int value = 0;
  // Principal variation starting with best_move. May stop short of the end
  // of the game if a needed table entry was overwritten.
  std::vector<Move> pv;
  bool complete = false;
  int depth = 0;  // last fully searched depth
  std::int64_t nodes = 0;
  double seconds = 0.0;
  std::int64_t tt_probes = 0;
  std::int64_t tt_hits = 0;  // probes that found an entry for the position
};

// Zobrist keys for endgame states. A hash is the XOR of one key per occupied
// square (letter and blank flag), one per (player, letter, count) for both
// racks, one for the scoreless counter, and one more when player 1 is to
// move. Keys come from a fixed seed, so hashes are stable across runs.
class Zobrist {
 public:
  static const Zobrist& Instance();

  // Full recomputation from scratch; the reference for Update().
  std::uint64_t Hash(const Board& board,
                     const std::array<Rack, kNumPlayers>& racks, int to_move,
                     int scoreless_turns) const;

  // The hash after `player` plays `move`, taking the rack from `before` to
  // `after` and the scoreless counter from `scoreless_before` to
  // `scoreless_after`. Side to move flips.
  std::uint64_t Update(std::uint64_t hash, const Move& move, int player,
                       const Rack& before, const Rack& after,
                       int scoreless_before, int scoreless_after) const;

 private:
  Zobrist();

  std::uint64_t RackKey(int player, const Rack& rack) const;

  // [square][letter + kNumLetters * is_blank]
  std::array<std::array<std::uint64_t, 2 * kNumLetters>, kNumSquares> square_;
  // [player][letter][count]
  std::array<std::array<std::array<std::uint64_t, kRackSize + 1>,
                        kAlphabetSize>,
             kNumPlayers>
      rack_;
  std::array<std::uint64_t, kScorelessTurnsToEnd> scoreless_;
  std::uint64_t side_ = 0;
};

// Reusable solver: owns the transposition table and per-ply buffers, so
// construct it once and call Solve() many times. Not thread-safe; give each
// thread its own.
//
// Determinism: with no time limit, Solve() is a pure function of its
// arguments. Entries left by earlier calls are invalidated (by a generation
// counter, in O(1)), so call history never changes a result.
class EndgameSolver {
 public:
  // `gaddag` must outlive the solver and be the lexicon of every Position
  // passed to Solve().
  explicit EndgameSolver(const Gaddag& gaddag,
                         const EndgameOptions& options = {});
  ~EndgameSolver();

  EndgameSolver(const EndgameSolver&) = delete;
  EndgameSolver& operator=(const EndgameSolver&) = delete;

  // Searches the endgame from `position` and `state`. `position` is mutated
  // during the search and restored bit for bit before returning.
  // Preconditions: as documented on EndgameState. Tile conservation is not
  // required (tests solve truncated racks), only that both racks are
  // non-empty and the game is not already over.
  EndgameResult Solve(Position& position, const EndgameState& state,
                      const EndgameLimits& limits = {});

 private:
  struct TtEntry;
  struct TtBucket;
  struct OrderedMove {
    int key;
    int index;
  };
  // Scratch for one ply. Vectors are cleared, never shrunk.
  struct Ply {
    std::vector<Move> moves;
    std::vector<OrderedMove> order;
  };
  // What Make() changes, for Unmake().
  struct Saved {
    Rack rack;
    std::uint64_t hash;
    int scoreless;
    PositionUndo position;
  };

  int Search(int ply, int depth, int alpha, int beta, bool* exact);
  void OrderMoves(int ply, int preferred);
  void Make(const Move& move, const Rack& after, int scoreless_after,
            Saved* saved);
  void Unmake(const Move& move, const Saved& saved);
  int GoOutValue(int opponent_rack_value) const;
  bool ShouldStop();
  void ExtendPv(std::vector<Move>& pv);

  const TtEntry* Probe(std::uint64_t key);
  void Store(std::uint64_t key, int depth, int value, int flag, int move);

  EndgameOptions options_;
  MoveGenerator generator_;

  // Transposition table; empty when disabled.
  std::vector<TtBucket> table_;
  std::uint64_t table_mask_ = 0;
  std::uint8_t generation_ = 0;

  std::vector<Ply> plies_;
  // Triangular principal-variation table: pv_[ply] is the best line found
  // from that ply, pv_length_[ply] moves long.
  std::vector<std::array<Move, kEndgameMaxPlies>> pv_;
  std::array<int, kEndgameMaxPlies + 1> pv_length_{};

  // Search state, valid during Solve().
  Position* position_ = nullptr;
  std::array<Rack, kNumPlayers> racks_;
  int side_ = 0;
  int scoreless_ = 0;
  std::uint64_t hash_ = 0;
  EndRules end_rules_ = EndRules::kNaspa;
  int root_preferred_ = -1;  // previous iteration's best root move index

  // Limits and statistics.
  EndgameLimits limits_;
  std::chrono::steady_clock::time_point start_;
  bool stoppable_ = false;
  bool stopped_ = false;
  std::int64_t nodes_ = 0;
  std::int64_t next_clock_check_ = 0;
  std::int64_t tt_probes_ = 0;
  std::int64_t tt_hits_ = 0;
};

}  // namespace scrabble

#endif  // SCRABBLE_ENDGAME_H_
