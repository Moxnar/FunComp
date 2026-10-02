#pragma once
#include "position.h"
#include "types.h"
#include <istream>
#include <string>
#include <vector>

// Reading PGN files (fastchess and cutechess output): the start position and
// the moves of each game, for tools that replay games (telemetry on game
// positions, data generation).
namespace pgn {

struct Game {
    std::string fen;                 // the FEN tag, or the standard start
    std::vector<std::string> moves;  // in SAN, as written
    std::string result;              // the Result tag
};

// Every game in `in`. Comments, variations, NAGs, move numbers and results
// in the movetext are skipped.
std::vector<Game> read(std::istream& in);

// The legal move of `pos` that `san` names (check marks and annotations
// ignored), or Move{} if there is none or the SAN is ambiguous.
Move parse_san(const Position& pos, const std::string& san);

}  // namespace pgn
