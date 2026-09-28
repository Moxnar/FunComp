#include "bench.h"
#include <chrono>
#include <cstdint>
#include <ostream>
#include <vector>

namespace {
// Openings, middlegames and endgames: the standard perft test positions
// (Chess Programming Wiki), then positions from FunComp's own self-play
// games (2026-09-27, from balanced book openings, plies 30 to 171).
constexpr const char* POSITIONS[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "r4rk1/1b2qppp/1p1b1n2/p1pp4/P2P4/2PBPNB1/2Q2PPP/3R1RK1 w - - 0 16",
    "4r3/5pk1/1p4q1/p1b3pp/P1Q1B3/4PPP1/5PK1/3R4 b - - 5 31",
    "8/5p2/2b1pPk1/3pP1p1/2pP3p/1rN4P/2RK2P1/8 w - - 3 46",
    "8/6p1/6k1/3p3P/2nPpP2/r1B1P3/2R2K2/8 b - - 0 61",
    "3r2k1/1p3ppp/p1n5/2b1p3/P2r4/1P1B2N1/2P2PPP/3R1RK1 w - - 5 21",
    "1rr5/4k3/p2n3p/1p1BpP1P/8/1RP2K2/1P6/3R4 b - - 0 36",
    "8/5k2/2R5/5r2/3P3b/4B3/4K3/8 w - - 17 76",
    "6R1/1p5r/P7/1pPp4/5p2/5Kp1/7k/8 b - - 0 51",
    "r4rk1/1p1n4/3pq2p/p1p3p1/P1n1B3/2Q2P2/1P3B1P/R1K3R1 w - - 0 26",
    "8/6K1/r5P1/5k2/8/8/8/6R1 b - - 9 66",
    "3R4/5pk1/p1B1p1np/p1N5/P1b3n1/2P5/3KP2P/8 w - - 12 41",
    "3R4/8/8/2rb4/3k4/8/8/1K6 b - - 54 86",
    "8/n7/8/3k3p/7P/2p1BP2/8/2K5 w - - 0 56",
};
}

void bench(Searcher& searcher, int depth, std::ostream& out) {
    // Only the searches are timed. Building the attack tables (on the first
    // Position) and clearing the hash table before each position are fixed
    // costs that would swamp a bench this short and understate the speed.
    using Clock = std::chrono::steady_clock;
    Clock::duration searching{};
    std::uint64_t nodes = 0;
    searcher.clear_stats();
    for (const char* fen : POSITIONS) {
        Position pos;
        if (!pos.set_fen(fen)) {
            out << "info string bench: bad FEN " << fen << std::endl;
            continue;
        }
        searcher.new_game();
        SearchLimits limits;
        limits.depth = depth;
        const auto start = Clock::now();
        searcher.search(pos, limits, {}, false);
        searching += Clock::now() - start;
        nodes += searcher.nodes();
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(searching).count();
    searcher.stats().print(out);
    out << "info string bench depth " << depth << ", " << std::size(POSITIONS) << " positions\n";
    out << nodes << " nodes " << (ms > 0 ? nodes * 1000 / static_cast<std::uint64_t>(ms) : nodes)
        << " nps" << std::endl;
}
