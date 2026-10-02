"""Label studies for the learned pruning guide (ROADMAP.md, "Research groundwork").

Run in WSL (numpy and scipy are Ubuntu packages there):
    python3 tools/research/labels_study.py matches/labels12a/tuned.csv matches/labels12a/v10.csv

On genlabels CSVs (labels.h) it reports, for each set:
  1. Calibration of the hand RFP margin: how often the searched score falls more
     than the margin below the eval that pruning used (an RFP-style error), by
     depth, phase and threat status. The margin is recomputed from the .params
     next to the CSV (RfpMargin, RfpImproving, CorrMarginPct); the hash-PV term
     (RfpTtPvMargin) is left out, since labels don't record the PV flag.
  2. Headroom: a linear quantile model of the gap (search_eval - score) at the
     98th percentile, on hand features only (depth, improving, correction) and on
     all features; compared by the mean margin each needs to cover 98% of
     held-out positions. A smaller margin at the same coverage prunes more.
And with two sets, 3. the transfer test: the all-feature model fitted on the
second set, evaluated on the first: does it keep its 98% coverage?

The gap target: g = search_eval - score, clipped to +-1000; mates dropped.
"""

import csv
import glob
import os
import sys

import numpy as np
from scipy.optimize import minimize

TAU = 0.98
CLIP = 1000

HAND = ["depth", "improving", "abs_correction"]
EXTRA = ["phase", "material", "our_pieces", "their_pieces", "our_pawns", "their_pawns",
         "our_threatened", "their_threatened", "best_capture", "our_king_attackers",
         "their_king_attackers", "passed_pawns", "mobility", "halfmove_clock",
         "under_null", "iir", "node_cut", "node_all"]


def read_params(csv_path):
    """The .params of the CSV, or of its first part file (tuned-01.csv for tuned.csv)."""
    candidates = [csv_path + ".params"]
    stem = os.path.splitext(csv_path)[0]
    candidates += sorted(glob.glob(stem + "-*.csv.params"))
    for p in candidates:
        if os.path.exists(p):
            out = {}
            with open(p) as f:
                for line in f:
                    if "=" in line and not line.startswith("#"):
                        k, v = line.strip().split("=", 1)
                        out[k] = v
            return out
    return {}


def load(path):
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        rows = [r for r in reader if r["mate"] == "0"]
    def col(name, conv=float):
        return np.array([conv(r[name]) for r in rows])
    d = {k: col(k) for k in ["depth", "static_eval", "correction", "search_eval", "alpha", "beta",
                              "improving", "under_null", "iir", "score", "phase", "material",
                              "our_pieces", "their_pieces", "our_pawns", "their_pawns",
                              "our_threatened", "their_threatened", "best_capture",
                              "our_king_attackers", "their_king_attackers", "passed_pawns",
                              "mobility", "halfmove_clock"]}
    node = [r["node"] for r in rows]
    d["node_cut"] = np.array([n == "cut" for n in node], dtype=float)
    d["node_all"] = np.array([n == "all" for n in node], dtype=float)
    d["abs_correction"] = np.abs(d["correction"])
    d["gap"] = np.clip(d["search_eval"] - d["score"], -CLIP, CLIP)
    return d


def design(d, names, stats=None):
    """Standardised features (with `stats`, another set's means and spreads), an
    intercept and a depth-squared term; returns the matrix and the stats used."""
    if stats is None:
        stats = {n: (d[n].mean(), d[n].std() or 1.0) for n in names + ["depth"]}
    cols = [np.ones_like(d["depth"])]
    for n in names:
        mu, sd = stats[n]
        cols.append((d[n] - mu) / sd)
    mu, sd = stats["depth"]
    dz = (d["depth"] - mu) / sd
    cols.append(dz * dz)
    return np.column_stack(cols), stats


def fit_quantile(X, y, tau=TAU, eps=2.0):
    """Linear quantile regression: minimise a smoothed pinball loss (L-BFGS)."""
    def loss(w):
        r = y - X @ w
        # Smooth |r| as sqrt(r^2 + eps^2): pinball = tau*r for r>0, (tau-1)*r for r<0.
        a = np.sqrt(r * r + eps * eps)
        val = np.mean(0.5 * a + (tau - 0.5) * r)
        grad_r = 0.5 * r / a + (tau - 0.5)
        return val, -(X.T @ grad_r) / len(y)
    w0 = np.zeros(X.shape[1])
    w0[0] = np.quantile(y, tau)
    res = minimize(loss, w0, jac=True, method="L-BFGS-B", options={"maxiter": 2000})
    return res.x


def coverage_margin(pred, y, target=TAU):
    """Shift the predictions so `target` of held-out gaps are covered; the mean margin then."""
    shift = np.quantile(y - pred, target)
    return float(np.mean(np.maximum(pred + shift, 0))), shift


