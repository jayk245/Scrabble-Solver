"""Fitting rack-leave values from self-play observations.

A *leave* is the multiset of tiles a player keeps after a move. Each
observation pairs a leave with the points the mover went on to net over the
rest of the game (``final_spread - spread - score`` in the arena log). A
leave's value is how much that future spread exceeds the average, so the
model is a set of conditional means, estimated in three steps:

1. **Centre and weight.** Targets are centred per bag size (so game phase
   does not leak into leave values) and weighted by the inverse of the
   per-bag-size variance (early-game targets cover more future moves and
   are noisier, so they count for less).
2. **Prior.** A weighted least-squares linear model over simple leave
   features, which gives every leave a smooth baseline value, including
   leaves never observed.
3. **Shrinkage.** Each observed leave's mean is pulled toward its prior in
   proportion to how noisy it is: ``(W * mean + k * prior) / (W + k)``,
   where ``W`` is its total weight and ``k`` is the noise-to-signal ratio,
   estimated from the data (empirical Bayes).

Leaves are handled as count vectors over the 27-tile alphabet (A-Z, then
the blank ``?``), which is the same canonical order as the C++ ``Rack``.
"""

from __future__ import annotations

import dataclasses
import io
from collections.abc import Iterable, Sequence
from pathlib import Path

import numpy as np
import numpy.typing as npt

ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ?"
BLANK = 26
NUM_TILES = len(ALPHABET)
RACK_SIZE = 7
MAX_LEAVE = RACK_SIZE - 1

# Copies of each tile in a standard English bag (A-Z, then the blank).
TILE_COUNTS = (9, 2, 2, 4, 12, 2, 3, 2, 9, 1, 1, 4, 2, 6, 8, 2, 1, 6, 4, 6,
               4, 2, 2, 1, 2, 1, 2)

# Y counts as a consonant and the blank as neither, matching the C++
# fallback model in leave_evaluator.cc.
VOWELS = frozenset("AEIOU")
_VOWEL_MASK = np.array([c in VOWELS for c in ALPHABET])
_CONSONANT_MASK = np.array([c not in VOWELS and c != "?" for c in ALPHABET])
_Q = ALPHABET.index("Q")
_U = ALPHABET.index("U")

_CHAR_INDEX = {c: i for i, c in enumerate(ALPHABET)}
_CHAR_INDEX.update({c.lower(): i for i, c in enumerate(ALPHABET[:26])})

Counts = npt.NDArray[np.uint8]
Floats = npt.NDArray[np.float64]


# ---------------------------------------------------------------------------
# Leave encoding


def parse_leave(text: str) -> npt.NDArray[np.uint8]:
    """Returns the 27-entry count vector of a leave such as ``"ERS?"``."""
    counts = np.zeros(NUM_TILES, dtype=np.uint8)
    for char in text:
        try:
            counts[_CHAR_INDEX[char]] += 1
        except KeyError:
            raise ValueError(f"bad tile {char!r} in leave {text!r}") from None
    return counts


def leave_string(counts: Sequence[int]) -> str:
    """Canonical form of a count vector: A-Z in order, blanks last."""
    return "".join(ALPHABET[i] * int(n) for i, n in enumerate(counts) if n)


def counts_matrix(leaves: Iterable[str]) -> Counts:
    """Stacks ``parse_leave`` over many leaves into an (n, 27) matrix."""
    rows = [parse_leave(leave) for leave in leaves]
    if not rows:
        return np.zeros((0, NUM_TILES), dtype=np.uint8)
    return np.stack(rows)


def enumerate_leaves(max_size: int = MAX_LEAVE) -> Counts:
    """Every leave of 1..max_size tiles that a standard bag allows.

    Built letter by letter: each partial leave is repeated once per number
    of copies of the next letter that still fits, so the whole set is made
    with a handful of vectorised operations. Rows are ordered by size, then
    canonically.
    """
    partial = np.zeros((1, NUM_TILES), dtype=np.uint8)
    sizes = np.zeros(1, dtype=np.int64)
    for tile in range(NUM_TILES):
        limit = min(TILE_COUNTS[tile], max_size)
        blocks_counts = []
        blocks_sizes = []
        for copies in range(limit + 1):
            keep = sizes + copies <= max_size
            block = partial[keep].copy()
            block[:, tile] = copies
            blocks_counts.append(block)
            blocks_sizes.append(sizes[keep] + copies)
        partial = np.concatenate(blocks_counts)
        sizes = np.concatenate(blocks_sizes)
    keep = sizes > 0
    partial, sizes = partial[keep], sizes[keep]
    # '~' sorts after 'Z', so blanks sort last as in canonical form.
    strings = np.array([leave_string(row).replace("?", "~")
                        for row in partial])
    order = np.lexsort((strings, sizes))
    return partial[order]


