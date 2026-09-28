#pragma once
#include "position.h"
#include <cstddef>
#include <cstdint>
#include <vector>

// Plain perft: no hashing, so it stays an independent check of move
// generation and make/unmake.
std::uint64_t perft(Position& pos, int depth);

// Transposition table for perft. Each entry holds a full Zobrist key plus
// the node count for one (position, depth) pair. Always-replace; a probe
// only hits when both key and depth match.
class PerftTable {
public:
    explicit PerftTable(std::size_t megabytes = 64);

    void clear();
    bool probe(std::uint64_t key, int depth, std::uint64_t& nodes) const;
    void store(std::uint64_t key, int depth, std::uint64_t nodes);

private:
    struct Entry {
        std::uint64_t key = 0;
        std::uint64_t data = 0;  // nodes << 8 | depth; depth 0 marks empty
    };
    std::vector<Entry> entries_;
    std::size_t mask_ = 0;
};

// Perft using `tt` for positions reached at depth >= 2. The table can be
// reused across calls: entries are keyed by position and depth.
std::uint64_t perft(Position& pos, int depth, PerftTable& tt);
