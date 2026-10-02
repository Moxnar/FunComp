#!/bin/bash
# Sums match telemetry: the files an engine writes at exit when the UCI
# option TelemetryDir is set (one per engine process), into one table.
# Usage: tools/telemetry-sum.sh matches/66-telemetry/new [matches/66-telemetry/base]
# With two directories, prints them side by side (new, base).
sum() {
    cat "$1"/telemetry-*.txt 2>/dev/null | awk '
        { t[$1] += $2; f[$1] += $3; v[$1] += $4; w[$1] += $5; if (!($1 in seen)) { seen[$1] = 1; order[n++] = $1 } }
        END { for (i = 0; i < n; i++) { k = order[i]; print k, t[k], f[k], v[k], w[k] } }'
}
files() { ls "$1"/telemetry-*.txt 2>/dev/null | wc -l; }
if [ -z "$2" ]; then
    echo "$(files "$1") files in $1"
    sum "$1" | awk '{ printf "%-24s %14d %14d %7.2f%%\n", $1, $2, $3, $2 ? 100 * $3 / $2 : 0 }'
else
    echo "new: $(files "$1") files in $1; base: $(files "$2") files in $2"
    printf "%-24s %14s %14s %8s | %14s %14s %8s\n" name tried fired fire% tried fired fire%
    join -j1 <(sum "$1" | sort) <(sum "$2" | sort) | awk '{
        printf "%-24s %14d %14d %7.2f%% | %14d %14d %7.2f%%\n", $1, $2, $3, $2 ? 100 * $3 / $2 : 0,
               $6, $7, $6 ? 100 * $7 / $6 : 0 }'
fi
