#include "tablebase.h"
#include "movegen.h"
#include <tbprobe.h>
#include <algorithm>
#include <memory>

namespace tb {

namespace {

// The position as Fathom takes it: occupancy by colour and by piece type,
// the en passant square (0 if none) and the side to move (true: White).
struct Boards {
    std::uint64_t white, black, kings, queens, rooks, bishops, knights, pawns;
    unsigned ep;
    bool white_to_move;
};

Boards boards(const Position& pos) {
    auto both = [&](PieceType pt) {
        return pos.pieces(static_cast<Piece>(piece_index(Color::White, pt))) |
               pos.pieces(static_cast<Piece>(piece_index(Color::Black, pt)));
    };
    return {pos.occupancy(Color::White),
            pos.occupancy(Color::Black),
            both(PieceType::King),
            both(PieceType::Queen),
            both(PieceType::Rook),
            both(PieceType::Bishop),
            both(PieceType::Knight),
            both(PieceType::Pawn),
            pos.ep_square() >= 0 ? static_cast<unsigned>(pos.ep_square()) : 0u,
            pos.side_to_move() == Color::White};
}

// Fathom numbers promotions queen 1 to knight 4.
PieceType promotion_of(unsigned tb_promotes) {
    switch (tb_promotes) {
    case TB_PROMOTES_QUEEN: return PieceType::Queen;
    case TB_PROMOTES_ROOK: return PieceType::Rook;
    case TB_PROMOTES_BISHOP: return PieceType::Bishop;
    case TB_PROMOTES_KNIGHT: return PieceType::Knight;
    default: return PieceType::None;
    }
}

// Rank at or beyond which a root move wins (loses) under the fifty-move
// rule, in Fathom's ranking with the rule on.
constexpr int WIN_RANK = 900;

}  // namespace

int init(const std::string& path) {
    // Fathom closes any tables already open, and opens none for an empty
    // path or "<empty>".
    if (!tb_init(path.c_str())) return 0;
    return static_cast<int>(TB_LARGEST);
}

int largest() { return static_cast<int>(TB_LARGEST); }

Wdl probe_wdl(const Position& pos) {
    const Boards b = boards(pos);
    const unsigned r = tb_probe_wdl(b.white, b.black, b.kings, b.queens, b.rooks, b.bishops,
                                    b.knights, b.pawns,
                                    static_cast<unsigned>(pos.halfmove_clock()),
                                    static_cast<unsigned>(pos.castling_rights()), b.ep,
                                    b.white_to_move);
    switch (r) {
    case TB_LOSS: return Wdl::Loss;
    case TB_BLESSED_LOSS: return Wdl::BlessedLoss;
    case TB_DRAW: return Wdl::Draw;
    case TB_CURSED_WIN: return Wdl::CursedWin;
    case TB_WIN: return Wdl::Win;
    default: return Wdl::Failed;
    }
}

bool rank_root(const Position& pos, RootRanking& out, bool use_dtz) {
    out = RootRanking{};
    const Boards b = boards(pos);
    const auto rule50 = static_cast<unsigned>(pos.halfmove_clock());
    const auto castling = static_cast<unsigned>(pos.castling_rights());
    // About 100 KB: too big for the search thread's stack frame.
    auto ranked = std::make_unique<TbRootMoves>();
    // Fathom ranks all wins alike until the fifty-move rule comes close,
    // unless told the position has repeated: then by distance to zeroing.
    // We always ask for that. Otherwise the search, left to choose among
    // equal wins, can drift until the counter forces progress, and convert
    // on the last ply (KBNK mated on ply 99 at 10 ms a move).
    out.dtz = use_dtz &&
              tb_probe_root_dtz(b.white, b.black, b.kings, b.queens, b.rooks, b.bishops,
                                b.knights, b.pawns, rule50, castling, b.ep, b.white_to_move,
                                true, true, ranked.get()) != 0;
    if (!out.dtz &&
        !tb_probe_root_wdl(b.white, b.black, b.kings, b.queens, b.rooks, b.bishops, b.knights,
                           b.pawns, rule50, castling, b.ep, b.white_to_move, true, ranked.get()))
        return false;

    // Fathom generates its own move list: match each move to ours.
    MoveList legal;
    int count = 0;
    generate_legal(pos, legal, count);
    if (static_cast<int>(ranked->size) != count) return false;
    int best = -1'000'000;
    for (unsigned i = 0; i < ranked->size; ++i) {
        const TbRootMove& rm = ranked->moves[i];
        const int from = static_cast<int>(TB_MOVE_FROM(rm.move));
        const int to = static_cast<int>(TB_MOVE_TO(rm.move));
        const PieceType promo = promotion_of(TB_MOVE_PROMOTES(rm.move));
        const auto it = std::find_if(legal.begin(), legal.begin() + count, [&](Move m) {
            return m.from() == from && m.to() == to && m.promotion() == promo;
        });
        if (it == legal.begin() + count) return false;
        out.moves.push_back({*it, rm.tbRank});
        best = std::max(best, rm.tbRank);
    }
    out.win = best >= WIN_RANK;
    out.loss = best <= -WIN_RANK;
    return true;
}

}  // namespace tb
