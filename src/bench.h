#pragma once
#include "search.h"
#include <iosfwd>

// Searches a fixed set of positions to a fixed depth and reports the total
// node count and speed. The node count is a signature of the search: any
// change to what it does changes the count, and a change that shouldn't
// (a refactor, a speedup) must leave it alone. Pruning statistics for the
// run follow.
void bench(Searcher& searcher, int depth, std::ostream& out);

inline constexpr int BENCH_DEPTH = 13;
