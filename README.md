# Scrabble Solver

A Scrabble engine in C++20, built toward Monte Carlo simulation and an exact
endgame solver, with a web UI for visualisation and bot-vs-bot benchmarking.

## Layout

```
engine/   Core library: board, tiles, lexicon, move generation, search
apps/     Executables built on the engine (CLI today; server/arena later)
tests/    GoogleTest unit tests
cmake/    Shared build configuration
```

## Building

Requires CMake ≥ 3.24, Ninja and a C++20 compiler (Clang 15+ / GCC 12+).

```sh
cmake --preset release          # -O3, -march=native, LTO
cmake --build --preset release
ctest --preset release

./build/release/apps/scrabble_cli
```

Use the `debug` preset for development; it enables AddressSanitizer and
UndefinedBehaviorSanitizer.

## Lexicon

Word lists are not committed. Fetch the public-domain ENABLE list (verified
by checksum) into `data/lexica/`:

```sh
scripts/fetch_lexicon.sh
```

Then compile it to a binary GADDAG, which the engine loads in milliseconds
instead of rebuilding:

```sh
./build/release/apps/scrabble_lexicon_tool data/lexica/enable1.txt
```

This writes `data/lexica/enable1.gaddag` and prints word, node and arc
counts, memory size and build/load times. Words are uppercased and filtered
to 2-15 letters A-Z (168,551 of ENABLE's 172,823 entries). The ENABLE
integration tests are skipped if the list has not been fetched.

## Web UI

An analysis board in the browser: set up a position, enter a rack, and
browse every legal move ranked by score with board previews. The C++ server
(`apps/server`, cpp-httplib + nlohmann/json) loads the GADDAG once and
answers stateless JSON requests; the frontend (`web/`) is React +
TypeScript built with Vite. Requires Node.js 20.19+ or 22.12+.

Production (one process; the server also serves `web/dist`):

```sh
(cd web && npm ci && npm run build)
./build/release/apps/server/scrabble_server   # http://127.0.0.1:8080
```

Development (hot reload; Vite proxies `/api` to the server):

```sh
./build/release/apps/server/scrabble_server   # terminal 1
(cd web && npm run dev)                       # terminal 2: http://localhost:5173
```

Server flags: `--port` (8080), `--host` (127.0.0.1), `--lexicon PATH`,
`--static-dir DIR`. Endpoints are documented in `apps/server/api.h`, and
the JSON schema in `web/src/api/types.ts`.

## Move generation benchmark

```sh
./build/release/apps/scrabble_movegen_bench --games 1000 --seed 1
```

Plays seeded games between uniformly random movers (a benchmark driver,
not a bot) and reports positions/sec, moves generated/sec and average moves
per position. `--lexicon PATH` selects a word list (default ENABLE). The
printed checksum must stay fixed for a given seed across optimisations.

## Arena

```sh
./build/release/apps/scrabble_arena --games 2000 --threads 8 --seed 1 \
    --leaves-a learned --leaves-b heuristic
```

Plays bot configuration A against B (alternating who moves first) and
reports A's win/loss/tie record, mean scores and spread with 95% confidence
intervals, bingos per game and games/sec. Results depend only on `--seed`,
never on `--threads`. Leaves are `learned` (default: the learned table
built into the engine), `heuristic`, `zero` or a `leave,value` CSV file. `--rules naspa|wespa` selects end-of-game scoring,
`--gcg-dir DIR` writes every game as GCG, and `--log FILE` writes a JSONL
move log (schema in `engine/include/scrabble/arena.h`) for leave training.

## Training leave values

A move's static equity is its score plus the value of the tiles it keeps
(the *leave*). Learned leave values come from self-play:

```sh
python3 tools/leaves/train.py      # after a release build; numpy only
```

Each generation plays 200,000 self-play games with the previous
generation's leaves (generation 0 is the hand-written heuristic), streams
the arena's JSONL log through a named pipe (so no multi-GB log is written
to disk), and fits a value for every possible leave of 1-6 tiles:

1. **Target.** For each move made with at least 7 tiles in the bag, the
   mover's net points over the rest of the game, excluding the move itself
   (`final_spread - spread - score`). Targets are centred per bag size and
   weighted by the inverse of each bag size's variance.
