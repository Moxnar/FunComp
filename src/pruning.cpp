#include "pruning.h"
#include <cmath>
#include <cstdio>
#include <ostream>

namespace prune {

namespace detail {
std::array<std::array<std::int32_t, 64>, 64> lmr_table{};
}

void init() {
    const double base = params::lmr_base / 100.0;
    const double mult = params::lmr_mult / 1e6;
    for (int d = 0; d < 64; ++d)
        for (int m = 0; m < 64; ++m) {
            const double r = d && m ? base + std::log(d) * std::log(m) * mult : 0.0;
            detail::lmr_table[static_cast<std::size_t>(d)][static_cast<std::size_t>(m)] =
                static_cast<std::int32_t>(
                    std::clamp(std::lround(r * LMR_SCALE), 0L, 63L * LMR_SCALE));
        }
}

void Stats::print(std::ostream& out) const {
    auto pct = [](std::uint64_t a, std::uint64_t b) {
        return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0;
    };
    char line[160];
    std::snprintf(line, sizeof line, "info string %-12s %14s %14s %7s %10s %9s %7s",
                  "heuristic", "tried", "fired", "fire%", "verified", "wrong", "wrong%");
    out << line << '\n';
    for (int h = 0; h < COUNT; ++h) {
        const Counter& k = c[static_cast<std::size_t>(h)];
        std::snprintf(line, sizeof line,
                      "info string %-12s %14llu %14llu %6.2f%% %10llu %9llu %6.2f%%", NAMES[h],
                      static_cast<unsigned long long>(k.tried),
                      static_cast<unsigned long long>(k.fired), pct(k.fired, k.tried),
                      static_cast<unsigned long long>(k.verified),
                      static_cast<unsigned long long>(k.wrong), pct(k.wrong, k.verified));
        out << line << '\n';
    }
    const Counter& lmr = c[LMR];
    std::snprintf(line, sizeof line, "info string lmr re-searches %llu (%.2f%% of reductions)",
                  static_cast<unsigned long long>(lmr_researches), pct(lmr_researches, lmr.fired));
    out << line << std::endl;
}

}  // namespace prune
