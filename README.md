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

## Roadmap

| # | Milestone | Status |
|---|-----------|--------|
| 0 | Build system, tests, board/tile basics | ✅ |
| 1 | Bag, rack, move representation | ✅ |
| 2 | Lexicon + GADDAG | ✅ |
| 3 | Scoring + cross-checks | ✅ |
| 4 | Move generation | ✅ |
| 5 | Self-play arena | ✅ |
| 6 | Learned rack-leave values | |
| 7 | Monte Carlo simulation | |
| 8 | Endgame solver | |
| 9 | Web UI + server | |
| 10 | Benchmarks against external engines | |
