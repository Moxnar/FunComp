#!/usr/bin/env python3
"""Cost head for the learned pruning guide, trained on genlabels CSVs.

Two heads, because the labels hold two different costs (see labels.h):

  tree head   (P, d, context) -> quantiles of log2 tree_nodes
      What this node will cost here, in this tree: window, hash entry and
      warm histories included. This is the number a search decision pays.

  fresh head  (P, k) -> quantiles of log2 N_fresh(P, k), k = 1..depth
      What depth k costs from a cold start. Every row gives `depth` points
      (fresh_curve), so this head learns the cost curve, and from it the
      marginal cost of one more ply:
          log2 N(P, d+1) - log2 N(P, d)       (the local branching factor)
      which the tree cost alone can't give, since each tree node is only
      ever seen at one depth.

Targets are log2 nodes and the loss is pinball (quantile) loss, as for the
eval head. Rows are split into training and validation by `group` (one
driver search), never by row: rows of one search share positions and
subtrees. Every metric is reported next to a baseline, the mean log2 cost
per (depth, node, tt_hit, tt_bound) cell: depth alone explains most of
the variance, and the head is only worth running if it beats that.

    python3 train_cost.py labels*.csv [--epochs 30] [--val 0.2] [--out cost_head.npz]
"""
import argparse
import csv
import math
import sys

import numpy as np

QUANTILES = np.array([0.1, 0.5, 0.9])
FEATURES = ["phase", "material", "our_pieces", "their_pieces", "our_pawns", "their_pawns",
            "our_threatened", "their_threatened", "best_capture", "our_king_attackers",
            "their_king_attackers", "passed_pawns", "mobility", "halfmove_clock"]
MAX_DEPTH = 16          # depth one-hot width; deeper depths share the last slot
BOUNDS = ["none", "upper", "lower", "exact"]
NODES = ["pv", "cut", "all"]


def onehot(i, n):
    v = np.zeros(n, np.float32)
    v[min(i, n - 1)] = 1
    return v


def depth_code(d):
    # A one-hot per depth, so each can have its own offset, plus the depth
    # itself, so the net can extrapolate the roughly log-linear growth.
    return np.concatenate([onehot(d - 1, MAX_DEPTH), [d / 8.0]]).astype(np.float32)


def position_inputs(r):
    return np.array([float(r[f]) for f in FEATURES]
                    + [float(r["static_eval"]) / 400, float(r["correction"]) / 100], np.float32)


def tree_context(r):
    # Only what the search knows at the sampling point, before node-level
    # pruning: the head must be computable where it would be used.
    d = int(r["depth"])
    tt_hit = int(r["tt_hit"])
    tt_gap = (int(r["tt_depth"]) - d) if tt_hit else 0
    margin = max(-4.0, min(4.0, (int(r["search_eval"]) - int(r["beta"])) / 200))
    return np.concatenate([
        onehot(NODES.index(r["node"]), 3), onehot(BOUNDS.index(r["tt_bound"]), 4),
        [tt_hit, max(-8, min(8, tt_gap)) / 4, margin, int(r["improving"]),
         int(r["under_null"]), int(r["iir"]), min(int(r["ply"]), 40) / 20],
    ]).astype(np.float32)


def load(paths):
    rows = []
    for gi, p in enumerate(paths):
        with open(p) as f:
            for r in csv.DictReader(f):
                if not r["fresh_curve"]:          # stalemate: no search, no cost
                    continue
                r["_group"] = (gi, int(r["group"]))  # groups are per file
                rows.append(r)
    return rows


def build(rows):
    """Arrays for both heads. Censored fresh points (iterations past the
    node cap) become one-sided targets: only 'more than the cap' is known."""
    tx, ty, tcell, tw, tg, treached = [], [], [], [], [], []
    fx, fy, fcens, fg = [], [], [], []
    for r in rows:
        d = int(r["depth"])
        pos = position_inputs(r)
        g = r["_group"]
        tx.append(np.concatenate([pos, depth_code(d), tree_context(r)]))
        ty.append(math.log2(int(r["tree_nodes"])))
        # Baseline cell: only what the head sees too. reached_moves is an
        # outcome of the node-level pruning that follows the sampling point.
        tcell.append((d, r["node"], r["tt_hit"], r["tt_bound"]))
        treached.append(int(r["reached_moves"]))
        tw.append(float(r["rate"]))
        tg.append(g)
        curve = [int(v) for v in r["fresh_curve"].split(";")]
        # A short curve is either capped (censored) or ended by a mate
        # found early, which deeper iterations don't search past: only the
        # first gets one-sided points past its end.
        last = d if r["censored"] == "1" else len(curve)
        for k in range(1, last + 1):
            fx.append(np.concatenate([pos, depth_code(k)]))
            fg.append(g)
            if k <= len(curve):
                fy.append(math.log2(curve[k - 1]))
                fcens.append(0)
            else:                                  # censored: at least the cap
                fy.append(math.log2(int(r["fresh_nodes"])))
                fcens.append(1)
    return (np.array(tx), np.array(ty, np.float32), tcell, np.array(tw, np.float32), tg,
            np.array(treached, bool),
            np.array(fx), np.array(fy, np.float32), np.array(fcens, np.int8), fg)


