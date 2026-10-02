#include "bingo.h"
#include "movegen.h"
#include "position.h"
#include "search.h"
#include <array>
#include <cstdlib>
#include <fstream>
#include <ostream>
#include <random>

namespace {

// One side's 18 back ranks, as eight piece letters (upper case): the queen,
// bishops and knights over files b, c, d, f, g, the bishops on opposite
// colours. `rank` is 0 for White, 7 for Black; a square is light when its
// file plus rank is odd.
std::vector<std::string> back_ranks(int rank) {
    constexpr std::array<int, 5> FILES = {1, 2, 3, 5, 6};
    std::vector<int> light, dark;
    for (int f : FILES) ((f + rank) % 2 == 1 ? light : dark).push_back(f);
    std::vector<std::string> out;
    for (int lb : light)
        for (int db : dark)
            for (int q : FILES) {
                if (q == lb || q == db) continue;
                std::string r = "R???K??R";
                r[static_cast<std::size_t>(lb)] = 'B';
                r[static_cast<std::size_t>(db)] = 'B';
                r[static_cast<std::size_t>(q)] = 'Q';
                for (char& c : r)
                    if (c == '?') c = 'N';
                out.push_back(r);
            }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(c - 'A' + 'a');
    return s;
}

}  // namespace

std::vector<std::string> bingo_starts() {
    const std::vector<std::string> white = back_ranks(0), black = back_ranks(7);
    std::vector<std::string> fens;
    for (const std::string& b : black)
        for (const std::string& w : white)
            fens.push_back(lower(b) + "/pppppppp/8/8/8/8/PPPPPPPP/" + w + " w KQkq - 0 1");
    return fens;
}

std::uint64_t generate_bingo(const BingoOptions& opt, std::ostream& log) {
    std::ofstream out(opt.out);
    if (!out) {
        log << "info string genbingo: cannot write " << opt.out << std::endl;
        return 0;
    }
    std::mt19937_64 rng(opt.seed);
    Searcher searcher(static_cast<std::size_t>(opt.hash_mb));
    const std::vector<std::string> starts = bingo_starts();
    std::uint64_t written = 0, tried = 0;
    for (std::size_t s = 0; s < starts.size(); ++s) {
        for (int k = 0; k < opt.per_start; ++k) {
            Position pos;
            pos.set_fen(starts[s]);
            std::vector<std::uint64_t> history;
            std::vector<StateInfo> states(static_cast<std::size_t>(opt.plies) + 1);
            bool alive = true;
            for (int i = 0; i < opt.plies && alive; ++i) {
                MoveList moves;
                int count = 0;
                generate_legal(pos, moves, count);
                if (count == 0) {
                    alive = false;
                    break;
                }
                history.push_back(pos.key());
                pos.make_move(moves[static_cast<std::size_t>(rng() % static_cast<std::uint64_t>(count))],
                              states[static_cast<std::size_t>(i)]);
            }
            MoveList moves;
            int count = 0;
            generate_legal(pos, moves, count);
            if (!alive || count == 0) continue;  // the game ended in the random plies
            ++tried;
            searcher.new_game();
            SearchLimits limits;
            limits.nodes = opt.nodes;
            searcher.search(pos, limits, history, false);
            if (std::abs(searcher.last_score()) > opt.max_score) continue;
            out << pos.fen() << '\n';
            ++written;
        }
        if ((s + 1) % 54 == 0)
            log << "info string genbingo: " << s + 1 << '/' << starts.size() << " starts, "
                << written << " of " << tried << " positions kept" << std::endl;
    }
    log << "info string genbingo: wrote " << written << " positions (of " << tried
        << " searched) to " << opt.out << std::endl;
    return written;
}
