#include "telemetry.h"
#include "pgn.h"
#include <fstream>
#include <random>
#include <vector>

void game_telemetry(Searcher& searcher, const TelemetryOptions& o, std::ostream& out) {
    std::ifstream file(o.in);
    if (!file) {
        out << "info string telemetry: cannot read " << o.in << std::endl;
        return;
    }
    const std::vector<pgn::Game> games = pgn::read(file);
    if (games.empty()) {
        out << "info string telemetry: no games in " << o.in << std::endl;
        return;
    }
    std::mt19937_64 rng(o.seed);
    searcher.clear_stats();
    searcher.set_verify_rate(o.verify);
    int done = 0, unreadable = 0;
    std::uint64_t nodes = 0;
    for (int tries = 0; done < o.positions && tries < 20 * o.positions; ++tries) {
        const pgn::Game& game = games[rng() % games.size()];
        const int plies = static_cast<int>(game.moves.size());
        if (plies <= o.min_ply) continue;
        const int ply = o.min_ply + static_cast<int>(rng() % static_cast<std::uint64_t>(plies - o.min_ply));
        Position pos;
        if (!pos.set_fen(game.fen)) {
            ++unreadable;
            continue;
        }
        // Replay to the sampled ply, keeping the keys for repetitions.
        std::vector<std::uint64_t> history;
        bool ok = true;
        for (int i = 0; i < ply && ok; ++i) {
            const Move m = pgn::parse_san(pos, game.moves[static_cast<std::size_t>(i)]);
            if (m == Move{}) {
                ok = false;
                break;
            }
            history.push_back(pos.key());
            StateInfo st;
            pos.make_move(m, st);
        }
        if (!ok) {
            ++unreadable;
            continue;
        }
        searcher.new_game();
        SearchLimits limits;
        limits.nodes = o.nodes;
        searcher.search(pos, limits, history, false);
        nodes += searcher.nodes();
        ++done;
    }
    searcher.set_verify_rate(0);
    out << "info string telemetry: " << done << " positions from " << games.size()
        << " games (" << unreadable << " unreadable), " << nodes << " nodes, verify 1 in "
        << o.verify << '\n';
    searcher.stats().print(out);
}
