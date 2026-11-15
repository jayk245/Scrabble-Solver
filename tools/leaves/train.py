#!/usr/bin/env python3
"""Trains rack-leave values by iterated self-play.

Generation 0 is the engine's hand-written heuristic. Each generation plays
``--games`` self-play games with the previous generation's leave values,
fits a new table to the moves of those games (see ``leavefit.py``), and
measures the new table against its predecessor in the arena. The final
table is then compared with the heuristic and with zero leaves, and its
best and worst leaves are printed.

Outputs:

* ``data/leaves/enable_leaves.csv``: every leave of 1-6 tiles (~11 MB;
  gitignored).
* ``data/leaves/enable_leaves_default.csv``: leaves of up to 4 tiles plus
  the fallback model's coefficients (~0.3 MB; committed).
* ``data/leaves/work/``: per-generation observations, tables and
  ``report.md`` (gitignored). Observations are reused on a rerun with the
  same games and seed, so a refit skips self-play.

Typical use, from the repository root after a release build::

    python3 tools/leaves/train.py          # 2 x 200k games, ~10 min
    python3 tools/leaves/train.py --games 5000 --generations 1  # smoke
"""

from __future__ import annotations

import argparse
import dataclasses
import os
import resource
import shutil
import sys
import time
from collections.abc import Sequence
from pathlib import Path

import numpy as np

from leavefit import (FitConfig, LeaveFit, Observations, fit_leaves,
                      read_table, write_table)
from report import RESULTS_HEADER, results_row, sanity_tables
from selfplay import Arena, ArenaSummary

REPO = Path(__file__).resolve().parents[2]


@dataclasses.dataclass
class Generation:
    """One round of self-play and refitting."""

    number: int
    table: Path
    observations: int
    selfplay_seconds: float
    fit_seconds: float
    fit: LeaveFit
    versus_previous: ArenaSummary


def log(message: str) -> None:
    print(message, flush=True)


def previous_label(number: int) -> str:
    return "heuristic" if number == 1 else f"gen {number - 1}"


def peak_rss_mb(who: int) -> float:
    """Peak resident set size of this process or its children, in MB."""
    peak = resource.getrusage(who).ru_maxrss
    # ru_maxrss is in bytes on macOS and in kilobytes on Linux.
    return peak / 1e6 if sys.platform == "darwin" else peak / 1e3


def parse_args(argv: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--games", type=int, default=200_000,
                        help="self-play games per generation")
    parser.add_argument("--generations", type=int, default=2)
    parser.add_argument("--eval-games", type=int, default=10_000,
                        help="arena games per comparison")
    parser.add_argument("--threads", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--seed", type=int, default=1,
                        help="base seed; generation g self-plays with "
                             "seed*1000+g, evaluations use seed*1000+999")
    parser.add_argument("--min-bag", type=int, default=FitConfig.min_bag,
                        help="ignore moves made with fewer tiles in the bag")
    parser.add_argument("--anchor", choices=("empty", "mean"),
                        default="empty",
                        help="what is worth 0: the empty leave, or the "
                             "average observed leave")
    parser.add_argument("--default-max-size", type=int, default=4,
                        help="longest leave listed in the committed default")
    parser.add_argument("--arena", type=Path,
                        default=REPO / "build/release/apps/scrabble_arena")
    parser.add_argument("--lexicon", type=Path, default=None)
    parser.add_argument("--work-dir", type=Path,
                        default=REPO / "data/leaves/work")
    parser.add_argument("--out", type=Path,
                        default=REPO / "data/leaves/enable_leaves.csv")
    parser.add_argument("--default-out", type=Path,
                        default=REPO / "data/leaves/enable_leaves_default.csv")
    return parser.parse_args(argv)


def self_play(arena: Arena, args: argparse.Namespace, number: int,
              policy: str) -> tuple[Observations, float]:
    """Self-play data for generation ``number``, cached in the work dir."""
    seed = args.seed * 1000 + number
    cache = args.work_dir / f"gen{number}_games{args.games}_seed{seed}.npz"
    if cache.exists():
        log(f"  reusing {cache.name}")
        return Observations.load(cache), 0.0
    start = time.monotonic()
    observations, summary = arena.self_play(args.games, seed, policy)
    seconds = time.monotonic() - start
    log(f"  {args.games:,} games in {seconds:.0f} s "
        f"({args.games / seconds:.0f} games/s), {len(observations):,} moves")
    log("  " + summary.text.strip().replace("\n", "\n  "))
    observations.save(cache)
    return observations, seconds