# ---------------------------------------------------------------------------
# Prior model


@dataclasses.dataclass
class LeaveModel:
    """The prior: a value for any leave of 1..6 tiles from a few lookups.

    This is exactly the fallback model ``LeaveEvaluator`` evaluates in C++
    (and the form the ``@`` rows of a leave CSV take): a leave's value is
    the sum of

    * ``tile[t][k]`` for each tile ``t`` it holds exactly ``k`` copies of,
    * ``length[n]`` for its size ``n``,
    * ``balance[v][c]`` for its ``v`` vowels and ``c`` consonants (blanks
      count as neither; blank-only leaves have no balance term),
    * ``q_without_u`` if it holds a Q but no U.
    """

    tile: Floats         # (27, 7); [t][0] is 0
    length: Floats       # (7,); [0] is 0
    balance: Floats      # (7, 7); 0 where v + c is 0 or more than 6
    q_without_u: float

    def predict(self, counts: Counts, chunk: int = 100_000) -> Floats:
        """Values of the leaves in ``counts`` (rows of 1..6 tiles)."""
        out = np.empty(counts.shape[0])
        for begin in range(0, counts.shape[0], chunk):
            block = counts[begin:begin + chunk].astype(np.int64)
            tiles = np.arange(NUM_TILES)
            value = self.tile[tiles, block].sum(axis=1)
            value += self.length[block.sum(axis=1)]
            vowels = (block * _VOWEL_MASK).sum(axis=1)
            consonants = (block * _CONSONANT_MASK).sum(axis=1)
            value += self.balance[vowels, consonants]
            value += self.q_without_u * ((block[:, _Q] > 0)
                                         & (block[:, _U] == 0))
            out[begin:begin + chunk] = value
        return out

    def shifted(self, shift: float) -> LeaveModel:
        """The model with ``shift`` subtracted from every leave's value.

        Every leave has exactly one length term, so that is where the
        shift goes.
        """
        length = self.length.copy()
        length[1:] -= shift
        return dataclasses.replace(self, length=length)

    def rows(self) -> list[tuple[str, float]]:
        """The model as ``(name, value)`` rows for the leave CSV."""
        rows = []
        for t in range(NUM_TILES):
            for k in range(1, min(TILE_COUNTS[t], MAX_LEAVE) + 1):
                rows.append((f"@{ALPHABET[t]}{k}", float(self.tile[t, k])))
        for n in range(1, MAX_LEAVE + 1):
            rows.append((f"@len{n}", float(self.length[n])))
        for v in range(RACK_SIZE):
            for c in range(max(1 - v, 0), RACK_SIZE - v):
                rows.append((f"@v{v}c{c}", float(self.balance[v, c])))
        rows.append(("@qnou", self.q_without_u))
        return rows