2. **Prior.** A weighted least-squares linear model over simple features:
   copies held of each tile, a shared duplicate penalty, leave length,
   vowel/consonant balance and Q without U. It values every leave,
   including the ~820,000 never seen.
3. **Shrinkage.** An observed leave's value is `(W·mean + k·prior) / (W + k)`,
   with `k` (per leave length) estimated from the data (empirical Bayes), so
   rarely seen leaves stay close to the prior.

Values are anchored so that the empty leave is worth 0, as the bot
assumes. Two generations take about 8 minutes on 8 cores (peak memory
about 400 MB). `--games`, `--generations` and `--eval-games` are flags,
and the script reuses cached self-play data in `data/leaves/work/`.
Outputs:

* `data/leaves/enable_leaves_default.csv` (committed, 0.3 MB): every leave
  of up to 4 tiles, plus the linear model as `@` rows that
  `LeaveEvaluator` uses for longer leaves.
* `data/leaves/enable_leaves.csv` (12 MB, gitignored): every leave of 1-6
  tiles.

The default table is compiled into the engine: CMake converts the CSV
into a byte array at build time (`cmake/EmbedFile.cmake`, rerun whenever
the CSV changes), and `LeaveEvaluator::Learned()` parses it. Every bot
uses it unless told otherwise (`BotConfig::leaves` defaults to
`DefaultLeaves()`, one shared instance), so binaries do not depend on
where they run from or on a data path. Use either table file with
`--leaves-a FILE`, or `--leaves-a heuristic` for the old baseline. Python
tests:
`python3 -m unittest discover -s tools/leaves`.

Arena results, 10,000 games each (seed 1999, 95% CIs):

| A vs B | A win rate | A spread |
|--------|-----------:|---------:|
| gen 1 vs heuristic | 52.0% ± 1.0 | +4.1 ± 1.9 |
| gen 2 vs gen 1 | 49.9% ± 1.0 | −1.3 ± 1.9 |
| final (gen 2) vs heuristic | 51.6% ± 1.0 | +3.5 ± 1.9 |
| final vs zero | 63.6% ± 0.9 | +33.9 ± 1.9 |
| heuristic vs zero | 62.7% ± 0.9 | +30.3 ± 1.9 |
| default (≤4 tiles + model) vs final | 50.2% ± 1.0 | −1.1 ± 1.9 |

The second generation gives no further gain, so training stops there. The
learned values beat the heuristic by about 4 points a game. Single tiles
come out as blank +29.5, S +8.4, X +3.4, Z +3.0, E +0.8, …, U −5.3, V −7.2
and Q −14.8. That order matches published tables, but the blank and Q are
more extreme than the commonly cited +25 and −7: these values are tuned to
this greedy static bot. `python3 tools/leaves/report.py TABLE` prints the
best and worst leaves of a table.

## Endgame solver

Once the bag is empty the opponent's rack is exactly the unseen tiles, so
the rest of the game is a two-player, zero-sum game of perfect information.
`EndgameSolver` (`engine/include/scrabble/endgame.h`) searches it exactly:

- Negamax with fail-soft alpha-beta and principal-variation search, plus
  iterative deepening that puts the previous iteration's best move first.
- Move ordering: the table move first, then going-out moves, then
  placements by score plus the face value they unload, then the pass.
- Transposition table: Zobrist keys over board squares, both racks, side
  to move and the scoreless-turn counter. Two-entry buckets (one
  depth-preferred, one always-replace) with exact/lower/upper bound flags.
  A generation counter invalidates the table in O(1) per solve, so results
  never depend on earlier solves.
- Make/unmake goes through `Position::Apply`/`Undo` with per-ply move
  buffers, so there is no Position copy or heap allocation per node.
- The rules match `Game` exactly. Going out swings the spread by twice the
  opponent's rack (under both NASPA and WESPA). Every scoreless turn, a pass
  or a zero-point play, counts toward the six that end the game, and each
  player then loses their own rack. Exchanges are never generated.
- Each node also reports whether a horizon guess touched its value. A solve
  is complete (exact) when the root's value is untouched. Otherwise the
  node or time limit returns the best move from the deepest completed
  depth.

Enable it in the bot with `BotConfig::endgame` (`enabled`, `node_limit`,
`time_limit_ms`, `tt_size_mb`), or in the arena with `--endgame-a` /
`--endgame-b`, `--endgame-nodes N` (default 2,000,000) and
`--endgame-ms T`. Use the node limit when results must be reproducible.

