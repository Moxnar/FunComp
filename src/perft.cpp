#include "perft.h"
#include "movegen.h"
#include <bit>

std::uint64_t perft(Position& pos, int depth) {
    if (depth == 0) return 1;
    if (depth == 1) return static_cast<std::uint64_t>(count_legal(pos));

    MoveList moves;
    int count = 0;
    generate_legal(pos, moves, count);

    std::uint64_t nodes = 0;
    for (int i = 0; i < count; ++i) {
        StateInfo st;
        pos.make_move(moves[i], st);
        nodes += perft(pos, depth - 1);
        pos.unmake_move(moves[i], st);
    }
    return nodes;
}

PerftTable::PerftTable(std::size_t megabytes) {
    // Largest power of two number of entries that fits in the budget.
    const std::size_t want = (megabytes << 20) / sizeof(Entry);
    const std::size_t n = want ? std::bit_floor(want) : 1;
    entries_.assign(n, Entry{});
    mask_ = n - 1;
}

void PerftTable::clear() {
    for (auto& e : entries_) e = Entry{};
}

bool PerftTable::probe(std::uint64_t key, int depth, std::uint64_t& nodes) const {
    const Entry& e = entries_[key & mask_];
    if (e.key != key || (e.data & 0xFF) != static_cast<std::uint64_t>(depth)) return false;
    nodes = e.data >> 8;
    return true;
}

void PerftTable::store(std::uint64_t key, int depth, std::uint64_t nodes) {
    entries_[key & mask_] = Entry{key, (nodes << 8) | static_cast<std::uint64_t>(depth & 0xFF)};
}

std::uint64_t perft(Position& pos, int depth, PerftTable& tt) {
    if (depth == 0) return 1;
    if (depth == 1) return static_cast<std::uint64_t>(count_legal(pos));

    std::uint64_t nodes = 0;
    if (tt.probe(pos.key(), depth, nodes)) return nodes;

    MoveList moves;
    int count = 0;
    generate_legal(pos, moves, count);

    for (int i = 0; i < count; ++i) {
        StateInfo st;
        pos.make_move(moves[i], st);
        nodes += perft(pos, depth - 1, tt);
        pos.unmake_move(moves[i], st);
    }
    tt.store(pos.key(), depth, nodes);
    return nodes;
}