def header_lines(args: argparse.Namespace, number: int,
                 fit: LeaveFit) -> list[str]:
    return [
        f"Learned leave values, generation {number} "
        f"(tools/leaves/train.py).",
        f"{args.games:,} self-play games per generation, seed {args.seed}, "
        f"moves with >= {args.min_bag} tiles in the bag "
        f"({fit.observations:,} used); anchor: {args.anchor} leave = 0.",
        "Value = mover's future net points relative to that anchor, shrunk",
        "toward a linear prior: (W*mean + k*prior) / (W + k).",
    ]


def summarise_fit(fit: LeaveFit) -> None:
    seen = int((fit.weight > 0).sum())
    log(f"  {fit.observations:,} observations, {seen:,} of "
        f"{fit.weight.size:,} leaves seen; shift {fit.shift:+.2f}")
    for length in range(1, 7):
        if np.isfinite(fit.tau2[length]):
            log(f"    {length}-tile: noise sd "
                f"{np.sqrt(fit.noise_var[length]):.1f}, prior sd "
                f"{np.sqrt(fit.tau2[length]):.2f}, "
                + (f"k = {fit.shrinkage_k[length]:,.0f}"
                   if fit.shrinkage_k[length] < 1e8 else
                   "k = inf (no spread beyond the prior detected: "
                   "prior only)"))


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if not args.arena.exists():
        print(f"error: {args.arena} not found; build the release preset "
              "first", file=sys.stderr)
        return 1
    args.work_dir.mkdir(parents=True, exist_ok=True)
    arena = Arena(args.arena, args.threads, args.lexicon)
    config = FitConfig(min_bag=args.min_bag,
                       anchor_empty=args.anchor == "empty")
    eval_seed = args.seed * 1000 + 999
    total_start = time.monotonic()

    policy = "heuristic"
    generations: list[Generation] = []
    for number in range(1, args.generations + 1):
        log(f"Generation {number}: self-play with {policy}")
        observations, selfplay_seconds = self_play(arena, args, number,
                                                   policy)
        start = time.monotonic()
        fit = fit_leaves(observations, config)
        fit_seconds = time.monotonic() - start
        summarise_fit(fit)
        table = args.work_dir / f"gen{number}.csv"
        write_table(fit, table, header=header_lines(args, number, fit))
        log(f"  fitted in {fit_seconds:.0f} s -> {table}")
        versus = arena.play(args.eval_games, eval_seed, str(table), policy)
        log("  " + results_row(f"gen {number} vs {previous_label(number)}",
                               versus))
        generations.append(Generation(number, table, len(observations),
                                      selfplay_seconds, fit_seconds, fit,
                                      versus))
        policy = str(table)

    final = generations[-1]
    shutil.copyfile(final.table, args.out)
    rows = write_table(final.fit, args.default_out,
                       max_size=args.default_max_size,
                       header=header_lines(args, final.number, final.fit)
                       + [f"Leaves of up to {args.default_max_size} tiles; "
                          "longer ones use the @ model."])
    log(f"Wrote {args.out} ({args.out.stat().st_size / 1e6:.1f} MB) and "
        f"{args.default_out} ({rows:,} leaves, "
        f"{args.default_out.stat().st_size / 1e6:.2f} MB)")

    comparisons = [
        ("final vs heuristic", str(args.out), "heuristic"),
        ("final vs zero", str(args.out), "zero"),
        ("heuristic vs zero", "heuristic", "zero"),
        ("default vs final", str(args.default_out), str(args.out)),
        ("default vs heuristic", str(args.default_out), "heuristic"),
    ]
    rows_md = [results_row(f"gen {g.number} vs {previous_label(g.number)}",
                           g.versus_previous) for g in generations]
    for label, a, b in comparisons:
        summary = arena.play(args.eval_games, eval_seed, a, b)
        log("  " + results_row(label, summary))
        rows_md.append(results_row(label, summary))

    report = "\n".join([
        "# Leave training report\n",
        f"{args.generations} generations x {args.games:,} self-play games; "
        f"evaluations {args.eval_games:,} games (seed {eval_seed}); total "
        f"{(time.monotonic() - total_start) / 60:.1f} min; peak RSS "
        f"{peak_rss_mb(resource.RUSAGE_SELF):.0f} MB (Python), "
        f"{peak_rss_mb(resource.RUSAGE_CHILDREN):.0f} MB (arena).\n",
        "| gen | observations | self-play s | fit s |",
        "|----:|-------------:|------------:|------:|",
        *[f"| {g.number} | {g.observations:,} | {g.selfplay_seconds:.0f} | "
          f"{g.fit_seconds:.0f} |" for g in generations],
        "",
        RESULTS_HEADER,
        *rows_md,
        "",
        sanity_tables(read_table(args.out)),
    ])
    (args.work_dir / "report.md").write_text(report + "\n", encoding="utf-8")
    log("\n" + report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
