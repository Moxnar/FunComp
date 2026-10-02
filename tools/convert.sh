#!/bin/bash
# Conversion check: one engine plays both sides from each position in an
# EPD (tools/bare-king.epd: queen, rook, two bishops, bishop and knight
# against a bare king), `go movetime <ms>` per move. Prints, per position,
# the plies to mate, "draw" after 100 plies (the fifty-move rule), or
# "stalemate". fastchess 1.8.2 crashes on such pawnless start positions,
# hence this driver.
# Usage: tools/convert.sh engines/best.exe tools/bare-king.epd 5 ["Name=value;Name=value"]
# The optional fourth argument sets engine options (e.g. SyzygyPath).
eng=$1; epd=$2; mt=$3; opts=$4
coproc E { "$eng"; }
send() { echo "$1" >&${E[1]}; }
waitfor() { local line; while IFS= read -r line <&${E[0]}; do line=${line%$'\r'}; [[ $line == $1* ]] && { REPLY_LINE=$line; return; }; [[ $line == info*score* ]] && LAST_INFO=$line; done; }
send uci; waitfor uciok
IFS=";" read -ra optlist <<< "$opts"
for o in "${optlist[@]}"; do send "setoption name ${o%%=*} value ${o#*=}"; done
while IFS= read -r fen; do
  fen=${fen%$'\r'}; [ -z "$fen" ] && continue
  send ucinewgame; send isready; waitfor readyok
  moves=""; result=draw
  for ((ply=0; ply<100; ply++)); do
    PREV_INFO=$LAST_INFO
    send "position fen $fen moves$moves"; send "go movetime $mt"; LAST_INFO=""; waitfor bestmove
    bm=$(echo "$REPLY_LINE" | awk '{print $2}')
    if [[ $bm == "0000" || $bm == "(none)" ]]; then
      # No legal move: mate if the side to move was mated (odd ply = defender to move... count on score)
      if [[ $PREV_INFO == *"score mate 1 "* ]]; then result=$ply; else result=stalemate; fi
      break
    fi
    moves="$moves $bm"
  done
  echo "$result"
done < "$epd"
send quit
