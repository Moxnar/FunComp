"""Try 4 learnability study (ROADMAP.md, item 4b).

Run in WSL on the gate logs (genconlog output, cont_log.h):
    python3 tools/research/try4_learnability.py 'matches/contlog-gate/g*.rows.csv'

Question: can a cheap model, from what the search knows before move k, predict
whether any move from k on will change the node's result (raise alpha or fail
high: what the parent sees)? Every searched move k >= 1 of a sampled node is a
decision point; its label is `later_raise_index >= 0` on row k, as in
oracle_ceiling.py's window variant. The policy stops a node at the first k
whose predicted probability is below a threshold; a miss is a stop where a
later move did change the result, the saving is that row's rest_nodes.

Per depth band, a logistic regression (L2, standardized inputs) on the logged
columns that exist when move k starts: the node's horizon, window, eval and
hash context, the loop so far, and the next move's own summary. Outcome
columns (score, gain, rest_nodes, final_best, bound, nodes_total) are never
inputs. Split by game, not by row: every fourth game (by file and game number)
is held out. Thresholds are chosen on the training games for 0.5, 1 and 2%
misses (of nodes) and reported on the held-out games, beside the oracle
ceiling (perfect foresight, with the same miss budget spent where it saves the
most) and the depth-and-index baseline (stop at a fixed move index per band,
the index chosen on the training games), all as shares of the sampled nodes'
own subtree nodes.

Parsed logs are cached as .npz beside each file (`<prefix>.learn.npz`). On
the 24 gate files: about 2 minutes, and a peak of about 14 GB, just inside
WSL's default 15 GB (TRAIN_KEEP and TRAIN_ROWS bound it).
"""

import csv
import glob
import os
import sys
from multiprocessing import Pool

import numpy as np
from scipy.optimize import minimize

BANDS = [(3, 4), (5, 6), (7, 9), (10, 99)]
MISS_RATES = [0.005, 0.01, 0.02]
MAX_INDEX = 40
TRAIN_ROWS = 3_000_000  # per band, subsampled by node for the fit
TRAIN_KEEP = 6_000_000  # per band, training decisions kept (whole nodes), for memory
CLIP = 1000

NODE_COLS = ["depth", "ply", "root_depth", "parent_reduction", "parent_provisional",
             "extensions", "node_type", "alpha", "beta", "raw_eval", "static_eval", "eval",
             "correction", "improving", "in_check", "threat", "tt_hit", "tt_depth", "tt_bound",
             "tt_score", "tt_move", "phase", "material", "halfmove", "parent_index",
             "parent_quiet", "parent_history", "legal", "legal_captures", "legal_quiets",
             "nodes_total", "game"]
ROW_COLS = ["index", "searched_before", "stage", "pruned_lmp", "pruned_history",
            "pruned_futility", "pruned_see_quiet", "pruned_see_capture", "pruned_underpromo",
            "lmp_skipping", "piece", "promotion", "capture", "gives_check", "killer",
            "hash_move", "see", "history", "capture_history", "lmr_reduction", "alpha",
            "best_before", "second_before", "near_best", "raised_before", "researches_before",
            "nodes_before", "later_raise_index", "rest_nodes"]
NONE = -99999  # an empty score field


def num(s):
    return int(s) if s != "" else NONE


def parse(rows_path):
    cache = rows_path.replace(".rows.csv", ".learn.npz")
    if os.path.exists(cache) and os.path.getmtime(cache) >= os.path.getmtime(rows_path):
        return cache
    nodes_path = rows_path.replace(".rows.csv", ".nodes.csv")
    with open(nodes_path, newline="") as f:
        r = csv.reader(f)
        h = {k: i for i, k in enumerate(next(r))}
        ids, vals = [], []
        for row in r:
            ids.append(int(row[h["node"]]))
            vals.append([num(row[h[c]]) for c in NODE_COLS])
    node_ids = np.array(ids, dtype=np.int64)
    node_vals = np.array(vals, dtype=np.float64)
    with open(rows_path, newline="") as f:
        r = csv.reader(f)
        h = {k: i for i, k in enumerate(next(r))}
        ix = [h[c] for c in ROW_COLS]
        i_node, i_row = h["node"], h["row"]
        chunks, rn, rk, rv = [], [], [], []
        for row in r:
            rn.append(int(row[i_node]))
            rk.append(int(row[i_row]))
            rv.append([num(row[i]) for i in ix])
            if len(rn) == 200_000:  # compact as we go: Python lists cost ~40x
                chunks.append((np.array(rn, np.int64), np.array(rk, np.int32),
                               np.array(rv, np.int32)))
                rn, rk, rv = [], [], []
        if rn:
            chunks.append((np.array(rn, np.int64), np.array(rk, np.int32),
                           np.array(rv, np.int32)))
    np.savez(cache + ".tmp.npz", node_ids=node_ids, node_vals=node_vals,
             row_node=np.concatenate([c[0] for c in chunks]),
             row_k=np.concatenate([c[1] for c in chunks]),
             row_vals=np.concatenate([c[2] for c in chunks]))
    os.replace(cache + ".tmp.npz", cache)
    return cache