class QuantileMLP:
    """inputs -> hidden (ReLU) -> hidden (ReLU) -> one output per quantile.
    Quantiles are the median plus softplus offsets, so they never cross."""

    def __init__(self, n_in, hidden=32, seed=1):
        rng = np.random.default_rng(seed)
        he = lambda a, b: rng.normal(0, math.sqrt(2 / a), (a, b)).astype(np.float32)
        self.p = {"W1": he(n_in, hidden), "b1": np.zeros(hidden, np.float32),
                  "W2": he(hidden, hidden), "b2": np.zeros(hidden, np.float32),
                  "W3": he(hidden, 3) * 0.1, "b3": np.zeros(3, np.float32)}
        self.m = {k: np.zeros_like(v) for k, v in self.p.items()}
        self.v = {k: np.zeros_like(v) for k, v in self.p.items()}
        self.t = 0
        self.mu = self.sd = None

    def normalise(self, x):
        if self.mu is None:
            self.mu, self.sd = x.mean(0), x.std(0) + 1e-6
            binary = np.all((x == 0) | (x == 1), axis=0)   # leave one-hots alone
            self.mu[binary], self.sd[binary] = 0, 1
        return (x - self.mu) / self.sd

    def forward(self, xn):
        p = self.p
        h1 = np.maximum(xn @ p["W1"] + p["b1"], 0)
        h2 = np.maximum(h1 @ p["W2"] + p["b2"], 0)
        z = h2 @ p["W3"] + p["b3"]
        sp = np.log1p(np.exp(-np.abs(z[:, [0, 2]]))) + np.maximum(z[:, [0, 2]], 0)
        q = np.stack([z[:, 1] - sp[:, 0], z[:, 1], z[:, 1] + sp[:, 1]], 1)
        return q, (xn, h1, h2, z)

    def predict(self, x):
        return self.forward(self.normalise(x))[0]

    def step(self, x, y, cens, lr):
        q, (xn, h1, h2, z) = self.forward(x)
        err = y[:, None] - q
        # Pinball gradient; for a censored point (true value >= y) only a
        # prediction below y is wrong.
        dq = np.where(err > 0, -QUANTILES, 1 - QUANTILES).astype(np.float32)
        if cens is not None:
            c = cens.astype(bool)
            dq[c] = np.where(err[c] > 0, -QUANTILES, 0)
        dq /= len(y)
        sig = lambda a: 1 / (1 + np.exp(-a))
        dz = np.empty_like(z)
        dz[:, 1] = dq.sum(1)
        dz[:, 0] = -dq[:, 0] * sig(z[:, 0])
        dz[:, 2] = dq[:, 2] * sig(z[:, 2])
        p = self.p
        g = {"W3": h2.T @ dz, "b3": dz.sum(0)}
        dh2 = (dz @ p["W3"].T) * (h2 > 0)
        g["W2"], g["b2"] = h1.T @ dh2, dh2.sum(0)
        dh1 = (dh2 @ p["W2"].T) * (h1 > 0)
        g["W1"], g["b1"] = xn.T @ dh1, dh1.sum(0)
        self.t += 1
        for k in p:                                            # Adam
            self.m[k] = 0.9 * self.m[k] + 0.1 * g[k]
            self.v[k] = 0.999 * self.v[k] + 0.001 * g[k] ** 2
            mh = self.m[k] / (1 - 0.9 ** self.t)
            vh = self.v[k] / (1 - 0.999 ** self.t)
            p[k] -= lr * mh / (np.sqrt(vh) + 1e-8)

    def fit(self, x, y, cens=None, epochs=30, batch=512, lr=2e-3, seed=1):
        xn = self.normalise(x)
        rng = np.random.default_rng(seed)
        for e in range(epochs):
            order = rng.permutation(len(y))
            for i in range(0, len(y), batch):
                j = order[i:i + batch]
                self.step(xn[j], y[j], None if cens is None else cens[j],
                          lr * (0.5 * (1 + math.cos(math.pi * e / epochs))))


def pinball(q, y):
    err = y[:, None] - q
    return float(np.mean(np.maximum(QUANTILES * err, (QUANTILES - 1) * err)))


def cell_baseline(train_cells, train_y, cells):
    """Per-cell quantiles from the training rows; unseen cells fall back to
    the depth's, then everything's."""
    by, by_d = {}, {}
    for c, v in zip(train_cells, train_y):
        by.setdefault(c, []).append(v)
        by_d.setdefault(c[0], []).append(v)
    qs = lambda vs: np.quantile(vs, QUANTILES)
    everything = qs(train_y)
    table = {c: qs(v) for c, v in by.items() if len(v) >= 20}
    table_d = {d: qs(v) for d, v in by_d.items()}
    return np.array([table.get(c, table_d.get(c[0], everything)) for c in cells])