def calibration(name, d, params):
    m = float(params.get("RfpMargin", 75))
    imp = float(params.get("RfpImproving", 0))
    corr = float(params.get("CorrMarginPct", 0))
    margin = m * d["depth"] - imp * d["improving"] + d["abs_correction"] * corr / 100
    err = d["gap"] > margin
    print(f"\n[{name}] calibration of the hand RFP margin (RfpMargin {m:.0f}, RfpImproving "
          f"{imp:.0f}, CorrMarginPct {corr:.0f}; hash-PV term omitted)")
    print(f"  all: {err.mean() * 100:.2f}% of {len(err)} positions have gap > margin")
    print("  by depth: " + "  ".join(
        f"{int(k)}:{err[d['depth'] == k].mean() * 100:.1f}%" for k in np.unique(d["depth"])))
    phase_band = np.digitize(d["phase"], [8, 16])
    print("  by phase (endgame <8, middle, opening >=16): " + "  ".join(
        f"{lab}:{err[phase_band == i].mean() * 100:.1f}%"
        for i, lab in enumerate(["end", "mid", "open"]) if (phase_band == i).any()))
    thr = d["our_threatened"] > 0
    print(f"  threatened (a piece of ours en prise to a cheaper one): "
          f"{err[thr].mean() * 100:.1f}% ({thr.mean() * 100:.0f}% of positions); "
          f"not: {err[~thr].mean() * 100:.1f}%")


def hand_margin(d, params):
    m = float(params.get("RfpMargin", 75))
    imp = float(params.get("RfpImproving", 0))
    corr = float(params.get("CorrMarginPct", 0))
    return m * d["depth"] - imp * d["improving"] + d["abs_correction"] * corr / 100


def prunes_at_errors(margin, d, sel, errors):
    """Shift `margin` until RFP's wrong prunes (search_eval - margin >= beta while the
    searched score < beta) on `sel` number `errors`; the prunes it makes then."""
    ev, beta, score = d["search_eval"][sel], d["beta"][sel], d["score"][sel]
    lo, hi = -2000.0, 2000.0
    for _ in range(60):
        s = (lo + hi) / 2
        prune = ev - (margin + s) >= beta
        if np.sum(prune & (score < beta)) > errors:
            lo = s
        else:
            hi = s
    prune = ev - (margin + hi) >= beta
    return int(prune.sum()), int(np.sum(prune & (score < beta)))


def headroom(name, d, params, rng):
    """RFP at non-PV nodes, held-out rows: matched to the hand margin's wrong prunes,
    how many prunes does each margin model make?"""
    n = len(d["gap"])
    idx = rng.permutation(n)
    tr, te = idx[: n * 7 // 10], idx[n * 7 // 10:]
    te = te[d["node_cut"][te] + d["node_all"][te] > 0]  # RFP never runs at PV nodes
    hand = hand_margin(d, params)[te]
    ev, beta, score = d["search_eval"][te], d["beta"][te], d["score"][te]
    base = ev - hand >= beta
    errors = int(np.sum(base & (score < beta)))
    print(f"\n[{name}] headroom on {len(te)} held-out non-PV rows: the hand RFP margin prunes "
          f"{int(base.sum())} with {errors} wrong ({errors / max(base.sum(), 1) * 100:.2f}%); "
          f"at the same wrong count:")
    flat = np.quantile(d["gap"][tr], TAU)
    models = [("constant margin", np.full(len(te), flat))]
    for label, names in [("hand features", HAND), ("all features", HAND + EXTRA)]:
        X, _ = design(d, names)
        w = fit_quantile(X[tr], d["gap"][tr])
        models.append((label, X[te] @ w))
    for label, margin in models:
        p, e = prunes_at_errors(margin, d, te, errors)
        print(f"  {label + ':':18s} {p:6d} prunes ({(p / max(base.sum(), 1) - 1) * 100:+.1f}% "
              f"against the hand margin), {e} wrong")


def transfer(src_name, src, dst_name, dst):
    names = HAND + EXTRA
    Xs, stats = design(src, names)
    w = fit_quantile(Xs, src["gap"])
    own = float(np.mean(src["gap"] <= Xs @ w))
    Xd, _ = design(dst, names, stats)  # the source's scaling: raw features carry over
    pred = Xd @ w
    cov = float(np.mean(dst["gap"] <= pred))
    shifted, _ = coverage_margin(pred, dst["gap"])
    print(f"\n[transfer] fitted on {src_name} ({own * 100:.1f}% coverage there), applied to "
          f"{dst_name} unchanged: {cov * 100:.1f}% coverage (target {TAU * 100:.0f}%); "
          f"mean margin {np.mean(np.maximum(pred, 0)):.1f} cp as is, {shifted:.1f} cp shifted "
          f"to cover {TAU * 100:.0f}% there")


def main():
    paths = sys.argv[1:]
    if not paths:
        print(__doc__)
        return
    rng = np.random.default_rng(1)
    sets = []
    for p in paths:
        name = os.path.basename(p)
        d = load(p)
        params = read_params(p)
        print(f"\n=== {name}: {len(d['gap'])} rows (mates dropped); git {params.get('git', '?')}")
        print(f"  gap = search_eval - score: median {np.median(d['gap']):.0f}, "
              f"98th percentile {np.quantile(d['gap'], TAU):.0f}")
        calibration(name, d, params)
        headroom(name, d, params, rng)
        sets.append((name, d))
    if len(sets) >= 2:
        transfer(sets[1][0], sets[1][1], sets[0][0], sets[0][1])


if __name__ == "__main__":
    main()
