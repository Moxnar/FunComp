# Changelog

## FunComp 2.0 (2026-10-02)

The complete classical engine: multi-threaded, with tablebases and
pondering, a richer evaluation, and every parameter tuned.

**Strength.** About **3280 on the CCRL Blitz scale**: 3282 ± 10 from a
single-threaded gauntlet of 2,880 games at 10+0.1 against 24 rated
engines from eight families (Stash, Booot, Reckless, Akimbo, RubiChess,
Seer, Igel, Clover; CCRL Blitz 3049-3419), about 245 above 1.0 measured
the same way. Against 1.0 directly: +306 ± 23 in 1,000 self-play games
from balanced openings at 8+0.08. An estimate on
our hardware, not an official CCRL rating.

**Binaries.**
- `funcomp-2.0-x86-64-v3.exe`: for CPUs with AVX2 and BMI2 (Intel
  Haswell, AMD Zen and later). The faster build.
- `funcomp-2.0-x86-64.exe`: for any 64-bit x86 CPU.

Both search identically (bench 1614718 nodes) and are statically linked.

**New UCI options.** `Threads` (Lazy SMP, 1-256), `Ponder`,
`SyzygyPath`, `SyzygyProbeLimit`, `SyzygyProbeDepth`, and `TelemetryDir`
(for testing). `Hash` and `Move Overhead` as before.

**What's new.**
- *Multi-threaded search* (Lazy SMP): threads share the hash table.
  +151 Elo at 4 threads and +87 at 2 in self-play.
- *Syzygy tablebases* through Fathom: WDL probes in the search, and at
  the root winning moves ranked by distance to zeroing, so won endings
  are converted inside the fifty-move rule.
- *Pondering.*
- *Evaluation:* an initiative term (threats, safe mobility, territory,
  coordination, pawn-push threats, safe checks, king pressure, tempo),
  a drawishness scale for opposite-coloured bishops and close pawnless
  endings, and driving a bare king to the edge (to the right corner for
  bishop and knight).
- *Search:* the halfmove clock in the hash key (so a score from early in
  a fifty-move count isn't reused late), a fix to the prior-countermove
  bonus, late move pruning in the move picker, underpromotion pruning,
  static exchange evaluation of promotions, and time management that
  weighs how much of the search went into the best move, how long it has
  held and whether the score dropped.
- *Tuning:* all of the search's parameters, about 110, tuned together by
  SPSA in two full passes (160,000 games), and the time
  management in a pass of its own at a longer time control.

**Checked before release.**
- A stress run of 8,000 games on both builds (bullet and 40 moves in
  10 s, 24 at a time): no time losses, crashes, disconnects or illegal
  moves; 300 games with both sides pondering, all normal.
- Threads with tablebases in bullet (4 threads, 2+0.02): the stress runs
  found and fixed three problems, none of which changes the search. The
  reported line could start with a helper thread's move; a tablebase
  root's ranking ran before the clock started; and that ranking (by
  distance to zeroing) can take 50 ms on cold tables, so with less than
  a second left the root is now ranked by win, draw or loss alone. After
  the fixes, 2,000 games with 20 search threads on 24 hardware threads:
  no failures.

**Known limitations.**
- No Chess960.
- The evaluation is hand-written; an NNUE comes in 3.0.
- With every hardware thread busy (many threaded games at once), bullet
  games can still be lost on time about once in a thousand, to the
  operating system's scheduling: raise `Move Overhead` in such setups.

**Next.** Chess960 (2.x), then an NNUE trained on FunComp's own games
(3.0), and a learned guide for the search's pruning.

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
