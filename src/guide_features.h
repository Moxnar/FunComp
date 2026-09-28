#pragma once
#include "position.h"
#include <array>

// Hand-written position features for the learned pruning guide: the
// inputs, besides depth and static eval, that the quantile model sees in
// the labels and would see again in the search. All are from the side to
// move's point of view, and material is in SEE_VALUE centipawns.
//
// Written for label logging, not speed: extracting them costs about as
// much as a node's move generation. A guide running inside the search
// would compute a cheaper subset.
namespace features {

inline constexpr int COUNT = 14;

inline constexpr std::array<const char*, COUNT> NAMES = {
    "phase",           // 24 with all pieces on the board, down to 0
    "material",        // ours minus theirs, pawns included
    "our_pieces",      // our non-pawn material
    "their_pieces",    // their non-pawn material
    "our_pawns",       // count
    "their_pawns",     // count
    "our_threatened",  // most valuable of our pieces attacked by something
                       // cheaper, or attacked and undefended
    "their_threatened",
    "best_capture",    // most valuable victim we can take without losing
                       // the exchange (by SEE)
    "our_king_attackers",    // enemy pieces attacking our king or the
                             // squares next to it
    "their_king_attackers",
    "passed_pawns",    // ours minus theirs
    "mobility",        // our legal move count
    "halfmove_clock",
};

using Vector = std::array<float, COUNT>;

Vector extract(const Position& pos);

}  // namespace features
