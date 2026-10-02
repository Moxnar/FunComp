#!/bin/bash
# Prints an SPSA run's parameter values as NAME=VALUE lines (rounded), from
# its state CSV (tools/spsa.ps1 -State): the average over the last N
# iterations (N=1: the final point). These lines are what sprt.ps1's
# -NewOptions takes and what tools/spsa/apply-values.sh writes into params.h.
# Usage: tools/spsa/values.sh matches/spsa-pass2-fine.csv [N]
state=$1
n=${2:-1}
[ -f "$state" ] || { echo "no state file $state" >&2; exit 1; }
rows=$(($(wc -l < "$state") - 1))
[ "$n" -le "$rows" ] || { echo "only $rows iterations in $state" >&2; exit 1; }
awk -F, -v first=$((rows - n + 1)) '
NR == 1 { for (i = 5; i <= NF; i++) name[i] = $i; cols = NF; next }
NR - 1 >= first { for (i = 5; i <= cols; i++) sum[i] += $i; k++ }
END { for (i = 5; i <= cols; i++) { v = sum[i] / k; printf "%s=%d\n", name[i], (v < 0 ? v - 0.5 : v + 0.5) } }
' "$state"
