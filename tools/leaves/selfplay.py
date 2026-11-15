"""Running the arena and collecting its training log.

The arena's JSONL log is about 185 bytes per move, or roughly 4.3 GB per
million games, so it is never written to disk. Instead the arena writes
into a named pipe and ``collect`` parses it as it arrives, keeping only
three small numbers per move (leave id, bag size, target): about 7 bytes,
or ~160 MB in memory per million games. Python's ``json`` module parses
some 500k lines/s, roughly twenty times faster than eight arena threads
produce them, so the pipe never slows self-play down.
"""

from __future__ import annotations

import array
import contextlib
import dataclasses
import json
import os
import re
import subprocess
import tempfile
import threading
from collections.abc import Iterable, Iterator
from pathlib import Path
import numpy as np

from leavefit import RACK_SIZE, Observations


def collect(lines: Iterable[str]) -> Observations:
    """Builds observations from arena JSONL log lines.

    Moves made with an empty bag are skipped (leave values are not used
    then), as are passes, whose "leave" is the whole rack.
    """
    index: dict[str, int] = {}
    vocab: list[str] = []
    leave_id = array.array("i")
    bag = array.array("B")
    target = array.array("i")
    for line in lines:
        record = json.loads(line)
        if record["bag"] == 0 or len(record["leave"]) >= RACK_SIZE:
            continue
        leave = record["leave"]
        i = index.get(leave)
        if i is None:
            i = index[leave] = len(vocab)
            vocab.append(leave)
        leave_id.append(i)
        bag.append(record["bag"])
        target.append(record["final_spread"] - record["spread"]
                      - record["score"])
    return Observations(vocab, np.frombuffer(leave_id, dtype=np.int32),
                        np.frombuffer(bag, dtype=np.uint8),
                        np.frombuffer(target, dtype=np.int32))


@dataclasses.dataclass(frozen=True)
class ArenaSummary:
    """The headline numbers the arena prints, from A's point of view."""

    games: int
    win_rate: float      # percent
    win_rate_ci: float   # 95% half-width, percent
    spread: float
    spread_ci: float
    seconds: float
    text: str            # the arena's full report

    @staticmethod
    def parse(text: str) -> ArenaSummary:
        def grab(pattern: str) -> list[float]:
            match = re.search(pattern, text)
            if match is None:
                raise ValueError(f"unexpected arena output:\n{text}")
            return [float(g) for g in match.groups()]

        games, = grab(r"Games\s+(\d+)")
        win, win_ci = grab(r"win rate ([\d.]+)% \+/- ([\d.]+)%")
        spread, spread_ci = grab(r"Spread \(A-B\)\s+([+-]?[\d.]+) \+/- "
                                 r"([\d.]+)")
        seconds, = grab(r"Time\s+([\d.]+) s")
        return ArenaSummary(int(games), win, win_ci, spread, spread_ci,
                            seconds, text)


@dataclasses.dataclass(frozen=True)
class Arena:
    """How to invoke ``scrabble_arena``."""

    binary: Path
    threads: int
    lexicon: Path | None = None

    def command(self, games: int, seed: int, leaves_a: str, leaves_b: str,
                log: Path | None = None) -> list[str]:
        command = [str(self.binary), "--games", str(games), "--threads",
                   str(self.threads), "--seed", str(seed), "--leaves-a",
                   leaves_a, "--leaves-b", leaves_b]
        if self.lexicon is not None:
            command += ["--lexicon", str(self.lexicon)]
        if log is not None:
            command += ["--log", str(log)]
        return command

    def play(self, games: int, seed: int, leaves_a: str,
             leaves_b: str) -> ArenaSummary:
        """Plays A against B and returns the arena's summary."""
        result = subprocess.run(
            self.command(games, seed, leaves_a, leaves_b), check=True,
            capture_output=True, text=True)
        return ArenaSummary.parse(result.stdout)

    def self_play(self, games: int, seed: int,
                  leaves: str) -> tuple[Observations, ArenaSummary]:
        """Plays ``leaves`` against itself and collects the move log."""
        with _fifo() as pipe:
            process = subprocess.Popen(
                self.command(games, seed, leaves, leaves, log=pipe),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            # Drain stdout/stderr on a thread so a chatty arena can never
            # block on a full pipe while we are busy reading the log.
            output: dict[str, str] = {}

            def drain() -> None:
                output["out"], output["err"] = process.communicate()
                if process.returncode != 0:
                    _release_reader(pipe)

            drainer = threading.Thread(target=drain)
            drainer.start()
            # Opening the read end blocks until the arena opens the write
            # end, and reading ends when the arena closes it.
            with open(pipe, encoding="ascii") as log:
                observations = collect(log)
            drainer.join()
        if process.returncode != 0:
            raise RuntimeError(f"arena failed ({process.returncode}): "
                               f"{output.get('err', '')}")
        return observations, ArenaSummary.parse(output["out"])


@contextlib.contextmanager
def _fifo() -> Iterator[Path]:
    """A named pipe in a fresh temporary directory, removed afterwards."""
    with tempfile.TemporaryDirectory(prefix="leaves-") as directory:
        path = Path(directory) / "log.jsonl"
        os.mkfifo(path)
        yield path


def _release_reader(pipe: Path) -> None:
    """Unblocks a reader stuck opening ``pipe`` after the writer died.

    If the arena exits before opening its log (say, a bad leave table),
    our blocking open of the read end would wait forever. Opening and
    closing the write end lets it complete and immediately see EOF. With
    O_NONBLOCK this fails harmlessly (ENXIO) when no reader is waiting.
    """
    with contextlib.suppress(OSError):
        os.close(os.open(pipe, os.O_WRONLY | os.O_NONBLOCK))
