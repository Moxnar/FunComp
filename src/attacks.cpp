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

// Magic numbers found once by the search below (xorshift64* from the seeds
// in init_attacks) and stored, since the search took a third of a second
// at every start. Each is still checked while filling its table.
constexpr std::uint64_t ROOK_MAGICS[64] = {
    0x8080002040008010ULL, 0x0240100020004000ULL, 0x0200200a00108040ULL, 0x0200100822000440ULL,
    0x06000a0004600810ULL, 0x0980020001140080ULL, 0x040004029008110eULL, 0x0200004100802402ULL,
    0x0080800040008020ULL, 0x0050400040201000ULL, 0x0004802001100080ULL, 0x2810801002880180ULL,
    0x1300800800040080ULL, 0x5802001004020008ULL, 0x0005000402000100ULL, 0x0202000061008a04ULL,
    0x1010820021004200ULL, 0x500140c010002001ULL, 0x0000410010200101ULL, 0x0900220010084200ULL,
    0x8001010004100800ULL, 0x0a02008004008002ULL, 0x0400040010020108ULL, 0x01004a0000910044ULL,
    0x0200400080008030ULL, 0x0000400040201000ULL, 0x0a41004100102000ULL, 0x1040080080100080ULL,
    0x0008008080040008ULL, 0x1414020080800400ULL, 0x0414888400010210ULL, 0x0008004200008104ULL,
    0x0242004082002100ULL, 0x2400201000400040ULL, 0x0006200841001100ULL, 0x080240100a002200ULL,
    0x0124004008080080ULL, 0x8404004100400200ULL, 0xa401000401000200ULL, 0x4020042042001081ULL,
    0x0030804000208000ULL, 0x3010002002444010ULL, 0x000a002080120040ULL, 0x8808008030028048ULL,
    0x080c000802808004ULL, 0x0045000204010008ULL, 0x8000020110040008ULL, 0x320002824402002dULL,
    0xa800800220c01280ULL, 0x1404400084200480ULL, 0x4021001020004100ULL, 0x0200800800100080ULL,
    0x498c000800048080ULL, 0x0300020004008080ULL, 0x0004411088020400ULL, 0x4080010084004200ULL,
    0x04081a8000210143ULL, 0x1600400080110021ULL, 0x0000401008200501ULL, 0x0414081001002005ULL,
    0x0081000800500205ULL, 0x40070002181c0005ULL, 0x0c04103802010084ULL, 0x2000082400830942ULL,
};
constexpr std::uint64_t BISHOP_MAGICS[64] = {
    0x6210040108003500ULL, 0x0011100080808801ULL, 0x00d000a0a1010200ULL, 0x8104440280462000ULL,
    0x6102121008080000ULL, 0x2888411010400120ULL, 0x0014108219200a02ULL, 0x209a620104202680ULL,
    0x0000c012044c00a0ULL, 0x1400040104190201ULL, 0x1008080828588818ULL, 0x0811022082000000ULL,
    0x0205108820002820ULL, 0x0120020924205050ULL, 0x0003690410040420ULL, 0x6dc0020044044420ULL,
    0x000802a008100094ULL, 0x0210000204180086ULL, 0x8802010424040008ULL, 0x0008881802004042ULL,
    0x00c4000088a00100ULL, 0x0200800040602010ULL, 0x0a01032201012000ULL, 0x0003015280880102ULL,
    0x0110248110241084ULL, 0x0050024410242902ULL, 0x0806280090004140ULL, 0x0020200802008008ULL,
    0x8000840002020204ULL, 0x0000820208221000ULL, 0x1000890804880881ULL, 0x0800409005040101ULL,
    0x08016008a0101000ULL, 0x0288088202082220ULL, 0x0208220100480802ULL, 0x4848600802030105ULL,
    0x1020008480040020ULL, 0x0010060200e22080ULL, 0x2008410120104802ULL, 0x3014010a14402080ULL,
    0x00112c6260204023ULL, 0xa0020829140c0882ULL, 0x2002010041180800ULL, 0x0100a24208020080ULL,
    0x2000084104000040ULL, 0x0049030106020301ULL, 0x0864501086002901ULL, 0xd641850e00800202ULL,
    0x0280821010040800ULL, 0x1001042101280090ULL, 0x8014420100881182ULL, 0x802c2080208802a0ULL,
    0x000044102048481fULL, 0x224040388800c000ULL, 0x005dc90801040008ULL, 0x010801114c010152ULL,
    0x2010120082201005ULL, 0x0202082082101002ULL, 0x0400001600840417ULL, 0x0808002520460800ULL,
    0x001008c020020491ULL, 0x0004019021012108ULL, 0x000110c401080214ULL, 0x0523040402840b00ULL,
};

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
                 const Dir (&dirs)[4], const std::uint64_t (&stored)[64],
                 std::uint64_t seed) {
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

        // Find a magic that maps every subset to a slot without a
        // destructive collision (two subsets may share a slot only if their
        // attack sets are identical): the stored one, else by search.
        bool try_stored = true;
        for (bool found = false; !found;) {
            if (try_stored) {
                m.magic = stored[s];
                try_stored = false;
            } else {
                assert(!"stored magic failed");
                m.magic = rng.sparse();
                if (std::popcount((m.mask * m.magic) >> 56) < 6) continue;
            }

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
    init_magics(rook_magics, rook_table, ROOK_TABLE_SIZE, ROOK_DIRS, ROOK_MAGICS,
                0x6a09e667f3bcc909ULL);
    init_magics(bishop_magics, bishop_table, BISHOP_TABLE_SIZE, BISHOP_DIRS, BISHOP_MAGICS,
                0xbb67ae8584caa73bULL);
    init_lines();  // depends on the slider tables
    initialized = true;
}
