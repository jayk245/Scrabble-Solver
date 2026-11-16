// Authoritative two-player game state and rules enforcement.
//
// Game is the referee: it owns the board, both racks, the bag and the scores,
// validates every move against them, and is the only place tiles change
// hands. Bots never see a Game; they receive a PlayerView, which carries
// exactly the information a player is entitled to (in particular, not the
// opponent's rack).
//
// Rules decisions:
//  * Player 0 always moves first. Callers that alternate who starts (the
//    arena) do so by choosing which bot sits in seat 0.
//  * An exchange needs at least kMinBagForExchange (7) tiles in the bag.
//    Replacement tiles are drawn before the exchanged tiles go back.
//  * The game ends when a player plays out with the bag empty, or after
//    kScorelessTurnsToEnd (6) consecutive scoreless turns. Passes,
//    exchanges and zero-point placements (e.g. two blanks) are all
//    scoreless turns.
//  * End-of-game rack adjustments follow EndRules (see below). When the game
//    ends on scoreless turns, both rule sets subtract each player's own rack
//    value from their score.

#ifndef SCRABBLE_GAME_H_
#define SCRABBLE_GAME_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "scrabble/bag.h"
#include "scrabble/board.h"
#include "scrabble/gaddag.h"
#include "scrabble/move.h"
#include "scrabble/move_validation.h"
#include "scrabble/position.h"
#include "scrabble/rack.h"
#include "scrabble/rng.h"
#include "scrabble/tile.h"

namespace scrabble {

inline constexpr int kNumPlayers = 2;

// Exchanging is legal only while the bag holds at least a full rack.
inline constexpr int kMinBagForExchange = kRackSize;

// Consecutive scoreless turns (by both players combined) that end the game.
// Six is the NASPA and WESPA standard: three rounds in which nobody scored.
inline constexpr int kScorelessTurnsToEnd = 6;

// How the end-of-game rack adjustment is applied when a player goes out.
enum class EndRules : std::uint8_t {
  // North American: the player who goes out gains twice the face value of
  // the opponent's remaining tiles; the opponent's score is unchanged.
  kNaspa,
  // International: the player who goes out gains the opponent's rack value
  // and the opponent loses the same amount. The spread swing equals NASPA's.
  kWespa,
};

struct GameConfig {
  EndRules end_rules = EndRules::kNaspa;
};

enum class GameStatus : std::uint8_t {
  kInProgress,
  kWentOut,         // a player emptied their rack with the bag empty
  kScorelessTurns,  // kScorelessTurnsToEnd consecutive scoreless turns
};

enum class PlayError : std::uint8_t {
  kOk,
  kGameOver,            // the game has already ended
  kIllegalMove,         // ValidateMove() rejected it; see move_error
  kExchangeNotAllowed,  // fewer than kMinBagForExchange tiles in the bag
};

struct PlayResult {
  PlayError error = PlayError::kOk;
  MoveError move_error = MoveError::kOk;  // set when error == kIllegalMove

  bool ok() const { return error == PlayError::kOk; }
};

// Human-readable reason, e.g. for the web UI.
std::string_view PlayErrorMessage(const PlayResult& result);

// One entry of the move history.
struct MoveRecord {
  int player = 0;
  // Canonicalised by ValidateMove(): play-through letters resolved, score
  // filled in. `equity` is whatever the mover reported (bots set it).
  Move move;
  Rack rack_before;      // the mover's full rack before the move
  Rack drawn;            // tiles drawn afterwards (replacements, for exchange)
  int bag_before = 0;    // tiles in the bag before the move
  int score = 0;         // points for this move
  int cumulative = 0;    // the mover's total after this move
};

// Everything a player may know when choosing a move. A plain value built by
// Game::View(); other front ends (e.g. a server analysing a user-entered
// position) can fill one in directly.
//
// `unseen` is the bag and the opponent's rack combined, which is what a
// player can actually deduce: tile tracking tells you which tiles are out,
// not where they are. Monte Carlo simulation (M7) samples opponent racks
// from it.
struct PlayerView {
  const Position* position = nullptr;  // never null in a valid view
  Rack rack;
  int player = 0;
  int score = 0;
  int opponent_score = 0;
  int bag_size = 0;
  int opponent_rack_size = 0;
  std::array<std::uint8_t, kAlphabetSize> unseen{};
  int unseen_size = 0;  // bag_size + opponent_rack_size
  int scoreless_turns = 0;
  int turn = 0;
  EndRules end_rules = EndRules::kNaspa;
  // The opponent's exact rack, when an analysis front end knows it (e.g. a
  // user entering a position from a finished game). Must be a sub-multiset
  // of `unseen` with opponent_rack_size tiles. Simulation then samples only
  // the bag order instead of the opponent's rack. Game never sets it: a
  // player is not entitled to see the opponent's tiles.
  std::optional<Rack> known_opponent_rack;