@dataclasses.dataclass(frozen=True)
class _Design:
    """Index layout of the regression features behind ``LeaveModel``.

    The regression is parameterised so that ridge shrinkage (toward 0)
    pulls rarely seen terms toward sensible defaults rather than toward
    "worth nothing":

    * ``at_least[t][k]``: holds at least ``k`` copies of tile ``t``. For
      ``k`` = 1 this is the tile's own value; for ``k`` > 1 it is how much
      the k-th copy differs from the generic duplicate penalty below.
    * ``extra_copies``: the number of non-blank tiles beyond the first of
      each letter (numeric). It is shared by every letter, so it is
      estimated from the many common pairs and then applies to rare
      triples and quads too. Without it a never-seen "LLLL" would be
      valued like "L" plus noise.
    * ``length[n]``: per-length intercept.
    * ``imbalance``: ``|vowels - consonants|`` (numeric), shared across
      lengths; ``cell[v][c]`` is each (vowels, consonants) cell's
      deviation from it.
    * ``q_without_u``.

    ``to_model`` folds these into ``LeaveModel``'s exact-count terms.
    """

    at_least: npt.NDArray[np.int64]  # (27, 7), -1 where impossible
    extra_copies: int
    length: npt.NDArray[np.int64]    # (7,), -1 at 0
    imbalance: int
    cell: npt.NDArray[np.int64]      # (7, 7), -1 unless 1 <= v + c <= 6
    q_without_u: int
    size: int
    # Active features per leave: up to six tile terms, then extra copies,
    # length, imbalance, cell and Q-without-U.
    slots: int = MAX_LEAVE + 5

    @staticmethod
    def build() -> _Design:
        index = 0
        at_least = np.full((NUM_TILES, RACK_SIZE), -1, dtype=np.int64)
        for t in range(NUM_TILES):
            for k in range(1, min(TILE_COUNTS[t], MAX_LEAVE) + 1):
                at_least[t, k] = index
                index += 1
        extra_copies = index
        index += 1
        length = np.full(RACK_SIZE, -1, dtype=np.int64)
        length[1:] = np.arange(index, index + MAX_LEAVE)
        index += MAX_LEAVE
        imbalance = index
        index += 1
        cell = np.full((RACK_SIZE, RACK_SIZE), -1, dtype=np.int64)
        for v in range(RACK_SIZE):
            for c in range(max(1 - v, 0), RACK_SIZE - v):
                cell[v, c] = index
                index += 1
        return _Design(at_least, extra_copies, length, imbalance, cell,
                       index, index + 1)

    def features(self, counts: Counts) -> tuple[npt.NDArray[np.int64],
                                                Floats]:
        """Sparse design rows: (indices, values), each (n, slots).

        Unused slots have index -1 and value 0. Requires 1..6 tiles per
        leave.
        """
        n = counts.shape[0]
        c = counts.astype(np.int64)
        idx = np.full((n, self.slots), -1, dtype=np.int64)
        val = np.zeros((n, self.slots))
        # Tile terms: a leave of s tiles activates exactly s "at least"
        # indicators, so six slots suffice; fill them left to right.
        fill = np.zeros(n, dtype=np.int64)
        rows = np.arange(n)
        for t in range(NUM_TILES):
            for k in range(1, min(TILE_COUNTS[t], MAX_LEAVE) + 1):
                held = c[:, t] >= k
                idx[rows[held], fill[held]] = self.at_least[t, k]
                val[rows[held], fill[held]] = 1.0
                fill += held
        base = MAX_LEAVE
        extra = np.maximum(c[:, :BLANK] - 1, 0).sum(axis=1)
        idx[:, base] = self.extra_copies
        val[:, base] = extra
        idx[:, base + 1] = self.length[c.sum(axis=1)]
        val[:, base + 1] = 1.0
        vowels = (c * _VOWEL_MASK).sum(axis=1)
        consonants = (c * _CONSONANT_MASK).sum(axis=1)
        idx[:, base + 2] = self.imbalance
        val[:, base + 2] = np.abs(vowels - consonants)
        cell = self.cell[vowels, consonants]
        idx[:, base + 3] = cell
        val[:, base + 3] = cell >= 0
        q_no_u = (c[:, _Q] > 0) & (c[:, _U] == 0)
        idx[:, base + 4] = np.where(q_no_u, self.q_without_u, -1)
        val[:, base + 4] = q_no_u
        # Indices of unused slots must be valid for bincount; their value
        # is 0, so pointing them at feature 0 changes nothing.
        idx[val == 0] = 0
        return idx, val

    def to_model(self, coef: Floats) -> LeaveModel:
        tile = np.zeros((NUM_TILES, RACK_SIZE))
        for t in range(NUM_TILES):
            total = 0.0
            for k in range(1, min(TILE_COUNTS[t], MAX_LEAVE) + 1):
                total += coef[self.at_least[t, k]]
                extra = coef[self.extra_copies] * (k - 1) if t != BLANK else 0
                tile[t, k] = total + extra
        length = np.zeros(RACK_SIZE)
        length[1:] = coef[self.length[1:]]
        balance = np.zeros((RACK_SIZE, RACK_SIZE))
        for v in range(RACK_SIZE):
            for c in range(RACK_SIZE):
                if self.cell[v, c] >= 0:
                    balance[v, c] = (coef[self.cell[v, c]]
                                     + coef[self.imbalance] * abs(v - c))
        return LeaveModel(tile, length, balance,
                          float(coef[self.q_without_u]))