```sh
./build/release/apps/scrabble_endgame_bench --positions 100 --ms 5000 --nodes 0
```

This solves positions taken at the moment the bag empties in seeded
self-play. `--max-tiles K` truncates both racks to study scaling. Results
are from one thread on an Apple Silicon laptop, 5 s per position, 64 MB
table:

| Racks (mover v opp) | Solved exactly | Median | p90 | p99 | Max | Median nodes | Nodes/s | TT hits |
|---|---|---|---|---|---|---|---|---|
| 2 v 2     | 100% | 0.2 ms | 1.1 ms | 3.1 ms | 3.5 ms | 122 | 1.7M | 65% |
| 3 v 3     | 100% | 1.1 ms | 15 ms | 29 ms | 46 ms | 852 | 1.6M | 79% |
| 4 v 3.8   | 100% | 8.1 ms | 140 ms | 561 ms | 1.0 s | 4.1K | 1.9M | 85% |
| 5 v 4.6   | 93%  | 59 ms | 851 ms | 5 s | 5 s | 48K | 1.6M | 80% |
| 7 v 5.5 (as dealt) | 66% | 1.0 s | 5 s | 5 s | 5 s | 1.0M | 1.3M | 57% |

Full-rack endgames are hard. With 13-14 tiles across both racks only half
solve within 5 s; the unsolved ones reach depth 7 (3-11). Next steps: a
better horizon estimate (score plus the opponent's best out-play), a
"stuck tiles" analysis that prunes tiles that can never be played, and
caching per-rack move lists across passes.

Strength check: 5,000 arena games (seed 1, NASPA) of the same heuristic
bot, endgame on (A, 200,000 nodes per move) against endgame off (B):

| | Win rate (A) | Spread (A−B) | Time per game |
|---|---|---|---|
| Endgame on vs off | 52.3% ± 1.4% | +5.4 ± 2.6 | 316 ms |
| Control: off vs off | 49.7% ± 1.4% | −0.1 ± 2.6 | 8 ms |

Intervals are 95%. Time per game is thread-seconds.

## Monte Carlo simulation

Static equity judges a move by its score and the leave it keeps.
Simulation plays the best few candidates forward against sampled futures
and averages what happens. That shows up board effects a leave table
cannot see: openings handed to the opponent, hooks, and setups for the
next turn. `Simulator` (`engine/include/scrabble/simulation.h`) does this
for the bot while the bag still has tiles:

- **Candidates.** The top N moves by static equity (`candidates`,
  default 15), ties in generation order, plus the highest-scoring move if
  it is not among them.
- **One iteration.** Shuffle the unseen tiles (bag plus the opponent's
  rack). The tail becomes the opponent's rack, so it is a uniform sample,
  and the rest is the bag in draw order. For each candidate: play it,
  draw replacements, then play `plies` more moves (default 2: the reply,
  then our next move). Each of those moves is the mover's best static
  move from their actual rack. Value = our points minus theirs over the
  candidate and the simulated plies, plus the leave term of our last move,
  minus the leave term of theirs. Both last leaves are still unplayed, so
  both are counted. Leaving out theirs would favour candidates that give
  the opponent a modest score but a strong leave. With `plies = 0` the
  value is exactly the static equity.
- **Common random numbers.** Every candidate in an iteration sees the
  same opponent rack and bag order, so the luck of the draw cancels when
  candidates are compared. Exchanged tiles go back into a random position
  in the bag.
- **Pruning.** Iterations run in rounds of 16. After `min_iterations`
  (default 64), a candidate is dropped once the leader beats it by more
  than `prune_z` (default 2) standard errors of their *paired* difference.
  The run stops when one candidate is left, `iterations` (default 1,000)
  are spent, or `time_limit_ms` passes.
- **End of the bag.** A rollout keeps going after its bag runs out, and
  plays its remaining plies with the static empty-bag rule. If a player
  goes out, or six turns in a row score nothing, the real end-of-game
  adjustment replaces the leave terms. This is crude, but it keeps every
  rollout the same length. Stopping a rollout when the bag empties would
  score bag-emptying candidates without the opponent's reply, and so
  favour them. Once the real bag is empty, the endgame solver takes over.
- **Determinism and threads.** Iteration *i* is seeded from (decision
  seed, *i*) alone. Work items (iteration × candidate) are spread over a
  thread pool. After each round, a `std::barrier` completion step adds up
  the values in a fixed order. So results are bit-identical for any
  thread count, unless a time limit is set.
- **No per-rollout allocation.** Each thread has its own Position (one
  copy per decision, then only `Apply`/`Undo`), MoveGenerator, move
  buffer and StaticEvaluator.

`Run()` returns a `SimulationResult`. For each candidate it holds the
iteration count, mean, standard deviation, standard error and the paired
difference from the chosen move (with its standard error), plus totals,
time and the stop reason. `Bot::last_simulation()` exposes it after
`ChooseMove()`.

Enable it with `BotConfig::simulation` (`enabled`, `candidates`,
`iterations`, `plies`, `min_iterations`, `prune_z`, `threads`,
`time_limit_ms`, `common_random_numbers`), or in the arena:

```sh
./build/release/apps/scrabble_arena --games 2000 --threads 8 --seed 2024 \
    --sim-a --endgame-a --endgame-b \
    --sim-candidates 10 --sim-iterations 80 --sim-min-iterations 32
```

Other flags are `--sim-b`, `--sim-plies`, `--sim-prune-z`, `--sim-ms` and
`--sim-no-crn`. `--sim-threads` defaults to 1, because the arena already
runs one game per thread. Use more threads for interactive analysis. To
benchmark:

```sh
./build/release/apps/scrabble_sim_bench --positions 30 --candidates 10 \
    --iterations 80 --min-iterations 32 --threads 8 --crn-compare
```

Results on an Apple M3 (4P+4E cores). The machine had heavy unrelated
load during these runs (load average 20–60), so the times are
pessimistic:

| Setting | Threads | Time/decision (mean / median) | Rollouts/s |
|---|---|---|---|
| 10 × 80 × 2 plies (arena) | 1 | 320 / 238 ms | 1,650 |
| 10 × 80 × 2 plies | 8 | 65 / 47 ms | 8,200 |
| 20 × 1,000 × 2 plies (interactive) | 8 | 491 / 365 ms | 5,600 |

A rollout costs about 0.6 ms on one thread, and nearly all of that is
move generation. The player's own rack usually keeps the blank (the
learned table values it at +29.5), and racks with a blank produce
thousands of moves. Pruning cut the interactive setting to 607 of 1,000
iterations on average. With CRN, the variance of the paired difference
between the best move and the others is **2.7× lower** (645 vs 1,715
points², median position 2.9×). That is like running 2.7× as many
iterations.

