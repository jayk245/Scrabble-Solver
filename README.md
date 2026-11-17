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

For analysing real games, use a tournament-style list. WGPO Official Words
2024 (WOW24, 195,383 words) is free for non-commercial use, so it is
downloaded rather than committed:

```sh
scripts/fetch_lexicon.sh wow24
./build/release/apps/server/scrabble_server --lexicon data/lexica/wow24.txt
```

The page header shows which lexicon the server loaded. WOW24 is close to,
but not the same as, NASPA's NWL2023; analyse each game with the lexicon it
was played under (see `data/lexica/README.md`). Every app takes
`--lexicon PATH` with any one-word-per-line file. The bot's learned leave
values were trained with ENABLE and are used unchanged with other lists.

## Web UI

Three tabs in the browser: a **Solver**, an analysis board that tells you
the best move in any position of a game you enter or import, with
variations and a whole-game review, **Play** against
the bot, and a **Game viewer** for self-play and GCG replays (each
described below). The C++ server (`apps/server`, cpp-httplib +
nlohmann/json) loads the GADDAG once, answers JSON requests and streams
games; the frontend (`web/`) is React + TypeScript built with Vite.
Requires Node.js 20.19+ or 22.12+.

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

### Solver

An analysis board, like a chess engine's: enter or import a game, step
through it, branch into variations and ask the engine for the best move in
any position.

**Entering a game.** The toolbar above the board switches between two
modes:

* **Enter moves** (default): tiles placed on the board form a move for the
  player to move, exactly as in the Play tab (click a square and type, or
  click a square then a rack tile; Shift+letter for a blank; Space turns;
  Backspace takes a tile back; Esc recalls). The engine checks the move live
  (`/api/validate`) and shows its score; **Record move** (Enter) adds it,
  credits the score and passes the turn. **Pass** and **Exchange** are
  there too. The mover's rack is optional: a full rack (7 tiles, or every
  tile left once the bag is empty) must supply the move; a partial one
  (e.g. last turn's leave) is topped up with the tiles the move needed; an
  empty one accepts any tiles. Racks are stored per position, so each
  move remembers the rack it was played from, and the next turn starts
  from the player's leave.
* **Edit board**: set up any position (board, scores, who moves); tiles
  typed here are not a move. Editing a position discards the moves after it
  (you are asked first).

Typed notation still works: **Check a move**, any candidate's **Play**, and
**Import → Moves** (a pasted list, played from the current position).

**Moving around.** Click any move in the move list, or use ◀ ▶ (Alt+←/→)
and ⏮ ⏭ (Home/End). Recording a different move from an earlier position
does not overwrite anything: it starts a **variation**, shown indented
under the move it replaces (nested variations in brackets). **Promote to
main line** makes the current line the main one; **Delete variation** /
**Delete from here** (Ctrl+Z) removes the current move and what follows.
**Show racks** hides every rack (board, move list, inputs) for presenting.

**Engine.** **Solve** asks the engine (`POST /api/analyze`) about the
current position with the mover's rack: the best move, the ranked
candidates with static equity, and

* with tiles in the bag, a Monte Carlo simulation of the top candidates
  (mean ± standard error, iterations, pruning) as a list and a bar chart. A
  known opponent rack ("I know the opponent's rack") is held fixed in every
  rollout; otherwise it is sampled from the unseen tiles;
* with the bag empty, the exact endgame solver: the final spread under best
  play, whether the search completed, and the best line.

Effort presets are Static, Quick (~1 s), Normal (≤ 2.5 s) and Deep
(≤ 4.5 s). **Engine on** solves every position with a full rack as soon as
you stop on it. Answers are cached per position (keyed by board, racks,
scores and effort, so a revisited or transposed position is instant and an
edited one is never shown a stale answer). Leaving a position aborts its
request, and the server drops a search that is still queued for a client
that has gone away, so stepping quickly through a game never builds a
backlog. When the position has a recorded continuation, that move is
evaluated too (badged "played"). Hover a candidate to preview it.

**Game review.** Analyses every main-line position whose mover's rack is
known (choose the effort; progress is shown and Cancel stops it), then
grades each move by the points it gave up against the engine's move:

| Grade | Points lost |
|---|---|
| Best | < 1 |
| Good | 1 – 4 |
| Inaccuracy (?!) | 4 – 10 |
| Mistake (?) | 10 – 20 |
| Blunder (??) | ≥ 20 |

Points are the difference in simulated mean spread when the review
simulates (the played move is always simulated alongside the engine's
candidates, via `"played"` in `/api/analyze`), the exact endgame value
once the bag is empty (the solver's value minus the value of the position
after the move), and static equity otherwise. The move list shows the
symbols, the panel summarises each player (average loss and counts), and a
chart plots the loss of every move, one player above the axis and the other
below; click a bar to go to that move.

**Import and export.** **Import → GCG** (paste or open a file) replaces the
game with the record as the main line, with every rack it records
(`POST /api/gcg` replays it with the same parser as the game viewer).
**Open in Solver** in the Game viewer and on the Play tab's end screen
loads that game with both players' racks. **Export GCG** downloads the
main line. End-of-game rack adjustments are not applied in the solver.

```sh
curl -s localhost:8080/api/analyze -d '{"rack": "EIQRSUZ", "effort": "quick",
  "played": "D5 QUI(R)E",
  "board": ["...............", "...............", "...............",
            "...............", "...............", "...............",
            "...............", "...RETAINS.....", "...............",
            "...............", "...............", "...............",
            "...............", "...............", "..............."]}'
curl -s localhost:8080/api/gcg -d "{\"gcg\": $(jq -Rs . < game.gcg)}"
```

### Play against the bot

Pick a strength, who moves first, and optionally a seed and NASPA/WESPA
rules:

| Strength | Bot |
|---|---|
| Easy | the highest-scoring move (no leave values) |
| Normal | static equity with the learned leaves |
| Hard | simulation, 10 candidates, ≤ 3 s a move |
| Expert | larger simulation plus the exact endgame solver, ≤ 4 s |

Place tiles by clicking a square and typing (Shift+letter or an
exhausted letter uses a blank), or by clicking a square and then a rack
tile; a blank asks for its letter. While you build a word the engine
checks it live (`/api/validate`) and shows its score. Submit (Enter),
Pass, Exchange (pick tiles), Shuffle, Recall (Esc), Hint (the top five
static moves, with Use to lay one down) and Resign. The scoreboard, bag
count, unseen tiles from your side and the move log update as the bot
replies; clicking a logged move shows the board at that point.

When the game ends you see the result, the bot's rack, a GCG download and
a review: for each of your moves the engine's best static move and the
equity you gave up. Hover a review row to see that choice on the board. **Open in Solver** loads the finished game, with the bot's racks, into the
Solver for a full review.

How it works: the server is the referee. It holds the game (`Game`,
board, both racks and the bag) in a `HumanGame` (`apps/server/human_game.h`),
validates every move with the engine, and streams events over the same
SSE endpoint as the viewer. An illegal word or placement is refused with
422 and the reason; there is no challenge rule, so a phony never reaches
the board. After your move the request returns at once and the bot thinks
on its own thread (at most two bots think at once); its move arrives as an
event. Events never contain the bot's rack, leave, draws or analysis, its
exchanges show only the tile count, and a random seed (which fixes the bag
order) is revealed only at the end. A game nobody touches or watches for
30 minutes is abandoned. `#play/<id>` reopens a game in progress.

```sh
curl -s localhost:8080/api/games -d '{"mode": "human", "strength": "hard"}'
curl -s localhost:8080/api/games/<id>/hint
curl -s localhost:8080/api/games/<id>/moves -d '{"move": "8D RETAINS"}'
curl -N localhost:8080/api/games/<id>/events
```

### Game viewer

The **Game viewer** tab watches the bot play itself, or replays a GCG
record, move by move: both racks, scores and bag count, the last move
highlighted, a clickable move log, and playback controls (play/pause, step,
scrub, speed; ←/→, Space, Home/End). For each move the analysis panel shows
how it was chosen: the top static candidates, or for a simulated move each
candidate's mean rollout value ± standard error (as a bar chart), or the
endgame solver's value and best line. Hovering a candidate previews it on
the board. Finished games download as GCG, and `#viewer/<id>` links reopen
a game while the server keeps it (15 minutes after it ends).

Each player is configured separately: leave model (learned, heuristic or
none), Monte Carlo simulation (candidates ≤ 20, iterations ≤ 1,000, plies
≤ 4) and the endgame solver, plus the seed and NASPA/WESPA end rules.

How it works:

* `POST /api/games` starts a self-play game on a server worker thread (at
  most two at once, two simulation threads each), or parses, validates and
  annotates a GCG record up front (`ParseGcg()` in `engine/include/scrabble/gcg.h`).
* `GET /api/games/{id}/events` streams the game as Server-Sent Events:
  `start`, one `move` per turn, then `end` with the final scores and the
  GCG. Every event is kept in an in-memory log, so a reconnecting client
  resumes from `Last-Event-ID` and a finished game can be watched again.
* The browser buffers every event and plays them back itself, so pausing,
  stepping and changing speed are instant; if playback catches up with a
  game still being computed it waits for the next move.
* `DELETE /api/games/{id}` stops a game; one nobody is watching is stopped
  after two minutes.

```sh
curl -s localhost:8080/api/games -d '{"seed": 42, "players": [
  {"name": "sim", "simulation": {"enabled": true}, "endgame": {"enabled": true}},
  {"name": "static"}]}'                         # -> {"id": "...", ...}
curl -N localhost:8080/api/games/<id>/events    # watch the stream
```

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
| 9 | Web UI + server | ✅ |
| 10 | Benchmarks against external engines | |
