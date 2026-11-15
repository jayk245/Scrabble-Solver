"""Human-readable summaries of leave tables and arena comparisons."""

from __future__ import annotations

import argparse
from collections.abc import Sequence
from pathlib import Path

from leavefit import read_table
from selfplay import ArenaSummary


def sanity_tables(table: dict[str, float], sizes: Sequence[int] = (1, 2, 3),
                  count: int = 20) -> str:
    """The best and worst leaves of each size, side by side, as Markdown.

    Single tiles are listed in full (there are only 27).
    """
    lines: list[str] = []
    for size in sizes:
        ranked = sorted(((v, k) for k, v in table.items() if len(k) == size),
                        reverse=True)
        if not ranked:
            continue
        if size == 1:
            lines.append("Single tiles, best to worst:\n")
            lines.append(" ".join(f"{k} {v:+.1f}" for v, k in ranked))
            lines.append("")
            continue
        n = min(count, len(ranked) // 2)
        lines.append(f"{size}-tile leaves: top {n} and bottom {n}\n")
        lines.append("| # | best | value | worst | value |")
        lines.append("|--:|------|------:|-------|------:|")
        for i in range(n):
            best_v, best = ranked[i]
            worst_v, worst = ranked[-1 - i]
            lines.append(f"| {i + 1} | {best} | {best_v:+.1f} | {worst} | "
                         f"{worst_v:+.1f} |")
        lines.append("")
    return "\n".join(lines)


def results_row(label: str, summary: ArenaSummary) -> str:
    """One Markdown row: label, games, win rate and spread with 95% CIs."""
    return (f"| {label} | {summary.games:,} | "
            f"{summary.win_rate:.1f}% ± {summary.win_rate_ci:.1f} | "
            f"{summary.spread:+.1f} ± {summary.spread_ci:.1f} |")


RESULTS_HEADER = ("| A vs B | games | A win rate | A spread |\n"
                  "|--------|------:|-----------:|---------:|")


def main(argv: Sequence[str] | None = None) -> None:
    parser = argparse.ArgumentParser(
        description="Print the best and worst leaves of a leave table.")
    parser.add_argument("table", type=Path, help="leave CSV")
    parser.add_argument("--count", type=int, default=20,
                        help="leaves to show at each end (default 20)")
    args = parser.parse_args(argv)
    print(sanity_tables(read_table(args.table), count=args.count))


if __name__ == "__main__":
    main()
