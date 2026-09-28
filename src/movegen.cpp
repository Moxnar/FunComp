#include "movegen.h"
#include "attacks.h"
#include <bit>

namespace {
enum class GenType { All, Captures, Quiets };

constexpr Bitboard FILE_A = 0x0101010101010101ULL;
constexpr Bitboard FILE_H = 0x8080808080808080ULL;
constexpr Bitboard RANK_1 = 0xFFULL;
constexpr Bitboard RANK_3 = 0xFFULL << 16;
constexpr Bitboard RANK_6 = 0xFFULL << 40;
constexpr Bitboard RANK_8 = 0xFFULL << 56;

inline int pop_lsb(Bitboard& b) {
    const int s = std::countr_zero(b);
    b &= b - 1;
    return s;
}

// Board-relative shifts; positive D moves toward rank 8.
template <int D>
constexpr Bitboard shift(Bitboard b) {
    if constexpr (D > 0) return b << D;
    else return b >> -D;
}

// Pawn directions from color C's point of view. "East" is toward the h-file.
template <Color C> constexpr int UP      = C == Color::White ?  8 : -8;
template <Color C> constexpr int UP_EAST = C == Color::White ?  9 : -7;
template <Color C> constexpr int UP_WEST = C == Color::White ?  7 : -9;

// Set-wise pawn captures. The file mask stops a capture from wrapping
// around the board edge.
template <Color C>
constexpr Bitboard pawn_attacks_east(Bitboard pawns) { return shift<UP_EAST<C>>(pawns & ~FILE_H); }
template <Color C>
constexpr Bitboard pawn_attacks_west(Bitboard pawns) { return shift<UP_WEST<C>>(pawns & ~FILE_A); }

// Collects moves, or in CountOnly mode just counts them. The counting paths
// use popcount on whole target sets, so leaf nodes never touch a move list.
template <bool CountOnly>
struct Sink {
    Move* out;
    int n = 0;

