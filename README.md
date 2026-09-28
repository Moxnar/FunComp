# FunComp

A UCI chess engine written in C++23 from the ground up: a classical
evaluation (PeSTO's tables) and a modern alpha-beta search in which every
feature was proven in games before it was kept. Version 1.0 is estimated
at about **3030 on the CCRL Blitz scale** (3031 ± 14 from a gauntlet of
2,040 games against 17 rated engines from eight families; an estimate on
our hardware, not an official CCRL rating). Single-threaded.

## Features

- **Board:** bitboards with magic sliders, legal move generation,
  make/unmake, Zobrist hashing; move generation verified by perft.
- **Evaluation:** PeSTO's tapered piece-square tables, updated
  incrementally. Correction history (by pawn structure, and by each
  side's non-pawn material) learns during the search how far the static
  eval tends to be off and corrects it; scores shrink toward a draw as
  the fifty-move counter runs.
- **Search:** iterative deepening, principal variation search with
  aspiration windows, quiescence search with SEE pruning.
- **Transposition table:** four-entry buckets with aging; entries
  remember whether the position was on a principal variation.
- **Move ordering:** a staged picker: hash move, winning captures (by
  SEE, victim and capture history), killers, quiet moves (butterfly and
  two continuation histories), losing captures last.
- **Pruning and reductions:** reverse futility, razoring, null move,
  ProbCut, late move pruning, futility, history and SEE pruning, late
  move reductions in fractions of a ply (with deeper and shallower
  re-searches), internal iterative reductions, mate distance pruning.
- **Extensions:** singular extensions (double and negative, with
  multi-cut), and checks that don't lose material.
- **Draws:** repetitions and the fifty-move rule (checkmate on the
  hundredth ply still counts), and detection of a repetition one move
  away.
- **Time management:** soft and hard limits, more time when the best
  move keeps changing or the score drops.

Every change was tested in games (an SPRT against the previous best
version) before it was kept. Ideas that did not pay are still in the
code, switched off, for the tuning pass of the next version. Every margin
and threshold lives in `src/params.h`, and the search reaches them through
`src/pruning.h`, which is built so that a learned model can later guide
the pruning (see "Learned pruning" below).

## Using FunComp

FunComp speaks UCI: load it in any UCI GUI (Arena, Cute Chess, BanksiaGUI,
En Croissant, ...) or match runner (fastchess, cutechess-cli). The release
has two Windows binaries:

- `funcomp-1.0-x86-64-v3.exe` for CPUs with AVX2 and BMI2 (Intel
  Haswell, AMD Zen and later): the faster one;
- `funcomp-1.0-x86-64.exe` for any 64-bit x86 CPU.

Options:

- `Hash` (MB, default 64, 1-4096): transposition table size.
- `Move Overhead` (ms, default 30, 0-5000): time kept in reserve per move
  for GUI and network latency.

Beyond UCI, from a console:

```text
d                     show the board
perft <depth>         count leaf nodes
bench [depth]         search a fixed position set; prints pruning
                      statistics, total nodes and nps
                      (also from the command line: funcomp bench)
pstats                pruning statistics since the last "pstats reset"
pstats reset
genlabels ...         write labels for the learned pruning guide (below)
```

The bench node count is the search's signature (2780319 for 1.0): a
change meant to alter the search changes it, and a refactor or speedup
must not.

## Build

On Windows with MSYS2 (UCRT64), or on Linux with a C++23 compiler and
CMake:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For builds that run on other machines, `-DARCH=` sets the CPU level:
`x86-64` runs on any 64-bit x86 CPU, `x86-64-v3` needs AVX2 and BMI2 and
is faster. Both search identically (same bench). Without it the
compiler's default is used; `-DNATIVE=ON` targets the build machine.

```bash
cmake -S . -B build-x86-64 -DARCH=x86-64
cmake -S . -B build-v3 -DARCH=x86-64-v3
```

`-DTUNE=ON` advertises every search parameter as a UCI option (and
`PruneVerify`, below), for tuning and for tests that change parameters.

## Testing tools

The scripts in `tools/` (PowerShell, plus awk) drive
[fastchess](https://github.com/Disservin/fastchess); they need
`fastchess` on PATH or `-Fastchess <path>`, and opening books (UHO books
for SPRTs, a balanced book such as 8moves_v3 for ratings).

- **`sprt.ps1`**: a sequential probability ratio test of a new build
  against a base, which stops as soon as the result is statistically
  clear. Default bounds `-Elo0 0 -Elo1 5` test for a gain; `-Elo0 -5
  -Elo1 0` for a change that only has to do no harm. `-NewOptions` and
  `-BaseOptions` set parameters on one side (TUNE builds only; the script
  checks).

  ```powershell
  tools\sprt.ps1 -New build\chess.exe -Base base.exe -Book books\UHO_Lichess_4852_v1.epd
  ```

- **`spsa.ps1`**: tunes parameters by SPSA (OpenBench's schedules) with a
  TUNE build playing itself. Parameters, ranges and step sizes come from
  a CSV (`tools/spsa/`); progress goes to a state CSV that also resumes a
  run. Values are rounded stochastically, so integer parameters get a
  gradient too.

  ```powershell
  tools\spsa.ps1 -Engine build-tune\chess.exe -Params tools\spsa\lmr.csv -Iterations 1000 -Book books\UHO_Lichess_4852_v1.epd -State spsa-lmr.csv
  ```

- **`stress.ps1`**: many fast self-play games at once, then a report of
  how games ended, any time losses, crashes, disconnects or illegal
  moves, and how many plies wins took from +10 pawns to mate.
- **`gauntlet.ps1`** and **`rating.awk`**: games against rated opponents
  from a balanced book, one game per physical core (hyper-threading would
  bias an absolute result), and one rating fitted to the PGN. The 1.0
  gauntlet's opponents and their CCRL Blitz ratings are in
  `tools/gauntlet-1.0.txt`, one per line as `file=rating`, optionally
  with `;name=...` (the name in the PGN) and `;option.Name=value`;
  `tools/opponents.sha256` has the checksums of the opponents' official
  binaries.

  ```powershell
  tools\gauntlet.ps1 -Engine funcomp.exe -Opponents (Get-Content tools\gauntlet-1.0.txt) -Rounds 60 -Pgn gauntlet.pgn
  ```

  ```bash
  awk -v ENGINE=FunComp -v RATINGS="stash-25.0=2933,stash-27.0=3049" -f tools/rating.awk gauntlet.pgn
  ```

- **`similarity.ps1`**: the move-similarity (clone) check: how often
  engines choose the same best move on a sample of positions.
- **`wdl.awk`**: the eval's scale, as win and draw rates by reported
  score, from self-play PGNs.

## Learned pruning

The long-term plan: replace hand-written margins with a model that
predicts, from the node's depth, static eval and features, how far the
searched score can land from the static eval. In place today:

1. **Hooks.** `prune::Context` (in `src/pruning.h`) holds the depth, ply,
   static eval, alpha, beta, improving flag, node type (PV, cut or all),
   and for move-level decisions the move, its index, reduced depth,
   history, killer and check flags, plus a slot for a feature vector.
   Setting `prune::guide` to a function installs a model; its answers are
   clamped by the `guide_*` parameters in `params.h`.
2. **Statistics.** `pstats` and `bench` show how often each heuristic is
   tried and fires. In a TUNE build, `PruneVerify` N (0 = off) checks 1 in
   N pruning decisions by searching anyway and counts how often the
   pruning was wrong (for diagnosis only: the checks share the hash table
   and histories with the real search, so they alter it).
3. **Labels.** `genlabels` samples nodes from real searches, records the
   search's context at each, and labels each node with the score of a
   search of it, on its own, to the depth it had in the tree:

   ```text
   genlabels out=labels.csv games=200
   genlabels out=labels.csv in=positions.epd depth=12
   ```

   Options: `out=` (required), `in=` (FEN/EPD file; self-play if absent),
   `games=`, `randomplies=` (random opening moves), `depth=` (driver
   search depth, default 10), `sample=` (1 in N depth-1 nodes, default
   256), `depthscale=` (percent: each ply deeper is sampled this much more
   often, default 200, which roughly evens out the depths), `maxdepth=`
   (deepest node depth sampled, default 8), `count=` (row limit),
   `seed=`, `hash=` (driver, MB), `labelhash=` (labeller, MB; cleared
   before every label so labels don't depend on processing order).

   Columns: `fen, depth, ply, static_eval, correction, search_eval, alpha,
   beta, improving, node, under_null, iir, score, mate`, then the hand
   features of `src/guide_features.h` (phase, material, threatened pieces,
   best safe capture, king attackers, passed pawns, mobility, halfmove
   clock). Scores are from the side to move's view. `static_eval` includes
   correction history, and `correction` is the part that came from it.
   `search_eval` is what pruning decided with: the static eval, or the hash
   score where its bound made it a sharper estimate. `alpha`/`beta` are
   the node's window in the tree, `node` is `pv`, `cut` or `all`. `depth`
   is what node-level pruning saw; `iir` is 1 if the node's moves were then
   searched a ply shallower. Mate scores are clipped to +-2000 and flagged
   in `mate`. The run's options and search parameters are written to
   `<out>.params`.

## Credits

FunComp's code is its own, but its ideas mostly are not: they come from
decades of published work by the chess-programming community.

- **Influences.** The Chess Programming Wiki was the main reference
  throughout. Stockfish influenced FunComp's development more than any
  other engine: many of the search techniques here (singular and negative
  extensions, ProbCut, continuation and correction histories, the ttPv
  flag, upcoming-repetition detection) are ideas we learned from it and
  from other open-source engines. The implementations, their structure
  and their constants are FunComp's own; nothing is copied from another
  engine.
- **Evaluation.** The piece-square tables and material values are
  PeSTO's, by Ronald Friederich, as published on the Chess Programming
  Wiki.
- **Published algorithms.** Static exchange evaluation uses the wiki's
  gain-array swap algorithm; sliders use magic bitboards; move ordering
  values pieces as in Tomasz Michniewski's Simplified Evaluation
  Function; the sampling generator is Sebastiano Vigna's xorshift64*.
- **Testing.** Games are run with fastchess, SPRTs from the UHO opening
  books by Stefan Pohl; the SPSA driver follows OpenBench's schedule. The
  rating gauntlets are played against engines generously published by
  their authors: Stash (Morgan Houppin), Booot (Alex Morozov), Reckless,
  Akimbo (Jamie Whiting), RubiChess (Andreas Matthies), Seer (Connor
  McMonigle), Igel (V. Medvedev, V. Shcherbyna) and Clover (Luca
  Metehau).

## License

FunComp is free software under the GNU General Public License, version 3:
see `LICENSE`.