def fit_prior(counts: Counts, means: Floats, weights: Floats,
              ridge: float, chunk: int = 100_000) -> LeaveModel:
    """Weighted ridge least squares of per-leave ``means`` on the features.

    Every feature is a function of the leave alone, so fitting the
    per-leave means with weight equal to each leave's total observation
    weight gives the same coefficients as fitting every observation
    individually. The normal equations X'WX and X'Wy are accumulated in
    chunks from the sparse design, so memory stays small however many
    leaves are seen.

    ``ridge`` is a penalty in units of observations: a coefficient backed
    by fewer than about ``ridge`` observations is pulled substantially
    toward 0 (that is, toward the shared duplicate, imbalance and length
    terms), one backed by thousands is essentially unaffected. Some
    features overlap (per-length intercepts and the balance cells), and
    the ridge also makes the solution unique.
    """
    design = _Design.build()
    p = design.size
    gram = np.zeros(p * p)
    rhs = np.zeros(p)
    for begin in range(0, counts.shape[0], chunk):
        idx, val = design.features(counts[begin:begin + chunk])
        w = weights[begin:begin + chunk]
        y = means[begin:begin + chunk]
        for i in range(design.slots):
            rhs += np.bincount(idx[:, i], weights=w * y * val[:, i],
                               minlength=p)
            for j in range(design.slots):
                gram += np.bincount(idx[:, i] * p + idx[:, j],
                                    weights=w * val[:, i] * val[:, j],
                                    minlength=p * p)
    gram = gram.reshape(p, p) + ridge * np.eye(p)
    return design.to_model(np.linalg.solve(gram, rhs))


# ---------------------------------------------------------------------------
# Observations


@dataclasses.dataclass
class Observations:
    """Self-play moves, one entry per move, as parallel arrays.

    ``leave_id`` indexes ``vocab`` (canonical leave strings, "" allowed for
    the empty leave), ``bag`` is the bag size before the move and
    ``target`` the mover's net points over the rest of the game, excluding
    the move itself.
    """

    vocab: list[str]
    leave_id: npt.NDArray[np.int32]
    bag: npt.NDArray[np.uint8]
    target: npt.NDArray[np.int32]

    def __len__(self) -> int:
        return int(self.leave_id.size)

    def save(self, path: Path) -> None:
        np.savez_compressed(path, vocab=np.array(self.vocab, dtype=str),
                            leave_id=self.leave_id, bag=self.bag,
                            target=self.target)

    @staticmethod
    def load(path: Path) -> Observations:
        with np.load(path) as data:
            return Observations(list(data["vocab"]), data["leave_id"],
                                data["bag"], data["target"])

    @staticmethod
    def concatenate(parts: Sequence[Observations]) -> Observations:
        """Merges observation sets, re-indexing their vocabularies."""
        vocab: list[str] = []
        index: dict[str, int] = {}
        ids = []
        for part in parts:
            remap = np.empty(len(part.vocab), dtype=np.int32)
            for i, leave in enumerate(part.vocab):
                remap[i] = index.setdefault(leave, len(vocab))
                if remap[i] == len(vocab):
                    vocab.append(leave)
            ids.append(remap[part.leave_id])
        return Observations(vocab, np.concatenate(ids),
                            np.concatenate([p.bag for p in parts]),
                            np.concatenate([p.target for p in parts]))


# ---------------------------------------------------------------------------
# Fitting


@dataclasses.dataclass
class FitConfig:
    """Knobs of ``fit_leaves``; the defaults are the documented method."""

    # Moves made with fewer tiles than this in the bag are ignored: the
    # mover cannot fully restock, so the leave means something different.
    min_bag: int = 7
    # Prior standard deviation of a regression coefficient, in points. The
    # ridge penalty is noise variance / coef_sd**2: about 150 observations
    # for a noise sd of ~62 points. A rare feature (say, four Ls) then
    # needs real evidence before it strays far from the shared terms.
    coef_sd: float = 5.0
    # Shift so the empty leave (bingo, or exchanging all seven) is worth
    # exactly 0, as the engine assumes; otherwise the shift is the
    # observation-weighted mean, so the average move's leave is worth 0.
    anchor_empty: bool = True