def report(name, q, y, base, w=None):
    cov = lambda q: (np.mean(y < q[:, 0]), np.mean(y < q[:, 2]))
    r2 = lambda q: 1 - np.mean((y - q[:, 1]) ** 2) / np.var(y)
    lo, hi = cov(q)
    blo, bhi = cov(base)
    print(f"  {name}: pinball {pinball(q, y):.3f} (baseline {pinball(base, y):.3f}), "
          f"R2 of median {r2(q):.3f} (baseline {r2(base):.3f}), "
          f"below q10/q90 {lo:.2f}/{hi:.2f} (baseline {blo:.2f}/{bhi:.2f})")
    if w is not None:   # reweighted to how often each depth occurs in a real tree
        err = y[:, None] - q
        wp = np.average(np.maximum(QUANTILES * err, (QUANTILES - 1) * err).mean(1), weights=w)
        err = y[:, None] - base
        bp = np.average(np.maximum(QUANTILES * err, (QUANTILES - 1) * err).mean(1), weights=w)
        print(f"  {name}, tree-weighted: pinball {wp:.3f} (baseline {bp:.3f})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--val", type=float, default=0.2, help="fraction of groups held out")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default="cost_head.npz")
    a = ap.parse_args()

    rows = load(a.csv)
    tx, ty, tcell, tw, tg, treached, fx, fy, fcens, fg = build(rows)
    groups = sorted(set(tg))
    rng = np.random.default_rng(a.seed)
    val_groups = set(map(tuple, rng.permutation(np.array(groups))[:max(1, int(len(groups) * a.val))]))
    tv = np.array([g in val_groups for g in tg])
    fv = np.array([g in val_groups for g in fg])
    print(f"{len(rows)} rows, {len(groups)} groups ({len(val_groups)} held out); "
          f"{len(fy)} fresh-curve points, {int(fcens.sum())} censored")
    if len(val_groups) < 2:
        print("warning: one validation group; metrics will be noisy", file=sys.stderr)

    tree = QuantileMLP(tx.shape[1], seed=a.seed)
    tree.fit(tx[~tv], ty[~tv], epochs=a.epochs, seed=a.seed)
    tcell_tr = [c for c, v in zip(tcell, tv) if not v]
    tcell_va = [c for c, v in zip(tcell, tv) if v]
    print("tree head, log2 tree_nodes, validation groups:")
    tq = tree.predict(tx[tv])
    tb = cell_baseline(tcell_tr, ty[~tv], tcell_va)
    report("all", tq, ty[tv], tb, tw[tv])
    # Most of the spread is pruned-at-once (a few nodes) against searched
    # (hundreds): how well the head does within each says whether it learned
    # more than the odds of being pruned.
    r = treached[tv]
    for name, m in (("pruned before moves", ~r), ("reached moves", r)):
        if m.sum() > 50:
            report(name, tq[m], ty[tv][m], tb[m])

    fresh = QuantileMLP(fx.shape[1], seed=a.seed)
    fresh.fit(fx[~fv], fy[~fv], cens=fcens[~fv], epochs=max(5, a.epochs // 3), seed=a.seed)
    ok = fv & (fcens == 0)
    kcell = lambda m: [(int(np.argmax(x[len(FEATURES) + 2:len(FEATURES) + 2 + MAX_DEPTH])) + 1,)
                       for x in fx[m]]
    tr = ~fv & (fcens == 0)
    print("fresh head, log2 N_fresh(P, k), validation groups (uncensored points):")
    report("all", fresh.predict(fx[ok]), fy[ok], cell_baseline(kcell(tr), fy[tr], kcell(ok)))

    # The marginal cost of one more ply, from the fresh head's medians.
    x = fx[ok]
    deeper = x.copy()
    k = np.argmax(x[:, len(FEATURES) + 2:len(FEATURES) + 2 + MAX_DEPTH], 1) + 1
    for i, kk in enumerate(k):
        deeper[i, len(FEATURES) + 2:] = depth_code(min(kk + 1, MAX_DEPTH))
    step = fresh.predict(deeper)[:, 1] - fresh.predict(x)[:, 1]
    print(f"  predicted log2 cost of one more ply: mean {step.mean():.2f}, "
          f"10-90% {np.quantile(step, 0.1):.2f} to {np.quantile(step, 0.9):.2f}")

    np.savez(a.out, **{f"tree_{k}": v for k, v in tree.p.items()},
             tree_mu=tree.mu, tree_sd=tree.sd,
             **{f"fresh_{k}": v for k, v in fresh.p.items()},
             fresh_mu=fresh.mu, fresh_sd=fresh.sd, quantiles=QUANTILES)
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
