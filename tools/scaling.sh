#!/bin/bash
# Thread scaling: nodes per second and depth reached, per thread count, over
# six positions (opening, middlegames, endgames) searched for a fixed time
# each, with a fresh hash table per position. Run on an idle machine: other
# load distorts it.
# Usage: tools/scaling.sh engines/best.exe "1 2 4 8 12 24" [ms per position] [hash MB]
eng=$1; threads=${2:-"1 2 4 8"}; ms=${3:-5000}; hash=${4:-256}
fens=(
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"
    "r4rk1/1b2qppp/1p1b1n2/p1pp4/P2P4/2PBPNB1/2Q2PPP/3R1RK1 w - - 0 16"
    "3r2k1/1p3ppp/p1n5/2b1p3/P2r4/1P1B2N1/2P2PPP/3R1RK1 w - - 5 21"
    "8/5p2/2b1pPk1/3pP1p1/2pP3p/1rN4P/2RK2P1/8 w - - 3 46"
    "1rr5/4k3/p2n3p/1p1BpP1P/8/1RP2K2/1P6/3R4 b - - 0 36"
)
printf "%-8s %12s %9s %9s\n" threads nps speedup depth
base=0
for t in $threads; do
    total_nodes=0; total_ms=0; depth_sum=0
    for fen in "${fens[@]}"; do
        out=$({ echo "setoption name Threads value $t"; echo "setoption name Hash value $hash"
                echo "ucinewgame"; echo "position fen $fen"; echo "go movetime $ms"
                sleep $(( ms / 1000 + 2 )); echo quit; } | "$eng" | grep "^info depth" | tail -1)
        nodes=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i=="nodes") print $(i+1)}')
        time=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i=="time") print $(i+1)}')
        depth=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i=="depth") print $(i+1)}')
        total_nodes=$((total_nodes + nodes)); total_ms=$((total_ms + time)); depth_sum=$((depth_sum + depth))
    done
    nps=$((total_nodes * 1000 / total_ms))
    [ $base -eq 0 ] && base=$nps
    printf "%-8s %12d %9s %9s\n" "$t" "$nps" "$(awk -v a=$nps -v b=$base 'BEGIN{printf "%.2fx", a/b}')" \
        "$(awk -v s=$depth_sum -v n=${#fens[@]} 'BEGIN{printf "%.1f", s/n}')"
done