def band_of(depth):
    out = np.full(depth.shape, -1, dtype=np.int8)
    for b, (lo, hi) in enumerate(BANDS):
        out[(depth >= lo) & (depth <= hi)] = b
    return out


def load(paths):
    with Pool(min(len(paths), 6)) as pool:  # memory, not cores, is the limit
        caches = pool.map(parse, paths)
    parts = []
    for fi, c in enumerate(caches):
        z = np.load(c)  # every z[...] reads the array again: read each once
        node_vals, node_ids, row_node = z["node_vals"], z["node_ids"], z["row_node"]
        nv = {name: np.ascontiguousarray(node_vals[:, i]) for i, name in enumerate(NODE_COLS)}
        order = np.argsort(node_ids)
        idx = order[np.searchsorted(node_ids, row_node, sorter=order)]
        parts.append((fi, nv, idx, z["row_k"], z["row_vals"]))
    return parts


def clip(x):
    return np.clip(x, -CLIP, CLIP) / 100.0


def features(nv, idx, rv):
    """Inputs at the decision before move k (row k): node context gathered to rows."""
    g = {k: v[idx] for k, v in nv.items()}
    alpha_now = rv["alpha"]
    best = rv["best_before"]
    second = rv["second_before"]
    has_second = second != NONE
    tt_hit = g["tt_hit"] > 0
    cols = {
        "depth": g["depth"], "ply": g["ply"] / 10, "parent_reduction": g["parent_reduction"],
        "parent_provisional": g["parent_provisional"], "extensions": g["extensions"],
        "pv": (g["node_type"] == 0) * 1.0, "cut": (g["node_type"] == 1) * 1.0,
        "window": clip(g["beta"] - g["alpha"]),
        "eval_alpha": clip(g["eval"] - alpha_now), "static_alpha": clip(g["static_eval"] - alpha_now),
        "raw_alpha": clip(g["raw_eval"] - alpha_now), "correction": clip(g["correction"]),
        "improving": g["improving"], "in_check": g["in_check"], "threat": g["threat"],
        "tt_hit": g["tt_hit"], "tt_depth_rel": np.where(tt_hit, g["tt_depth"] - g["depth"], 0) / 4,
        "tt_lower": (tt_hit & (g["tt_bound"] == 2)) * 1.0, "tt_upper": (tt_hit & (g["tt_bound"] == 1)) * 1.0,
        "tt_score_alpha": np.where(tt_hit, clip(g["tt_score"] - alpha_now), 0),
        "tt_move": g["tt_move"], "phase": g["phase"] / 24, "material": clip(g["material"]),
        "halfmove": g["halfmove"] / 50, "parent_index": np.log1p(g["parent_index"]),
        "parent_quiet": g["parent_quiet"], "parent_history": g["parent_history"] / 8000,
        "legal": g["legal"] / 20, "legal_captures": g["legal_captures"] / 5,
        "remaining": np.maximum(g["legal"] - rv["index"], 0) / 20,
        "log_index": np.log1p(rv["index"]), "searched_before": np.log1p(rv["searched_before"]),
        "pruned_lmp": np.log1p(rv["pruned_lmp"]), "pruned_history": np.log1p(rv["pruned_history"]),
        "pruned_futility": np.log1p(rv["pruned_futility"]),
        "pruned_see": np.log1p(rv["pruned_see_quiet"] + rv["pruned_see_capture"]),
        "lmp_skipping": rv["lmp_skipping"], "promotion": (rv["promotion"] > 0) * 1.0,
        "capture": rv["capture"], "gives_check": rv["gives_check"], "killer": rv["killer"],
        "hash_move": rv["hash_move"], "see": clip(rv["see"]), "history": rv["history"] / 8000,
        "capture_history": rv["capture_history"] / 8000, "lmr": rv["lmr_reduction"] / 1024,
        "alpha_raised": clip(alpha_now - g["alpha"]), "best_alpha": clip(best - alpha_now),
        "best_beta": clip(best - g["beta"]),
        "second_gap": np.where(has_second, clip(best - second), 0), "has_second": has_second * 1.0,
        "near_best": np.log1p(rv["near_best"]), "raised_before": np.log1p(rv["raised_before"]),
        "researches_before": np.log1p(rv["researches_before"]),
        "nodes_before": np.log1p(rv["nodes_before"]),
        "nodes_per_move": np.log1p(rv["nodes_before"] / np.maximum(rv["searched_before"], 1)),
    }
    for s in range(1, 8):
        cols[f"stage{s}"] = (rv["stage"] == s) * 1.0
    for p in range(6):
        cols[f"piece{p}"] = ((rv["piece"] % 8) == p) * 1.0
    names = list(cols)
    return names, np.column_stack([cols[n] for n in names]).astype(np.float64)