@dataclasses.dataclass
class LeaveFit:
    """Result of ``fit_leaves``: a value for every leave of 1..6 tiles."""

    model: LeaveModel            # prior model, after the anchor shift
    leaves: Counts               # every possible leave, (n, 27)
    values: Floats               # final (shrunk) value per leave
    prior: Floats                # prior value per leave
    weight: Floats               # total observation weight per leave
    raw_mean: Floats             # observed mean (nan if unseen)
    shrinkage_k: Floats          # k per leave length (index = length)
    tau2: Floats                 # prior variance per leave length
    noise_var: Floats            # per-observation noise per leave length
    shift: float                 # subtracted from everything
    observations: int            # observations used

    def strings(self) -> list[str]:
        return [leave_string(row) for row in self.leaves]


def _random_effects_tau2(deviation: Floats, weight: Floats,
                         noise_var: float) -> float:
    """DerSimonian-Laird estimate of the spread of true values.

    ``deviation`` is each leave's observed mean minus its prior, with
    sampling variance ``noise_var / weight``. If leaves had no real
    differences beyond the prior, the precision-weighted sum of squared
    deviations ``Q`` would average ``K - 1``; the excess is attributed to
    genuine variation of true leave values around the prior.
    """
    precision = weight / noise_var
    q_stat = float(np.sum(precision * deviation ** 2))
    scale = precision.sum() - np.sum(precision ** 2) / precision.sum()
    tau2 = (q_stat - (deviation.size - 1)) / scale
    return max(tau2, 1e-6)


def fit_leaves(obs: Observations, config: FitConfig | None = None,
               all_leaves: Counts | None = None) -> LeaveFit:
    """Fits a value for every leave of 1..6 tiles; see the module docstring.

    ``all_leaves`` defaults to ``enumerate_leaves()``; tests pass a smaller
    universe.
    """
    config = config or FitConfig()
    keep = obs.bag >= config.min_bag
    vocab_sizes = np.array([len(v) for v in obs.vocab], dtype=np.int64)
    keep &= vocab_sizes[obs.leave_id] <= MAX_LEAVE
    leave_id = obs.leave_id[keep]
    bag = obs.bag[keep].astype(np.int64)
    target = obs.target[keep].astype(np.float64)
    if target.size == 0:
        raise ValueError("no observations with enough tiles in the bag")

    # 1. Centre per bag size and weight by inverse per-bag-size variance,
    #    normalised so the mean weight is 1 (weights then read as "effective
    #    observations").
    bag_n = np.bincount(bag, minlength=101).astype(np.float64)
    bag_sum = np.bincount(bag, weights=target, minlength=101)
    bag_mean = np.divide(bag_sum, bag_n, out=np.zeros(101), where=bag_n > 0)
    resid = target - bag_mean[bag]
    bag_sq = np.bincount(bag, weights=resid ** 2, minlength=101)
    bag_var = np.divide(bag_sq, bag_n, out=np.ones(101), where=bag_n > 1)
    obs_weight = 1.0 / np.maximum(bag_var[bag], 1e-9)
    obs_weight /= obs_weight.mean()

    # Per-leave sufficient statistics.
    n_vocab = len(obs.vocab)
    w_sum = np.bincount(leave_id, weights=obs_weight, minlength=n_vocab)
    wr_sum = np.bincount(leave_id, weights=obs_weight * resid,
                         minlength=n_vocab)
    wrr_sum = np.bincount(leave_id, weights=obs_weight * resid ** 2,
                          minlength=n_vocab)
    seen = w_sum > 0
    mean = np.divide(wr_sum, w_sum, out=np.zeros(n_vocab), where=seen)

    # Within-leave noise variance per unit of weight, pooled over the
    # leaves of each length: longer leaves arise from rarer, more varied
    # situations (one-tile plays, small exchanges) and are noisier.
    leave_n = np.bincount(leave_id, minlength=n_vocab)
    within = wrr_sum - w_sum * mean ** 2
    noise_var = np.full(RACK_SIZE, np.nan)
    for length in range(MAX_LEAVE + 1):
        group = seen & (vocab_sizes == length)
        dof = int(leave_n[group].sum()) - int(group.sum())
        if dof > 0:
            noise_var[length] = float(within[group].sum()) / dof

    empty = [i for i, v in enumerate(obs.vocab) if v == ""]
    empty_mean = float(mean[empty[0]]) if empty and seen[empty[0]] else 0.0
    if config.anchor_empty:
        shift = empty_mean
    else:
        shift = float(np.sum(obs_weight * resid) / obs_weight.sum())

    # 2. Prior: weighted least squares over the observed non-empty leaves.
    nonempty = seen & (vocab_sizes >= 1)
    ids = np.flatnonzero(nonempty)
    seen_counts = counts_matrix(obs.vocab[i] for i in ids)
    pooled_noise = float(np.nansum(noise_var * np.bincount(
        vocab_sizes[leave_id], minlength=RACK_SIZE)[:RACK_SIZE])
        / leave_id.size)
    model = fit_prior(seen_counts, mean[ids], w_sum[ids],
                      ridge=pooled_noise / config.coef_sd ** 2)
    seen_prior = model.predict(seen_counts)

    # 3. Shrinkage strength per leave length (empirical Bayes).
    seen_len = vocab_sizes[ids]
    tau2 = np.full(RACK_SIZE, np.nan)
    shrinkage_k = np.full(RACK_SIZE, np.nan)
    for length in range(1, MAX_LEAVE + 1):
        group = seen_len == length
        if group.sum() < 2 or not np.isfinite(noise_var[length]):
            continue
        tau2[length] = _random_effects_tau2(
            mean[ids][group] - seen_prior[group], w_sum[ids][group],
            noise_var[length])
        shrinkage_k[length] = noise_var[length] / tau2[length]

    # Assemble every possible leave: prior everywhere, shrunk mean where
    # observed.
    if all_leaves is None:
        all_leaves = enumerate_leaves()
    all_strings = [leave_string(row) for row in all_leaves]
    position = {leave: i for i, leave in enumerate(all_strings)}
    del all_strings
    prior = model.predict(all_leaves)
    weight = np.zeros(len(all_leaves))
    raw_mean = np.full(len(all_leaves), np.nan)
    for i, w, m in zip(ids, w_sum[ids], mean[ids]):
        j = position.get(obs.vocab[i])
        if j is None:
            raise ValueError(f"observed leave {obs.vocab[i]!r} is not in "
                             "the leave universe")
        weight[j] = w
        raw_mean[j] = m
    sizes = all_leaves.sum(axis=1).astype(np.int64)
    k = shrinkage_k[sizes]
    observed = weight > 0
    values = prior.copy()
    values[observed] = ((weight[observed] * raw_mean[observed]
                         + k[observed] * prior[observed])
                        / (weight[observed] + k[observed]))

    return LeaveFit(model=model.shifted(shift), leaves=all_leaves,
                    values=values - shift, prior=prior - shift,
                    weight=weight, raw_mean=raw_mean - shift,
                    shrinkage_k=shrinkage_k, tau2=tau2, noise_var=noise_var,
                    shift=shift, observations=int(keep.sum()))