Strength, 2,000 games (seed 2024, NASPA, 95% CIs). Both sides use learned
leaves and the endgame solver:

| A vs B | A win rate | A spread | Time per game |
|---|---|---|---|
| sim (10 × 80 × 2) vs static | 54.6% ± 2.2 | +11.3 ± 4.2 | 16 s* |
| learned static vs heuristic static (5,000 games, seed 7) | 50.9% ± 1.4 | +3.0 ± 2.7 | 43 ms |

\* Thread-seconds (wall time × threads / games). Under the outside load
the arena got only about 4 of its 8 cores, and the run took 67 minutes.
Its CPU time was an estimated 7–8 s per game.

Simulation is worth about 11 points a game, roughly three times the gain
from learned leaves over the heuristic. Next ideas:

- Infer the opponent's rack from their last play. A player who kept tiles
  probably did not keep a blank or an S they could have played.
- A proper pre-endgame: once the bag is nearly empty, solve the empty-bag
  endgames inside the rollouts exactly.
- Make rollouts faster with a move generator that returns only the best
  static move, pruning on an equity bound, instead of every move.
- Choose by win probability instead of spread late in the game.

## Roadmap

| # | Milestone | Status |
|---|-----------|--------|
| 0 | Build system, tests, board/tile basics | ✅ |
| 1 | Bag, rack, move representation | ✅ |
| 2 | Lexicon + GADDAG | ✅ |
| 3 | Scoring + cross-checks | ✅ |
| 4 | Move generation | ✅ |
| 5 | Self-play arena | ✅ |
| 6 | Learned rack-leave values | ✅ |
| 7 | Monte Carlo simulation | ✅ |
| 8 | Endgame solver | ✅ |
| 9 | Web UI + server | |
| 10 | Benchmarks against external engines | |
