#pragma once
#include "position.h"

// Generates exactly the legal moves in `pos`; `count` is set to the number
// written into `moves`. Legality is decided up front from the checkers and
// pinned pieces, so no move is played to test it.
void generate_legal(const Position& pos, MoveList& moves, int& count);

// Returns the number of legal moves without writing them anywhere. Same
// logic as generate_legal, but whole target sets are popcounted. Used for
// bulk counting at perft leaves.
int count_legal(const Position& pos);

// Legal captures only: every capture (en passant and capture-promotions
// included) plus quiet promotions. Used by quiescence search.
void generate_captures(const Position& pos, MoveList& moves, int& count);

// Legal non-captures that aren't promotions, castling included. Together
// with generate_captures this is exactly generate_legal.
void generate_quiets(const Position& pos, MoveList& moves, int& count);