  bool CanExchange() const { return bag_size >= kMinBagForExchange; }
};

// Tiles not on `board` and not on `own_rack`, from the full distribution:
// what a player with `own_rack` would count as unseen. Blanks on the board
// count as blanks, not as their designated letter. Equals PlayerView::unseen
// for any view Game produces from a full-bag start.
std::array<std::uint8_t, kAlphabetSize> UnseenTiles(const Board& board,
                                                    const Rack& own_rack);

// An explicit starting state, for tests and custom positions. No tile
// conservation is enforced: racks and bag may hold any tiles.
struct GameSetup {
  Board board;
  std::array<Rack, kNumPlayers> racks;
  Bag bag;
};

// Not thread-safe; give each thread its own Game. Copyable (it owns all its
// state except the lexicon), which lets callers snapshot a game.
class Game {
 public:
  // A new game: full bag, racks drawn for player 0 then player 1. All
  // randomness comes from `seed`, so equal seeds and equal move sequences
  // give identical games. `gaddag` must outlive the Game.
  Game(const Gaddag& gaddag, std::uint64_t seed, const GameConfig& config = {});

  // Starts from `setup` instead of a fresh bag. Racks are taken as given
  // (not topped up); `seed` drives later draws.
  Game(const Gaddag& gaddag, const GameSetup& setup, std::uint64_t seed,
       const GameConfig& config = {});

  // Restarts as a new game from a full bag, reusing the history buffer.
  void Reset(std::uint64_t seed);

  // Plays `move` for the player to move: validates it, updates board,
  // rack, score and bag, records it, and applies end-of-game adjustments if
  // it ends the game. On any error nothing changes.
  PlayResult Play(const Move& move);

  // The view for `player`.
  PlayerView View(int player) const;
  PlayerView ViewForMover() const { return View(to_move_); }

  const Position& position() const { return position_; }
  const Board& board() const { return position_.board(); }
  const Gaddag& lexicon() const { return position_.lexicon(); }
  const GameConfig& config() const { return config_; }

  // Full knowledge, for the referee, logs and tests. Do not hand these to a
  // bot.
  const Rack& rack(int player) const { return racks_[Idx(player)]; }
  const Bag& bag() const { return bag_; }

  int score(int player) const { return scores_[Idx(player)]; }
  // score(player) - score(opponent).
  int spread(int player) const { return score(player) - score(1 - player); }
  int to_move() const { return to_move_; }
  // Number of moves played so far (the next move's zero-based index).
  int turn() const { return static_cast<int>(history_.size()); }
  int scoreless_turns() const { return scoreless_turns_; }
  GameStatus status() const { return status_; }
  bool IsOver() const { return status_ != GameStatus::kInProgress; }
  // The player who went out; -1 unless status() == kWentOut.
  int went_out() const { return went_out_; }
  // Points added to (positive) or subtracted from each player's score by
  // the end-of-game rack adjustment; zero while the game is in progress.
  int end_adjustment(int player) const {
    return end_adjustments_[Idx(player)];
  }
  const std::vector<MoveRecord>& history() const { return history_; }

 private:
  static std::size_t Idx(int player) {
    return static_cast<std::size_t>(player);
  }

  void Finish(GameStatus status);

  GameConfig config_;
  Position position_;
  std::array<Rack, kNumPlayers> racks_;
  Bag bag_;
  Rng rng_;
  std::array<int, kNumPlayers> scores_{};
  std::array<int, kNumPlayers> end_adjustments_{};
  int to_move_ = 0;
  int scoreless_turns_ = 0;
  int went_out_ = -1;
  GameStatus status_ = GameStatus::kInProgress;
  std::vector<MoveRecord> history_;
};

}  // namespace scrabble

#endif  // SCRABBLE_GAME_H_
