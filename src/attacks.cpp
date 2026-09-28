#include "attacks.h"
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>

namespace attack_tables {
Bitboard pawn[2][64];
Bitboard knight[64];
Bitboard king[64];
Bitboard between[64][64];
Bitboard line[64][64];
Magic bishop_magics[64];
Magic rook_magics[64];
}

namespace {
using namespace attack_tables;

// Exact sizes of the shared attack tables: the sum over all squares of
// 2^popcount(relevant mask).
constexpr std::size_t ROOK_TABLE_SIZE = 102400;
constexpr std::size_t BISHOP_TABLE_SIZE = 5248;
Bitboard rook_table[ROOK_TABLE_SIZE];
Bitboard bishop_table[BISHOP_TABLE_SIZE];

bool initialized = false;

struct Dir { int df, dr; };
constexpr Dir BISHOP_DIRS[4] = {{1,1},{1,-1},{-1,1},{-1,-1}};
constexpr Dir ROOK_DIRS[4] = {{1,0},{-1,0},{0,1},{0,-1}};

constexpr Bitboard RANK_1 = 0x00000000000000FFULL;
constexpr Bitboard RANK_8 = 0xFF00000000000000ULL;
constexpr Bitboard FILE_A = 0x0101010101010101ULL;
constexpr Bitboard FILE_H = 0x8080808080808080ULL;

// Reference slider attacks by ray walking. Used only during initialization.
Bitboard sliding_attacks(int square, Bitboard occupied, const Dir (&dirs)[4]) {
    Bitboard attacks = 0;
    for (const Dir& d : dirs) {
        int f = file_of(square) + d.df, r = rank_of(square) + d.dr;
        while (f >= 0 && f < 8 && r >= 0 && r < 8) {
            const int s = sq(f, r);
            attacks |= bit(s);
            if (occupied & bit(s)) break;
            f += d.df; r += d.dr;
        }
    }
    return attacks;
}

// Deterministic xorshift64* generator, so magic search is reproducible.
struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1DULL;
    }
    // Magic candidates with few set bits are far more likely to work.
    std::uint64_t sparse() { return next() & next() & next(); }
};

void init_magics(Magic (&magics)[64], Bitboard* table, std::size_t table_size,
                 const Dir (&dirs)[4], std::uint64_t seed) {
    Rng rng{seed};
    std::array<Bitboard, 4096> occupancies{};
    std::array<Bitboard, 4096> reference{};
    std::array<int, 4096> epoch{};  // avoids clearing the table between attempts
    int attempt = 0;
    std::size_t used = 0;

    for (int s = 0; s < 64; ++s) {
        // Edge squares never block anything further along the ray, so they
        // are left out of the mask (unless the slider sits on that edge).
        const Bitboard edges = ((RANK_1 | RANK_8) & ~(RANK_1 << (8 * rank_of(s)))) |
                               ((FILE_A | FILE_H) & ~(FILE_A << file_of(s)));

        Magic& m = magics[s];
        m.mask = sliding_attacks(s, 0, dirs) & ~edges;
        const int bits = std::popcount(m.mask);
        m.shift = static_cast<unsigned>(64 - bits);
        m.table = table + used;
        Bitboard* entries = table + used;

        // Enumerate every subset of the mask (Carry-Rippler trick).
        int size = 0;
        Bitboard b = 0;
        do {
            occupancies[static_cast<std::size_t>(size)] = b;
            reference[static_cast<std::size_t>(size)] = sliding_attacks(s, b, dirs);
            ++size;
            b = (b - m.mask) & m.mask;
        } while (b);

        // Search for a magic that maps every subset to a slot without a
        // destructive collision (two subsets may share a slot only if their
        // attack sets are identical).
        for (bool found = false; !found;) {
            m.magic = rng.sparse();
            if (std::popcount((m.mask * m.magic) >> 56) < 6) continue;

            ++attempt;
            found = true;
            for (int i = 0; i < size; ++i) {
                const std::size_t idx = m.index(occupancies[static_cast<std::size_t>(i)]);
                const Bitboard attacks = reference[static_cast<std::size_t>(i)];
                if (epoch[idx] < attempt) {
                    epoch[idx] = attempt;
                    entries[idx] = attacks;
                } else if (entries[idx] != attacks) {
                    found = false;
                    break;
                }
            }
        }
        used += static_cast<std::size_t>(size);
    }
    assert(used == table_size);
    (void)table_size;
}

void init_leapers() {
    constexpr int knight_df[] = {1,2,2,1,-1,-2,-2,-1};
    constexpr int knight_dr[] = {2,1,-1,-2,-2,-1,1,2};
    constexpr int king_df[] = {1,1,1,0,0,-1,-1,-1};
    constexpr int king_dr[] = {1,0,-1,1,-1,1,0,-1};

    for (int s = 0; s < 64; ++s) {
        const int f = file_of(s), r = rank_of(s);
        auto on_board = [](int nf, int nr) { return nf >= 0 && nf < 8 && nr >= 0 && nr < 8; };

        knight[s] = 0;
        king[s] = 0;
        for (int i = 0; i < 8; ++i) {
            if (on_board(f + knight_df[i], r + knight_dr[i]))
                knight[s] |= bit(sq(f + knight_df[i], r + knight_dr[i]));
            if (on_board(f + king_df[i], r + king_dr[i]))
                king[s] |= bit(sq(f + king_df[i], r + king_dr[i]));
        }

        pawn[0][s] = pawn[1][s] = 0;
        for (int df : {-1, 1}) {
            if (on_board(f + df, r + 1)) pawn[0][s] |= bit(sq(f + df, r + 1));
            if (on_board(f + df, r - 1)) pawn[1][s] |= bit(sq(f + df, r - 1));
        }
    }
}

void init_lines() {
    for (int a = 0; a < 64; ++a) {
        for (int b = 0; b < 64; ++b) {
            between[a][b] = line[a][b] = 0;
            if (a == b) continue;
            if (bishop_attacks(a, 0) & bit(b)) {
                line[a][b] = (bishop_attacks(a, 0) & bishop_attacks(b, 0)) | bit(a) | bit(b);
                between[a][b] = bishop_attacks(a, bit(b)) & bishop_attacks(b, bit(a));
            } else if (rook_attacks(a, 0) & bit(b)) {
                line[a][b] = (rook_attacks(a, 0) & rook_attacks(b, 0)) | bit(a) | bit(b);
                between[a][b] = rook_attacks(a, bit(b)) & rook_attacks(b, bit(a));
            }
        }
    }
}
}

void init_attacks() {
    if (initialized) return;
    init_leapers();
    init_magics(rook_magics, rook_table, ROOK_TABLE_SIZE, ROOK_DIRS, 0x6a09e667f3bcc909ULL);
    init_magics(bishop_magics, bishop_table, BISHOP_TABLE_SIZE, BISHOP_DIRS, 0xbb67ae8584caa73bULL);
    init_lines();  // depends on the slider tables
    initialized = true;
}
