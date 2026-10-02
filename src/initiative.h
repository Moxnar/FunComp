#pragma once
#include "position.h"
#include <iosfwd>

// Initiative: who is asking the questions.
//
// A small, bitboard-only term added to the PeSTO score. PeSTO says what the
// position is worth if nothing happens; this says who is making things
// happen: whose pieces have room, press into the other half, outnumber the
// defence of the same targets, threaten something, can give a safe check,
// or are converging on the king.
//
// Everything is measured for both colours and combined from the side to
// move's point of view, so the term is colour-symmetric (a position and its
// colour-flipped mirror score the same) and the engine values the
// opponent's initiative exactly as it values its own: it seeks it and
// denies it. The one deliberate asymmetry is between the side to move and
// the side that just moved:
//
// - Threats against the side to move are pending: that side must spend its
//   move on them. The largest costs a tempo; the second largest is a double
//   attack one move can't meet, weighted far more heavily. This is "making
//   the opponent respond" as a number, and what a stand-pat score can't see.
// - Threats by the side to move are latent: quiescence resolves the captures
//   among them, so they get a small weight.
// - A flat tempo bonus for the side to move.
//
// Persistence (does the initiative survive the reply?) is left to the
// search: a network trained on searched scores of this evaluation learns
// the initiative that survives.
//
// Weights are in params.h (Init*), all tunable; InitScale scales the whole
// term (percent; 0 skips the computation entirely).
namespace initiative {

// Centipawns from the side to move's point of view: the weighted sum,
// clamped to +-InitClamp, times InitScale percent.
int evaluate(const Position& pos);

// Every feature and its contribution, for the "eval" UCI command.
void print_trace(const Position& pos, std::ostream& out);

}  // namespace initiative
