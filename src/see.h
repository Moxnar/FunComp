#pragma once
#include "position.h"

// Static exchange evaluation: the material outcome of the sequence of
// captures on a move's destination square, each side recapturing with its
// least valuable attacker and free to stop whenever continuing would lose
// material. X-ray attackers behind the capturing pieces join in as the
// pieces in front leave. Pins are ignored, as usual.

// Piece values for exchanges, indexed by PieceType (king last, never
// captured). Close to the search's ordering values rather than PeSTO's
// tapered ones, so thresholds read as rough centipawns.
inline constexpr int SEE_VALUE[6] = {100, 320, 330, 500, 900, 0};

// True if the exchange started by `m` wins at least `threshold`
// centipawns for the side making it. Castling, en passant and promotions
// are treated as winning exactly 0.
bool see_at_least(const Position& pos, Move m, int threshold);
