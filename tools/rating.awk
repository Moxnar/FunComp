# Rating estimate from a gauntlet PGN.
#
#   awk -v ENGINE=FunComp -v RATINGS="stash-15.3=2173,stash-17.0=2297" \
#       -f tools/rating.awk matches/gauntlet.pgn
#
# For each opponent: games, wins/draws/losses of ENGINE, score, and the
# rating that score implies (opponent's rating plus the logistic Elo
# difference). Then a combined estimate: the single rating whose expected
# scores against all opponents add up to the actual total (maximum
# likelihood under the logistic model), with a rough 95% interval from the
# game-level variance.

BEGIN {
    n = split(RATINGS, pairs, ",")
    for (i = 1; i <= n; i++) {
        split(pairs[i], kv, "=")
        rating[kv[1]] = kv[2] + 0
        order[i] = kv[1]
    }
}

/^\[White "/ { white = $0; sub(/^\[White "/, "", white); sub(/"\]$/, "", white) }
/^\[Black "/ { black = $0; sub(/^\[Black "/, "", black); sub(/"\]$/, "", black) }
/^\[Result "/ {
    res = $0; sub(/^\[Result "/, "", res); sub(/"\]$/, "", res)
    if (res == "*") next
    if (white == ENGINE) { opp = black; s = res == "1-0" ? 1 : res == "0-1" ? 0 : 0.5 }
    else if (black == ENGINE) { opp = white; s = res == "0-1" ? 1 : res == "1-0" ? 0 : 0.5 }
    else next
    games[opp]++; points[opp] += s; sq[opp] += s * s
    if (s == 1) wins[opp]++; else if (s == 0) losses[opp]++; else draws[opp]++
}

function expected(r, ro) { return 1 / (1 + 10 ^ ((ro - r) / 400)) }

END {
    printf "%-12s %6s %5s %5s %5s %5s %7s %9s\n", "opponent", "rating", "games", "W", "D", "L", "score", "implied"
    total_games = 0; total_points = 0; var = 0
    for (i = 1; i <= n; i++) {
        o = order[i]
        if (!games[o]) continue
        sc = points[o] / games[o]
        c = sc; if (c < 0.01) c = 0.01; if (c > 0.99) c = 0.99
        implied = rating[o] - 400 * log(1 / c - 1) / log(10)
        printf "%-12s %6d %5d %5d %5d %5d %6.1f%% %9.0f\n", o, rating[o], games[o], wins[o] + 0, draws[o] + 0, losses[o] + 0, 100 * sc, implied
        total_games += games[o]; total_points += points[o]
        var += sq[o] - points[o] * points[o] / games[o]   # sum of squared deviations
    }
    # Solve sum_o games[o] * expected(R, rating[o]) = total_points by bisection.
    lo = 0; hi = 4000
    for (it = 0; it < 60; it++) {
        mid = (lo + hi) / 2; e = 0
        for (i = 1; i <= n; i++) if (games[order[i]]) e += games[order[i]] * expected(mid, rating[order[i]])
        if (e < total_points) lo = mid; else hi = mid
    }
    R = (lo + hi) / 2
    # Standard error: sd(points) / d(expected total)/dR.
    slope = 0
    for (i = 1; i <= n; i++) {
        o = order[i]; if (!games[o]) continue
        p = expected(R, rating[o]); slope += games[o] * p * (1 - p) * log(10) / 400
    }
    se = sqrt(var) / slope
    printf "\ncombined: %d games, %.1f points (%.1f%%)\n", total_games, total_points, 100 * total_points / total_games
    printf "estimate: %.0f +- %.0f (95%%), on the CCRL Blitz scale\n", R, 1.96 * se
}
