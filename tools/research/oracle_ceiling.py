"""Oracle ceiling for Try 4 (ROADMAP.md, "Continuation logging and oracle ceiling").

Run in WSL on genconlog output (cont_log.h):
    python3 tools/research/oracle_ceiling.py 'matches/contlog-gate/g*.rows.csv'

For every sampled node, a stop before move k (k >= 1: one move searched) is safe
when nothing after it changes the result: the raw signed gain from k on is 0.
Safety is monotone in k, so perfect foresight stops at the earliest safe move
and saves the nodes the rest of the loop cost. Reported per depth band, as a
share of the sampled nodes' own subtree nodes:

  - the ceiling with no misses;
  - with a tolerated miss rate (0.5, 1, 2% of nodes): those nodes may stop
    right after the first move although something later changed, chosen where
    that saves the most;
  - a non-learned baseline: stop at a fixed move index per depth band (the
    smallest index whose miss rate stays within 1%), using only depth and
    index, as LMP does: how much of the ceiling a dumb rule already gets.

The logged rows already come after the search's own pruning, so this is room on
top of it. Nodes are not reweighted by their sampling rates: within one depth
band the rate is nearly constant.
"""

import csv
import glob
import os
import sys
from collections import defaultdict
from multiprocessing import Pool

BANDS = [(3, 4), (5, 6), (7, 9), (10, 99)]
MISS_RATES = [0.005, 0.01, 0.02]
MAX_INDEX = 40


def band_of(depth):
    for lo, hi in BANDS:
        if lo <= depth <= hi:
            return f"{lo}-{hi}" if hi < 99 else f"{lo}+"
    return None


def empty():
    return {"nodes": 0, "total": 0, "safe": 0, "changed": 0, "extra": [],
            "saved": [0] * (MAX_INDEX + 1), "misses": [0] * (MAX_INDEX + 1)}


def add_node(agg, depth, total, rows, window):
    """rows: (index, gain or None, rest_nodes, later_raise_index) per move searched.
    window False: a change is any later gain (the raw signed gain > 0); True: a
    later move raised alpha or failed high (what the parent sees)."""
    b = band_of(depth)
    if b is None or total <= 0 or len(rows) < 2:
        return
    changed = (lambda row: row[3] >= 0) if window else (lambda row: row[1] > 0)
    safe_k = next((k for k in range(1, len(rows)) if not changed(rows[k])), None)
    save_safe = rows[safe_k][2] if safe_k is not None else 0
    save_first = rows[1][2]
    if window:
        last_change = max((rows[k][3] for k in range(1, len(rows)) if rows[k][3] >= 0),
                          default=-1)
    else:
        last_change = max((rows[k][0] for k in range(1, len(rows)) if rows[k][1] > 0),
                          default=-1)
    a = agg[b]
    a["nodes"] += 1
    a["total"] += total
    a["safe"] += save_safe
    a["changed"] += last_change >= 0
    if save_first > save_safe:
        a["extra"].append(save_first - save_safe)
    # Baseline: stop before the first searched move with index >= t; a miss if
    # a move at or after that index changed the result.
    k = 1
    for t in range(1, MAX_INDEX + 1):
        while k < len(rows) and rows[k][0] < t:
            k += 1
        if k == len(rows):
            break
        a["saved"][t] += rows[k][2]
        a["misses"][t] += last_change >= rows[k][0]


def scan(args):
    rows_path, window = args
    nodes_path = rows_path.replace(".rows.csv", ".nodes.csv")
    info = {}
    with open(nodes_path, newline="") as f:
        r = csv.reader(f)
        h = {k: i for i, k in enumerate(next(r))}
        for row in r:
            info[row[h["node"]]] = (int(row[h["depth"]]), int(row[h["nodes_total"]]))
    agg = defaultdict(empty)
    with open(rows_path, newline="") as f:
        r = csv.reader(f)
        h = {k: i for i, k in enumerate(next(r))}
        i_node, i_gain, i_rest, i_idx = h["node"], h["gain"], h["rest_nodes"], h["index"]
        i_raise = h["later_raise_index"]
        cur, rows = None, []
        for row in r:
            node = row[i_node]
            if node != cur:
                if cur is not None:
                    add_node(agg, *info[cur], rows, window)
                cur, rows = node, []
            g = row[i_gain]
            rows.append((int(row[i_idx]), int(g) if g != "" else None, int(row[i_rest]),
                         int(row[i_raise])))
        if cur is not None:
            add_node(agg, *info[cur], rows, window)
    return dict(agg)


def main():
    paths = sorted(p for a in sys.argv[1:] for p in glob.glob(a))
    if not paths:
        print(__doc__)
        return
    for window in (True, False):
        with Pool(min(len(paths), os.cpu_count() or 4)) as pool:
            parts = pool.map(scan, [(p, window) for p in paths])
        print(f"\n{len(paths)} files; a change = "
              + ("a later move raised alpha or failed high" if window
                 else "any later gain (the raw signed gain > 0)"))
        report(parts)


def report(parts):
    for lo, hi in BANDS:
        b = f"{lo}-{hi}" if hi < 99 else f"{lo}+"
        a = empty()
        for part in parts:
            q = part.get(b)
            if not q:
                continue
            for key in ("nodes", "total", "safe", "changed"):
                a[key] += q[key]
            a["extra"] += q["extra"]
            for t in range(MAX_INDEX + 1):
                a["saved"][t] += q["saved"][t]
                a["misses"][t] += q["misses"][t]
        if not a["nodes"]:
            continue
        n, total = a["nodes"], a["total"]
        print(f"depth {b}: {n} nodes; a later move changed the result in "
              f"{a['changed'] / n * 100:.1f}%")
        print(f"  ceiling, no misses: {a['safe'] / total * 100:.1f}% of their nodes")
        extra = sorted(a["extra"], reverse=True)
        for m in MISS_RATES:
            allowed = int(m * n)
            print(f"  ceiling, {m * 100:.1f}% misses: "
                  f"{(a['safe'] + sum(extra[:allowed])) / total * 100:.1f}%")
        best = next(((t, a["saved"][t] / total, a["misses"][t] / n)
                     for t in range(1, MAX_INDEX + 1)
                     if a["misses"][t] <= 0.01 * n and a["saved"][t] > 0), None)
        if best:
            print(f"  baseline, stop at index {best[0]} ({best[2] * 100:.2f}% misses): "
                  f"{best[1] * 100:.1f}%")
        else:
            print(f"  baseline: no index up to {MAX_INDEX} within 1% misses")


if __name__ == "__main__":
    main()