def fit_logistic(X, y, l2=1e-4):
    X = X.astype(np.float64)
    mu, sd = X.mean(0), X.std(0)
    sd[sd == 0] = 1
    Z = (X - mu) / sd
    Z = np.column_stack([Z, np.ones(len(Z))])
    n = len(y)

    def f(w):
        t = Z @ w
        loss = np.logaddexp(0, t).sum() - y @ t
        p = 1 / (1 + np.exp(-t))
        grad = Z.T @ (p - y)
        reg = l2 * n * 0.5 * (w[:-1] @ w[:-1])
        grad[:-1] += l2 * n * w[:-1]
        return (loss + reg) / n, grad / n

    w = minimize(f, np.zeros(Z.shape[1]), jac=True, method="L-BFGS-B",
                 options={"maxiter": 500}).x
    return mu, sd, w


def predict(model, X, chunk=1_000_000):
    mu, sd, w = model
    out = np.empty(len(X))
    for s in range(0, len(X), chunk):
        t = ((X[s:s + chunk] - mu) / sd) @ w[:-1] + w[-1]
        out[s:s + chunk] = 1 / (1 + np.exp(-t))
    return out


def auc(p, y):
    order = np.argsort(p)
    ranks = np.empty(len(p))
    ranks[order] = np.arange(1, len(p) + 1)
    npos = y.sum()
    nneg = len(y) - npos
    return (ranks[y == 1].sum() - npos * (npos + 1) / 2) / max(npos * nneg, 1)


def policy_curve(seg, p, y, rest):
    """Stop each node at its first decision with p < tau. Returns, as tau grows,
    the thresholds and cumulative (misses, saved): rows are sorted by node then k."""
    nseg = seg.max() + 1
    q = p + 2.0 * (nseg - seg)  # each later node starts below every earlier value
    pm = np.minimum.accumulate(q)
    first = np.r_[True, seg[1:] != seg[:-1]]
    rec = first | (q < np.r_[np.inf, pm[:-1]])  # a new prefix minimum
    ri = np.flatnonzero(rec)
    # Within a node, the record after record j (later k, lower p) is the stop
    # tau moves away from when it crosses p_j.
    nxt = np.r_[ri[1:], -1]
    same = np.r_[seg[ri[1:]] == seg[ri[:-1]], False]
    d_saved = rest[ri] - np.where(same, rest[nxt], 0)
    d_miss = y[ri] - np.where(same, y[nxt], 0)
    order = np.argsort(p[ri], kind="stable")
    return p[ri][order], np.cumsum(d_miss[order]), np.cumsum(d_saved[order])


def tau_for(curve, max_miss):
    taus, miss, _ = curve
    ok = np.flatnonzero(miss <= max_miss)
    if len(ok) == 0:
        return 0.0
    i = ok[-1]
    return np.nextafter(taus[i], 1.0)


def apply_tau(curve, tau):
    taus, miss, saved = curve
    i = np.searchsorted(taus, tau, side="left") - 1
    return (0, 0) if i < 0 else (miss[i], saved[i])


def ceiling(seg, y, rest, nodes, m):
    """Perfect foresight: each node stops at its first safe k; m nodes may stop
    after the first move instead, where that saves the most."""
    first = np.r_[True, seg[1:] != seg[:-1]]
    rest_first = rest[first]
    safe_rest = np.zeros(nodes)
    safe = y == 0
    sidx = np.flatnonzero(safe)
    s_first = np.r_[True, seg[sidx[1:]] != seg[sidx[:-1]]] if len(sidx) else np.array([], bool)
    safe_rest[seg[sidx[s_first]]] = rest[sidx[s_first]]
    extra = np.sort(np.maximum(rest_first - safe_rest, 0))[::-1]
    return safe_rest.sum() + extra[:m].sum()


def baseline_curve(seg, index, y, rest, nodes):
    """Stop at the first decision with move index >= t; per t: (misses, saved)."""
    out = []
    for t in range(1, MAX_INDEX + 1):
        ok = index >= t
        oi = np.flatnonzero(ok)
        if len(oi) == 0:
            out.append((0, 0))
            continue
        f = np.r_[True, seg[oi[1:]] != seg[oi[:-1]]]
        stop = oi[f]
        out.append((y[stop].sum(), rest[stop].sum()))
    return out