# ---------------------------------------------------------------------------
# Output


def write_table(fit: LeaveFit, path: Path, max_size: int = MAX_LEAVE,
                header: Sequence[str] = ()) -> int:
    """Writes the CSV read by ``LeaveEvaluator::Load``; returns row count.

    Leaves of up to ``max_size`` tiles are listed explicitly. The prior
    model's coefficients follow as ``@`` rows, which the evaluator uses for
    any leave of 1..6 tiles not listed, so a truncated table still values
    every leave.
    """
    buffer = io.StringIO()
    for line in header:
        buffer.write(f"# {line}\n")
    buffer.write("leave,value\n")
    sizes = fit.leaves.sum(axis=1)
    rows = 0
    for row, value, size in zip(fit.leaves, fit.values, sizes):
        if size <= max_size:
            buffer.write(f"{leave_string(row)},{value:.2f}\n")
            rows += 1
    buffer.write("# Fallback model for unlisted leaves of 1-6 tiles: the sum"
                 " of the\n# @<tile><count> terms of the tiles held, @len<n>,"
                 " @v<vowels>c<consonants>\n# and, if a Q is held without a"
                 " U, @qnou.\n")
    for name, value in fit.model.rows():
        buffer.write(f"{name},{value:.4f}\n")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(buffer.getvalue(), encoding="ascii")
    return rows


def read_table(path: Path) -> dict[str, float]:
    """Reads the leave rows (not the model rows) of a leave CSV."""
    table: dict[str, float] = {}
    for line in path.read_text(encoding="ascii").splitlines():
        line = line.strip()
        if not line or line.startswith(("#", "@")) or line == "leave,value":
            continue
        leave, value = line.split(",")
        table[leave_string(parse_leave(leave.strip()))] = float(value)
    return table
