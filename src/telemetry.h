#pragma once
#include "search.h"
#include <cstdint>
#include <ostream>
#include <string>

// Telemetry on game positions: searches positions sampled from the games
// of a PGN file (an SPRT's games, say) with pruning verification on, and
// prints the pruning and feature counters. The bench's 18 positions are
// few and mostly early in the fifty-move count; this sees the positions
// the engine actually meets, and, unlike match telemetry (TelemetryDir),
// can verify, so it gives error rates.
struct TelemetryOptions {
    std::string in;               // PGN file
    int positions = 500;          // positions to search
    std::uint64_t nodes = 200000; // per position
    int verify = 20;              // verify 1 in N pruning decisions (0: none)
    int min_ply = 8;              // skip the first plies of each game
    std::uint64_t seed = 1;
};

void game_telemetry(Searcher& searcher, const TelemetryOptions& options, std::ostream& out);
