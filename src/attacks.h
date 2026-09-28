#pragma once

#include "types.h"
#include <cstddef>

// Precomputed attack tables.
//
// Leapers (pawn, knight, king) use plain 64-entry lookup tables. Sliders
// (bishop, rook, queen) use "fancy" magic bitboards: the relevant blockers
// are masked out of the occupancy, multiplied by a per-square magic number,
// and the top bits of the product index directly into a precomputed table
// of attack sets.
//
// init_attacks() must run before any lookup. It is idempotent and is called
// from Position's constructor, so any code holding a Position is safe.

void init_attacks();

struct Magic {
    Bitboard mask = 0;              // relevant blocker squares (board edges excluded)
    Bitboard magic = 0;
    const Bitboard* table = nullptr;
    unsigned shift = 0;             // 64 - popcount(mask)

    std::size_t index(Bitboard occupied) const {
        return static_cast<std::size_t>(((occupied & mask) * magic) >> shift);
    }
};

namespace attack_tables {
extern Bitboard pawn[2][64];        // squares attacked by a pawn of the given color
extern Bitboard knight[64];
extern Bitboard king[64];
extern Bitboard between[64][64];    // squares strictly between two aligned squares, else 0
extern Bitboard line[64][64];       // full board line through two aligned squares, else 0
extern Magic bishop_magics[64];
extern Magic rook_magics[64];
}

inline Bitboard pawn_attacks(Color c, int square) {
    return attack_tables::pawn[static_cast<int>(c)][square];
}
inline Bitboard knight_attacks(int square) { return attack_tables::knight[square]; }
inline Bitboard king_attacks(int square) { return attack_tables::king[square]; }

inline Bitboard bishop_attacks(int square, Bitboard occupied) {
    const Magic& m = attack_tables::bishop_magics[square];
    return m.table[m.index(occupied)];
}
inline Bitboard rook_attacks(int square, Bitboard occupied) {
    const Magic& m = attack_tables::rook_magics[square];
    return m.table[m.index(occupied)];
}
inline Bitboard queen_attacks(int square, Bitboard occupied) {
    return bishop_attacks(square, occupied) | rook_attacks(square, occupied);
}

inline Bitboard squares_between(int a, int b) { return attack_tables::between[a][b]; }
inline Bitboard line_through(int a, int b) { return attack_tables::line[a][b]; }
