#pragma once
#include "position.h"
#include "types.h"
#include <string>
#include <vector>

// Syzygy endgame tablebases, read through Fathom (third_party/fathom).
//
// Two kinds of table: WDL says whether a position is won, drawn or lost
// with best play, and is what the search probes; DTZ says how many moves
// it takes to reach the next capture or pawn move while keeping that
// result, and ranks the root moves so a won position is actually converted
// within the fifty-move rule. Both assume the halfmove clock at zero and
// no castling rights.
namespace tb {

// Largest number of pieces (kings included) the search probes at, below
// what the tables hold if smaller (UCI SyzygyProbeLimit).
inline int probe_limit = 7;
// At exactly probe_limit pieces, probe only at this depth or more: the
// largest tables are the slowest to read (UCI SyzygyProbeDepth).
inline int probe_depth = 1;

// Opens the tables found in `path` (directories separated by ';' on
// Windows, ':' elsewhere). An empty path or "<empty>" closes them. Returns
// the largest piece count found, 0 if none.
int init(const std::string& path);

// Largest piece count the open tables cover; 0 when there are none.
int largest();

// The result for the side to move. Cursed wins and blessed losses would
// be wins and losses without the fifty-move rule, and are draws with it.
enum class Wdl { Loss, BlessedLoss, Draw, CursedWin, Win, Failed };

// For a position with the halfmove clock at 0, no castling rights and at
// most largest() pieces; Failed otherwise, or if a table is missing.
Wdl probe_wdl(const Position& pos);

// A root move and how good the tables say it is: higher is better. Moves
// with the best rank keep the root's result, and winning moves rank by
// distance to zeroing (the next capture or pawn move), fastest first.
struct RankedMove {
    Move move;
    int rank;
};

// Result of ranking the root: `moves` covers every legal move. `win` or
// `loss` is true when the best move wins or loses under the fifty-move
// rule. `dtz` tells whether the DTZ tables did the ranking (otherwise
// WDL alone: correct results, but no progress guarantee).
struct RootRanking {
    std::vector<RankedMove> moves;
    bool win = false;
    bool loss = false;
    bool dtz = false;
};

// Ranks the root moves of `pos`, which must have no castling rights and at
// most largest() pieces. False if the tables can't rank every move.
// use_dtz false ranks by WDL alone, without reading the DTZ tables.
bool rank_root(const Position& pos, RootRanking& out, bool use_dtz = true);

}  // namespace tb
