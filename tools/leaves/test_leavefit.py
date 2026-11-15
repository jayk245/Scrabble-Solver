"""Unit tests for leavefit and selfplay.collect, on synthetic data.

Run from the repository root with::

    python3 -m unittest discover -s tools/leaves
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import numpy as np

import leavefit as lf
from selfplay import collect


def synthetic(rng: np.random.Generator, universe: lf.Counts, n_obs: int,
              tau: float, noise: float
              ) -> tuple[lf.Observations, np.ndarray]:
    """Observations whose leaves have known true values.

    True values are a random linear model over the prior's features plus an
    independent per-leave deviation of standard deviation ``tau``. Each
    observation's target is its leave's true value plus a bag-size offset
    plus Gaussian noise whose spread also depends on bag size, mimicking
    the phase effects the fit is meant to remove. The empty leave is worth
    exactly 0, so with the empty-leave anchor the fit should recover the
    true values directly.
    """
    design = lf._Design.build()  # pylint: disable=protected-access
    model = design.to_model(rng.normal(0.0, 3.0, design.size))
    truth = model.predict(universe) + rng.normal(0.0, tau, len(universe))
    vocab = [""] + [lf.leave_string(row) for row in universe]
    true_by_id = np.concatenate([[0.0], truth])
    # Zipf-like popularity: a few leaves are common, most are rare.
    popularity = 1.0 / np.arange(1, len(vocab) + 1) ** 0.8
    popularity = rng.permutation(popularity)
    popularity[0] = popularity.max()  # the empty leave is common
    leave_id = rng.choice(len(vocab), size=n_obs,
                          p=popularity / popularity.sum()).astype(np.int32)
    bag = rng.integers(0, 87, size=n_obs).astype(np.uint8)
    phase_offset = -0.3 * bag.astype(np.float64)
    phase_noise = noise * (0.5 + bag / 86.0)
    target = (true_by_id[leave_id] + phase_offset
              + rng.normal(0.0, 1.0, n_obs) * phase_noise)
    obs = lf.Observations(vocab, leave_id, bag,
                          np.round(target).astype(np.int32))
    return obs, truth


class EncodingTest(unittest.TestCase):

    def test_round_trip_is_canonical(self) -> None:
        counts = lf.parse_leave("s?eR")
        self.assertEqual(lf.leave_string(counts), "ERS?")
        with self.assertRaises(ValueError):
            lf.parse_leave("E1")

    def test_enumeration_respects_the_bag(self) -> None:
        leaves = lf.enumerate_leaves(2)
        # 27 singles, plus pairs of distinct tiles, plus doubles of every
        # tile with at least two copies (all but J, K, Q, X and Z).
        self.assertEqual(len(leaves), 27 + 27 * 26 // 2 + 22)
        strings = [lf.leave_string(row) for row in leaves]
        self.assertEqual(len(set(strings)), len(strings))
        self.assertIn("??", strings)
        self.assertNotIn("QQ", strings)
        self.assertEqual(strings[0], "A")
        self.assertEqual(strings[26], "?")
        self.assertEqual(len(lf.enumerate_leaves()), 914_624)

    def test_model_sums_its_terms(self) -> None:
        rng = np.random.default_rng(7)
        design = lf._Design.build()  # pylint: disable=protected-access
        model = design.to_model(rng.normal(0.0, 3.0, design.size))
        terms = dict(model.rows())
        self.assertEqual(len(terms), len(model.rows()))
        values = model.predict(lf.counts_matrix(["QEE", "??"]))
        self.assertAlmostEqual(values[0], sum(terms[k] for k in (
            "@E2", "@Q1", "@len3", "@qnou", "@v2c1")))
        # Blanks only: no balance term.
        self.assertAlmostEqual(values[1], terms["@?2"] + terms["@len2"])

    def test_rare_duplicates_borrow_the_shared_penalty(self) -> None:
        # Pairs are common and cost 5 points each; other leaves are worth
        # 0. Four Ls are never observed, yet should come out near -15
        # (three extra copies), not near 0.
        letters = "ADEGILNORTU"
        leaves = [a + b for i, a in enumerate(letters)
                  for b in letters[i:]] + list(letters)
        counts = lf.counts_matrix(leaves)
        extra = np.maximum(counts[:, :lf.BLANK].astype(int) - 1, 0).sum(1)
        means = -5.0 * extra
        model = lf.fit_prior(counts, means, np.full(len(leaves), 1e4),
                             ridge=10.0)
        quad = model.predict(lf.counts_matrix(["LLLL"]))[0]
        self.assertAlmostEqual(quad, -15.0, delta=2.0)


class FitTest(unittest.TestCase):

    def setUp(self) -> None:
        self.rng = np.random.default_rng(12345)
        self.universe = lf.enumerate_leaves(3)

    def test_recovers_a_purely_linear_truth(self) -> None:
        obs, truth = synthetic(self.rng, self.universe, 400_000, tau=0.0,
                               noise=10.0)
        fit = lf.fit_leaves(obs, lf.FitConfig(min_bag=0),
                            all_leaves=self.universe)
        # With no leave-specific deviations the prior is the whole story
        # and fits leaves, seen or not, to a fraction of a point. (The
        # worst are triples such as III, whose @I3 term only one leave in
        # this universe informs.)
        for estimate in (fit.prior, fit.values):
            error = np.abs(estimate - truth)
            self.assertLess(float(np.sqrt(np.mean(error ** 2))), 0.3)
            self.assertLess(float(np.quantile(error, 0.99)), 1.0)
        # Little genuine spread around the prior, so shrinkage is strong.
        self.assertGreater(fit.shrinkage_k[3], 100)

    def test_shrinkage_beats_raw_means_and_prior(self) -> None:
        obs, truth = synthetic(self.rng, self.universe, 300_000, tau=3.0,
                               noise=20.0)
        fit = lf.fit_leaves(obs, lf.FitConfig(min_bag=0),
                            all_leaves=self.universe)
        seen = fit.weight > 0

        def rmse(estimate: np.ndarray) -> float:
            return float(np.sqrt(np.mean((estimate[seen] - truth[seen]) ** 2)))

        self.assertLess(rmse(fit.values), rmse(fit.raw_mean))
        self.assertLess(rmse(fit.values), rmse(fit.prior))
        # The estimated spread around the prior is close to the true 3.
        self.assertAlmostEqual(float(np.sqrt(fit.tau2[3])), 3.0, delta=0.5)
        # Each value lies between its raw mean and its prior, nearer the
        # prior when there is less data.
        lo = np.minimum(fit.raw_mean[seen], fit.prior[seen]) - 1e-9
        hi = np.maximum(fit.raw_mean[seen], fit.prior[seen]) + 1e-9
        self.assertTrue(np.all((fit.values[seen] >= lo)
                               & (fit.values[seen] <= hi)))
        # Unseen leaves get the prior.
        np.testing.assert_array_equal(fit.values[~seen], fit.prior[~seen])

    def test_min_bag_and_anchor(self) -> None:
        obs, truth = synthetic(self.rng, self.universe, 200_000, tau=1.0,
                               noise=10.0)
        late = lf.fit_leaves(obs, lf.FitConfig(min_bag=80),
                             all_leaves=self.universe)
        self.assertLess(late.observations, len(obs) / 5)
        # Anchoring on the average instead of the empty leave moves every
        # value by the same amount.
        by_empty = lf.fit_leaves(obs, all_leaves=self.universe)
        by_mean = lf.fit_leaves(obs, lf.FitConfig(anchor_empty=False),
                                all_leaves=self.universe)
        offset = by_mean.values - by_empty.values
        self.assertLess(np.ptp(offset), 1e-9)
        self.assertLess(abs(by_mean.shift), 1e-9)

    def test_table_round_trip(self) -> None:
        obs, _ = synthetic(self.rng, self.universe, 50_000, tau=1.0,
                           noise=10.0)
        fit = lf.fit_leaves(obs, all_leaves=self.universe)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "leaves.csv"
            rows = lf.write_table(fit, path, max_size=2, header=["test"])
            self.assertEqual(rows, 27 + 373)
            table = lf.read_table(path)
            text = path.read_text()
        self.assertEqual(len(table), rows)
        strings = fit.strings()
        self.assertAlmostEqual(table["S?"], fit.values[strings.index("S?")],
                               places=2)
        self.assertIn("@len1,", text)
        self.assertIn("@qnou,", text)
        # The model rows reproduce the prior of an unlisted 3-tile leave.
        terms = dict(line.split(",") for line in text.splitlines()
                     if line.startswith("@"))
        i = strings.index("EQS")
        model = sum(float(terms[k]) for k in
                    ("@E1", "@Q1", "@S1", "@len3", "@v1c2", "@qnou"))
        self.assertAlmostEqual(model, fit.prior[i], places=2)


class CollectTest(unittest.TestCase):

    def test_parses_arena_log_lines(self) -> None:
        def line(**fields: object) -> str:
            record = {"game": 0, "seed": 1, "turn": 0, "player": 0,
                      "bot": "A", "rack": "AEINRST", "move": "8D RETAINS",
                      "type": "place", "score": 70, "equity": 70.0,
                      "leave": "", "bag": 86, "spread": 0,
                      "final_spread": 100}
            record.update(fields)
            return json.dumps(record)

        obs = collect([
            line(),
            line(leave="ER", bag=40, spread=-5, score=20, final_spread=30),
            line(leave="ER", bag=0),                      # bag empty
            line(leave="AEINRST", type="pass", score=0),  # whole rack
        ])
        self.assertEqual(obs.vocab, ["", "ER"])
        np.testing.assert_array_equal(obs.leave_id, [0, 1])
        np.testing.assert_array_equal(obs.bag, [86, 40])
        # final_spread - spread - score.
        np.testing.assert_array_equal(obs.target, [30, 15])

    def test_concatenate_merges_vocabularies(self) -> None:
        a = lf.Observations(["", "S"], np.array([1, 0], np.int32),
                            np.array([9, 9], np.uint8),
                            np.array([1, 2], np.int32))
        b = lf.Observations(["Q", "S"], np.array([0, 1], np.int32),
                            np.array([8, 8], np.uint8),
                            np.array([3, 4], np.int32))
        both = lf.Observations.concatenate([a, b])
        self.assertEqual(both.vocab, ["", "S", "Q"])
        self.assertEqual([both.vocab[i] for i in both.leave_id],
                         ["S", "", "Q", "S"])


if __name__ == "__main__":
    unittest.main()
