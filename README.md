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
    --leaves-a heuristic --leaves-b zero
```

Plays bot configuration A against B (alternating who moves first) and
reports A's win/loss/tie record, mean scores and spread with 95% confidence
intervals, bingos per game and games/sec. Results depend only on `--seed`,
never on `--threads`. Leaves are `heuristic` (default), `zero` or a
`leave,value` CSV file. `--rules naspa|wespa` selects end-of-game scoring,
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

Use either with `--leaves-a FILE`. Python tests:
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
| 7 | Monte Carlo simulation | |
| 8 | Endgame solver | ✅ |
| 9 | Web UI + server | |
| 10 | Benchmarks against external engines | |
