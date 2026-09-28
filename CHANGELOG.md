# Changelog

## FunComp 1.0 (2026-09-28)

The first public release: a classical engine (PeSTO's tables and a modern
alpha-beta search), single-threaded.

**Strength.** About **3030 on the CCRL Blitz scale**: 3031 ± 14 from a
gauntlet of 2,040 games at 10+0.1 against 17 rated engines from eight
families (Stash, Booot, Reckless, Akimbo, RubiChess, Seer, Igel, Clover;
CCRL Blitz 2933-3345). An estimate on our hardware, not an official CCRL
rating.

**Binaries.**
- `funcomp-1.0-x86-64-v3.exe`: for CPUs with AVX2 and BMI2 (Intel
  Haswell, AMD Zen and later). The faster build.
- `funcomp-1.0-x86-64.exe`: for any 64-bit x86 CPU.

Both search identically (bench 2780319 nodes) and are statically linked.

**UCI options.** `Hash` (MB, default 64), `Move Overhead` (ms, default 30).

**What went into it.** Every search feature was tested by SPRT
against the previous best build before it was kept. Since the
last internal milestone, accepted: double and negative singular
extensions, a flag for positions on principal variations, late move
reductions in fractions of a ply, ProbCut, history pruning, a limit
on quiet evasions in quiescence, correction history by non-pawn
material, fifty-move scaling, and detection of repetitions one move
away. Together these gained +61 ± 15 Elo against that milestone in
1,000 balanced games. Tested ideas that didn't pay remain in the code,
switched off, for the tuning pass of 2.0.

**Checked before release.**
- A stress run of 8,000 games on both builds (bullet and 40 moves in
  10 s, 24 at a time): no time losses, crashes, disconnects or illegal
  moves.
- A robustness pass on the protocol: negative clocks from the GUI, a
  clock with no time left, FENs without move counters, invalid FENs.
- An originality review: FunComp's code is its own. Stockfish and the
  chess-programming community influenced its ideas (see the README's
  credits); a best-move similarity test puts FunComp no closer to
  Stockfish than unrelated engines are to each other.

**Known limitations.**
- Single-threaded (no `Threads` option); no endgame tablebases; no
  Chess960.
- At bullet speeds (about 5 ms a move) a winning side occasionally fails
  to convert before the fifty-move rule (0.3-0.65% of games at 1+0.01).

**Next.** Version 2.0: a full tuning pass and multi-threaded search.
Then Chess960 and an NNUE.