def main():
    paths = sorted(p for a in sys.argv[1:] for p in glob.glob(a))
    if not paths:
        print(__doc__)
        return
    parts = load(paths)
    col = {c: i for i, c in enumerate(ROW_COLS)}
    rng = np.random.default_rng(1)
    names = None
    for b, (lo, hi) in enumerate(BANDS):
        label = f"{lo}-{hi}" if hi < 99 else f"{lo}+"
        acc = {"train": [], "test": []}
        rows_b = sum(int(((row_k >= 1) & (band_of(nv["depth"][idx]) == b)).sum())
                     for _, nv, idx, row_k, _ in parts)
        keep_p = min(1.0, TRAIN_KEEP / max(0.75 * rows_b, 1))
        for fi, nv, idx, row_k, rvals in parts:
            node = idx
            sel = (row_k >= 1) & (band_of(nv["depth"][node]) == b)
            if not sel.any():
                continue
            node = node[sel]
            sub = rvals[sel].astype(np.float64)
            rv = {c: sub[:, i] for c, i in col.items()}
            names, X = features(nv, node, rv)
            X = X.astype(np.float32)
            y = (rv["later_raise_index"] >= 0).astype(np.float64)
            held = ((fi + nv["game"][node]) % 4) == 3  # by game: one in four held out
            kept = rng.random(len(nv["depth"]))[node] < keep_p  # training nodes, whole
            for split, m in (("train", ~held & kept), ("test", held)):
                if m.any():
                    acc[split].append((node[m], X[m], y[m], rv["rest_nodes"][m],
                                       nv["nodes_total"][node[m]], rv["index"][m]))
        sets = {}
        for split, items in acc.items():
            if not items:
                continue
            segs, offset = [], 0
            for node, *_ in items:
                # Rows are grouped by node (in finishing order, not by id).
                first = np.r_[True, node[1:] != node[:-1]]
                seg = np.cumsum(first) - 1
                segs.append(seg + offset)
                offset += seg[-1] + 1
            seg = np.concatenate(segs)
            first = np.r_[True, seg[1:] != seg[:-1]]
            cat = [np.concatenate([it[j] for it in items]) for j in range(1, 6)]
            X, y, rest, total, index = cat
            sets[split] = (seg, X, y, rest, total[first], index)
        acc = items = None  # the concatenated copies are all we need
        if len(sets) < 2:
            continue
        if b == 0:
            print(f"{len(paths)} files; inputs: {len(names)} columns")
        seg, X, y, rest, totals, index = sets["train"]
        nodes = seg.max() + 1
        keep = np.ones(len(y), bool)
        if len(y) > TRAIN_ROWS:  # subsample whole nodes for the fit
            chosen = rng.random(nodes) < TRAIN_ROWS / len(y)
            keep = chosen[seg]
        model = fit_logistic(X[keep], y[keep])
        p_tr = predict(model, X)
        curve_tr = policy_curve(seg, p_tr, y, rest)
        base_tr = baseline_curve(seg, index, y, rest, nodes)
        tseg, tX, ty, trest, ttot, tindex = sets["test"]
        tnodes = tseg.max() + 1
        p_te = predict(model, tX)
        curve_te = policy_curve(tseg, p_te, ty, trest)
        base_te = baseline_curve(tseg, tindex, ty, trest, tnodes)
        tt = ttot.sum()
        print(f"\ndepth {label}: train {nodes} nodes / {len(y)} decisions, held out "
              f"{tnodes} nodes / {len(ty)} decisions; a later change at {ty.mean() * 100:.1f}% "
              f"of decisions; AUC held out {auc(p_te, ty):.3f} (train {auc(p_tr, y):.3f})")
        print(f"  ceiling, no misses: {ceiling(tseg, ty, trest, tnodes, 0) / tt * 100:.1f}%")
        for mr in MISS_RATES:
            ceil = ceiling(tseg, ty, trest, tnodes, int(mr * tnodes)) / tt
            tau = tau_for(curve_tr, mr * nodes)
            miss, saved = apply_tau(curve_te, tau)
            t_b = next((t for t in range(1, MAX_INDEX + 1)
                        if base_tr[t - 1][0] <= mr * nodes and base_tr[t - 1][1] > 0), None)
            if t_b:
                bm, bs = base_te[t_b - 1]
                btxt = f"index {t_b}: {bs / tt * 100:.1f}% ({bm / tnodes * 100:.2f}% misses)"
            else:
                btxt = "none"
            share = saved / tt / ceil * 100 if ceil > 0 else 0
            print(f"  {mr * 100:.1f}% misses: model {saved / tt * 100:.1f}% "
                  f"({miss / tnodes * 100:.2f}% misses held out, {share:.0f}% of the ceiling "
                  f"{ceil * 100:.1f}%); baseline {btxt}")
        mu, sd, w = model
        top = np.argsort(-np.abs(w[:-1]))[:10]
        print("  largest weights (standardized): "
              + ", ".join(f"{names[i]} {w[i]:+.2f}" for i in top))


if __name__ == "__main__":
    main()
