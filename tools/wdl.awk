# Eval scale: win/draw rate of the side to move, by the score it reported.
# Reads fastchess PGNs (scores in move comments, like {+0.26/14 0.183s},
# from the mover's point of view) and prints, per 10 cp bucket from -400
# to +400: bucket, positions, win rate, draw rate. Only full moves MINMV to
# MAXMV count, as a stand-in for typical material. Use self-play PGNs only:
# other engines' scores are on their own scales.
#
#   cat matches/1*.pgn | awk -v MINMV=20 -v MAXMV=60 -f tools/wdl.awk
/^\[Result / { res = $2; gsub(/[\]"]/, "", res); next }
/^\[/ { next }
{
  line = $0
  # track move numbers: tokens like "12." (white) or "12..." (black)
  n = split(line, tok, " ")
  for (i = 1; i <= n; i++) {
    t = tok[i]
    if (t ~ /^[0-9]+\.\.\.$/) { mv = t + 0; side = "b"; continue }
    if (t ~ /^[0-9]+\.$/) { mv = t + 0; side = "w"; continue }
    if (t ~ /^\{[-+]?[0-9]+\.[0-9]+\//) {
      s = t; sub(/^\{/, "", s); sub(/\/.*/, "", s); cp = s * 100
      if (mv >= MINMV && mv <= MAXMV && res != "*") {
        r = (res == "1/2-1/2") ? 0.5 : ((res == "1-0") == (side == "w")) ? 1 : 0
        b = int((cp + (cp < 0 ? -5 : 5)) / 10) * 10
        if (b >= -400 && b <= 400) { N[b]++; if (r == 1) W[b]++; else if (r == 0.5) D[b]++ }
      }
      # the next move without a number is black's (after a white move)
      side = (side == "w") ? "b" : "w"; if (side == "w") mv++
      continue
    }
  }
}
END { for (b = -400; b <= 400; b += 10) if (N[b]) printf "%d %d %.4f %.4f\n", b, N[b], W[b]/N[b], D[b]/N[b] }
