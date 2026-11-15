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

## Roadmap

| # | Milestone | Status |
|---|-----------|--------|
| 0 | Build system, tests, board/tile basics | ✅ |
| 1 | Bag, rack, move representation | |
| 2 | Lexicon + GADDAG | |
| 3 | Scoring + cross-checks | |
| 4 | Move generation | |
| 5 | Greedy bot + self-play arena | |
| 6 | Learned rack-leave values | |
| 7 | Monte Carlo simulation | |
| 8 | Endgame solver | |
| 9 | Web UI + server | |
| 10 | Benchmarks against external engines | |