    void add(Move m) {
        if constexpr (!CountOnly) out[n] = m;
        ++n;
    }
    // One piece moving to every square in `to`.
    void add_from(int from, Bitboard to) {
        if constexpr (CountOnly) n += std::popcount(to);
        else while (to) out[n++] = Move(from, pop_lsb(to));
    }
    // Many pawns, each moving by the same fixed offset.
    void add_pawns(Bitboard to, int offset) {
        if constexpr (CountOnly) n += std::popcount(to);
        else while (to) { const int t = pop_lsb(to); out[n++] = Move(t - offset, t); }
    }
    void add_promotions(Bitboard to, int offset) {
        if constexpr (CountOnly) n += 4 * std::popcount(to);
        else while (to) {
            const int t = pop_lsb(to), f = t - offset;
            out[n++] = Move(f, t, PieceType::Queen);
            out[n++] = Move(f, t, PieceType::Rook);
            out[n++] = Move(f, t, PieceType::Bishop);
            out[n++] = Move(f, t, PieceType::Knight);
        }
    }
};

template <Color Us, GenType Type, bool CountOnly>
int generate(const Position& pos, Move* out) {
    // Captures: every capture (including en passant and capture-promotions)
    // plus quiet promotions. Quiets: everything else, including castling.
    constexpr bool Caps = Type != GenType::Quiets;
    constexpr bool Qts = Type != GenType::Captures;
    constexpr Color Them = opposite(Us);
    constexpr int up = UP<Us>;
    constexpr Bitboard promo_rank = Us == Color::White ? RANK_8 : RANK_1;
    constexpr Bitboard third_rank = Us == Color::White ? RANK_3 : RANK_6;

    Sink<CountOnly> sink{out};

    auto piece = [&](Color c, PieceType pt) {
        return pos.pieces(static_cast<Piece>(piece_index(c, pt)));
    };

    const Bitboard ours = pos.occupancy(Us);
    const Bitboard theirs = pos.occupancy(Them);
    const Bitboard occ = pos.occupied();

    const Bitboard king_bb = piece(Us, PieceType::King);
    if (!king_bb) return 0;  // malformed position: treat as having no moves
    const int ksq = std::countr_zero(king_bb);

    const Bitboard their_pawns = piece(Them, PieceType::Pawn);
    const Bitboard their_knights = piece(Them, PieceType::Knight);
    const Bitboard their_diagonal = piece(Them, PieceType::Bishop) | piece(Them, PieceType::Queen);
    const Bitboard their_orthogonal = piece(Them, PieceType::Rook) | piece(Them, PieceType::Queen);

    // Squares a non-pawn move may land on, by move type.
    const Bitboard type_mask = Type == GenType::Captures ? theirs
                             : Type == GenType::Quiets   ? ~occ
                             : ~ours;

    // King moves: only the few candidate squares are tested for attacks,
    // rather than mapping every square the opponent attacks. Our king is
    // lifted off the board, so that a slider checking along a line also
    // covers the square directly behind the king. They are emitted last:
    // move pickers break ties in generation order, and a king move is
    // rarely the one to try first.
    Bitboard king_targets = 0;
    {
        const Bitboard occ_nk = occ ^ king_bb;
        for (Bitboard b = king_attacks(ksq) & type_mask; b;) {
            const int to = pop_lsb(b);
            if (!(pos.attackers_to(to, occ_nk) & theirs)) king_targets |= bit(to);
        }
    }

    const Bitboard checkers = pos.checkers();

    // In double check only the king can move.
    if (checkers & (checkers - 1)) {
        sink.add_from(ksq, king_targets);
        return sink.n;
    }

    // Castling: not while in check, path empty, transit and destination
    // squares unattacked. Not being in check, no slider is aimed at the
    // king, so the ordinary occupancy serves. The rook check guards against
    // FENs whose castling field doesn't match the board.
    if (Qts && !checkers) {
        constexpr int r = Us == Color::White ? 0 : 7;
        constexpr int KS = Us == Color::White ? Position::WK_CASTLE : Position::BK_CASTLE;
        constexpr int QS = Us == Color::White ? Position::WQ_CASTLE : Position::BQ_CASTLE;
        if (ksq == sq(4, r)) {
            const int rights = pos.castling_rights();
            const Bitboard rooks = piece(Us, PieceType::Rook);
            constexpr Bitboard ks_path = bit(sq(5, r)) | bit(sq(6, r));
            constexpr Bitboard qs_path = bit(sq(1, r)) | bit(sq(2, r)) | bit(sq(3, r));
            auto attacked = [&](int s) { return (pos.attackers_to(s, occ) & theirs) != 0; };
            if ((rights & KS) && (rooks & bit(sq(7, r))) && !(occ & ks_path) &&
                !attacked(sq(5, r)) && !attacked(sq(6, r)))
                sink.add(Move(sq(4, r), sq(6, r)));
            if ((rights & QS) && (rooks & bit(sq(0, r))) && !(occ & qs_path) &&
                !attacked(sq(3, r)) && !attacked(sq(2, r)))
                sink.add(Move(sq(4, r), sq(2, r)));
        }
    }

    // In single check, other pieces must capture the checker or block.
    const Bitboard check_mask = checkers
        ? (squares_between(ksq, std::countr_zero(checkers)) | checkers)
        : ~Bitboard{0};

    // Pinned pieces: our piece that is the only blocker between our king and
    // an enemy slider on the same line. It may only move along that line.
    Bitboard pinned = 0;
    {
        Bitboard snipers = (bishop_attacks(ksq, 0) & their_diagonal) |
                           (rook_attacks(ksq, 0) & their_orthogonal);
        while (snipers) {
            const int s = pop_lsb(snipers);
            const Bitboard blockers = squares_between(ksq, s) & occ;
            if (blockers && !(blockers & (blockers - 1)) && (blockers & ours))
                pinned |= blockers;
        }
    }

    // A pinned piece can never resolve a check: it stays on its pin line,
    // which meets the checking line only at the king.
    const Bitboard movable = checkers ? ~pinned : ~Bitboard{0};
    const Bitboard targets = type_mask & check_mask;

    // Knights. A pinned knight can never move.
    for (Bitboard b = piece(Us, PieceType::Knight) & ~pinned; b;) {
        const int from = pop_lsb(b);
        sink.add_from(from, knight_attacks(from) & targets);
    }

    // Sliders. Queens are handled as a bishop plus a rook.
    const Bitboard our_queens = piece(Us, PieceType::Queen);
    for (Bitboard b = (piece(Us, PieceType::Bishop) | our_queens) & movable; b;) {
        const int from = pop_lsb(b);
        Bitboard a = bishop_attacks(from, occ) & targets;
        if (pinned & bit(from)) a &= line_through(ksq, from);
        sink.add_from(from, a);
    }
    for (Bitboard b = (piece(Us, PieceType::Rook) | our_queens) & movable; b;) {
        const int from = pop_lsb(b);
        Bitboard a = rook_attacks(from, occ) & targets;
        if (pinned & bit(from)) a &= line_through(ksq, from);
        sink.add_from(from, a);
    }

    // Pawns (except en passant), all unpinned pawns at once.
    const Bitboard pawns = piece(Us, PieceType::Pawn);
    const Bitboard empty = ~occ;
    {
        const Bitboard free_pawns = pawns & ~pinned;

        const Bitboard single = shift<up>(free_pawns) & empty;
        const Bitboard push = single & check_mask;
        if constexpr (Qts) {
            sink.add_pawns(push & ~promo_rank, up);
            sink.add_pawns(shift<up>(single & third_rank) & empty & check_mask, 2 * up);
        }
        if constexpr (Caps) {
            sink.add_promotions(push & promo_rank, up);
            const Bitboard east = pawn_attacks_east<Us>(free_pawns) & theirs & check_mask;
            const Bitboard west = pawn_attacks_west<Us>(free_pawns) & theirs & check_mask;
            sink.add_pawns(east & ~promo_rank, UP_EAST<Us>);
            sink.add_promotions(east & promo_rank, UP_EAST<Us>);
            sink.add_pawns(west & ~promo_rank, UP_WEST<Us>);
            sink.add_promotions(west & promo_rank, UP_WEST<Us>);
        }
    }

    // Pinned pawns are rare and (per the note above) can't move in check,
    // so they go one at a time, restricted to their pin line. The rank mask
    // just keeps a malformed FEN with a pawn on its last rank from shifting
    // off the board.
    if (!checkers) {
        for (Bitboard b = pawns & pinned & ~promo_rank; b;) {
            const int from = pop_lsb(b);
            const Bitboard line = line_through(ksq, from);

            const Bitboard one = shift<up>(bit(from)) & empty;
            const Bitboard push = one & line;
            if constexpr (Qts) {
                sink.add_pawns(push & ~promo_rank, up);
                sink.add_pawns(shift<up>(one & third_rank) & empty & line, 2 * up);
            }
            if constexpr (Caps) {
                sink.add_promotions(push & promo_rank, up);
                for (Bitboard caps = pawn_attacks(Us, from) & theirs & line; caps;) {
                    const int to = pop_lsb(caps);
                    sink.add_pawns(bit(to) & ~promo_rank, to - from);
                    sink.add_promotions(bit(to) & promo_rank, to - from);
                }
            }
        }
    }

    // En passant. Rare, and it removes two pieces from one line at once
    // (which the pin logic above doesn't model), so each candidate is tested
    // directly: rebuild the occupancy after the capture and check whether
    // anything still attacks our king.
    const int ep = pos.ep_square();
    if (Caps && ep >= 0 && rank_of(ep) == (Us == Color::White ? 5 : 2)) {
        const int cap_sq = ep - up;
        if ((their_pawns & bit(cap_sq)) && !(occ & bit(ep))) {
            for (Bitboard b = pawn_attacks(Them, ep) & pawns; b;) {
                const int from = pop_lsb(b);
                const Bitboard occ_after = (occ ^ bit(from) ^ bit(cap_sq)) | bit(ep);
                const Bitboard attackers =
                    (bishop_attacks(ksq, occ_after) & their_diagonal) |
                    (rook_attacks(ksq, occ_after) & their_orthogonal) |
                    (knight_attacks(ksq) & their_knights) |
                    (pawn_attacks(Us, ksq) & (their_pawns ^ bit(cap_sq)));
                if (!attackers)
                    sink.add(Move(from, ep, PieceType::None, true));
            }
        }
    }

    sink.add_from(ksq, king_targets);
    return sink.n;
}

template <GenType Type>
int dispatch(const Position& pos, Move* out) {
    return pos.side_to_move() == Color::White
        ? generate<Color::White, Type, false>(pos, out)
        : generate<Color::Black, Type, false>(pos, out);
}
}

void generate_legal(const Position& pos, MoveList& moves, int& count) {
    count = dispatch<GenType::All>(pos, moves.data());
}

void generate_captures(const Position& pos, MoveList& moves, int& count) {
    count = dispatch<GenType::Captures>(pos, moves.data());
}

void generate_quiets(const Position& pos, MoveList& moves, int& count) {
    count = dispatch<GenType::Quiets>(pos, moves.data());
}

int count_legal(const Position& pos) {
    return pos.side_to_move() == Color::White
        ? generate<Color::White, GenType::All, true>(pos, nullptr)
        : generate<Color::Black, GenType::All, true>(pos, nullptr);
}
