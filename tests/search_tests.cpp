#include "bingo.h"
#include "cont_log.h"
#include "eval.h"
#include "guide_features.h"
#include "initiative.h"
#include "labels.h"
#include "movegen.h"
#include "params.h"
#include "pgn.h"
#include "position.h"
#include "search.h"
#include "see.h"
#include "tablebase.h"
#include "threads.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

Move find_move(const Position& pos, const std::string& uci) {
    MoveList moves;
    int count = 0;
    generate_legal(pos, moves, count);
    for (int i = 0; i < count; ++i)
        if (pos.move_to_uci(moves[static_cast<std::size_t>(i)]) == uci)
            return moves[static_cast<std::size_t>(i)];
    return Move{};
}

// The exchange started by `uci` in `fen` is worth exactly `value`.
void see_case(const char* name, const char* fen, const char* uci, int value) {
    Position pos;
    if (!pos.set_fen(fen)) {
        check(false, std::string(name) + ": bad FEN");
        return;
    }
    const Move m = find_move(pos, uci);
    if (m == Move{}) {
        check(false, std::string(name) + ": move " + uci + " not legal");
        return;
    }
    check(see_at_least(pos, m, value), std::string(name) + ": SEE >= " + std::to_string(value));
    check(!see_at_least(pos, m, value + 1), std::string(name) + ": SEE < " + std::to_string(value + 1));
}

void test_see() {
    see_case("undefended pawn", "1k1r4/1pp4p/p7/4p3/8/P5P1/1PP4P/2K1R3 w - - 0 1", "e1e5", 100);
    see_case("queen takes defended pawn", "4k3/8/3p4/4p3/8/8/8/4QK2 w - - 0 1", "e1e5", -800);
    see_case("knight takes defended pawn", "4k3/8/3p4/4p3/8/5N2/8/4K3 w - - 0 1", "f3e5", -220);
    // White's second rook joins from behind the first; so does Black's.
    // Without x-rays this would come out at +100.
    see_case("x-ray battery", "4k3/4r3/4r3/4p3/8/8/4R3/4RK2 w - - 0 1", "e2e5", -400);
    // Nxc4 Nxc4 Bxc4: the defender's recapture only loses more, so the
    // exchange must be played to the end, not cut off after it.
    see_case("recapture that doesn't help",
             "r2k3r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R2K3R w - - 2 1", "e2c4", -330);
    // A king can't recapture on a square the other king guards.
    see_case("king can't recapture next to king",
             "8/2p5/1p1p2p1/1P1Bpk2/p1Pr4/P3K3/8/4R3 w - - 8 1", "d5e4", -330);
    see_case("pawn guarded by king", "8/2p5/1p1p2pk/1P1B4/p1P2K2/P6q/8/2Q5 b - - 0 1", "g6g5", 0);
    see_case("quiet move to attacked square", "4k3/8/3p4/8/8/3N4/8/4K3 w - - 0 1", "d3e5", -320);
    see_case("quiet move to safe square", "4k3/8/3p4/8/8/3N4/8/4K3 w - - 0 1", "d3f4", 0);
    // The king may take only if nothing takes it back.
    see_case("king takes undefended", "4k3/8/8/8/8/8/3p4/4K3 w - - 0 1", "e1d2", 100);
    see_case("en passant counts as even", "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", "e5d6", 0);
    // Promotions gain the new piece less the pawn, and the new piece is
    // what can be taken back.
    see_case("safe promotion", "4k3/P7/8/8/8/8/8/4K3 w - - 0 1", "a7a8q", 800);
    see_case("promotion onto a guarded square", "7r/P3k3/8/8/8/8/8/4K3 w - - 0 1", "a7a8q", -100);
    see_case("capture-promotion, retaken", "r3k3/1P6/1n6/8/8/8/8/4K3 w - - 0 1", "b7a8q", 400);
    // Qxb8 Rxb8 axb8=Q: the recapture would lose the rook to a promoting
    // pawn, so Black declines it (without the promotion: -80).
    see_case("pawn recapture promotes", "1n5r/P7/4k3/8/8/8/8/1Q2K3 w - - 0 1", "b1b8", 320);
}

// The mop-up term in `fen` is worth exactly `value` to the side to move.
void mop_up_case(const char* name, const char* fen, int value) {
    Position pos;
    if (!pos.set_fen(fen)) {
        check(false, std::string(name) + ": bad FEN");
        return;
    }
    const int with = evaluate(pos);
    const int edge = params::mop_edge, kings = params::mop_kings;
    params::mop_edge = params::mop_kings = 0;
    const int without = evaluate(pos);
    params::mop_edge = edge;
    params::mop_kings = kings;
    check(with - without == value,
          std::string(name) + ": mop-up " + std::to_string(with - without) + ", expected " + std::to_string(value));
}

void test_mop_up() {
    // Lone king in the corner, kings 7 apart: the edge term alone.
    mop_up_case("rook vs cornered king", "k7/8/8/8/8/8/8/R3K3 w - - 0 1", 6 * params::mop_edge);
    mop_up_case("seen by the defender", "k7/8/8/8/8/8/8/R3K3 b - - 0 1", -6 * params::mop_edge);
    // King on d5 (a centre square), kings 2 apart: the kings term alone.
    mop_up_case("kings close", "8/8/8/3k4/8/4K3/8/R7 w - - 0 1", 5 * params::mop_kings);
    // Bishop and knight: only the corners of the bishop's colour count.
    mop_up_case("bishop's corner", "7k/8/8/8/8/8/8/1NB1K3 w - - 0 1", 7 * params::mop_edge);
    mop_up_case("wrong corner", "7k/8/8/8/8/8/8/1N1BK3 w - - 0 1", 0);
    mop_up_case("knight alone", "k7/8/8/8/8/8/8/N3K3 w - - 0 1", 0);
    mop_up_case("two knights", "k7/8/8/8/8/8/8/NN2K3 w - - 0 1", 0);
    // Only against a bare king: a rook left to the defender turns it off.
    mop_up_case("queen vs rook", "k7/r7/8/8/8/8/8/Q3K3 w - - 0 1", 0);
    // Pawns on the stronger side are fine.
    mop_up_case("rook and pawn", "k7/8/8/8/8/8/P7/R3K3 w - - 0 1", 6 * params::mop_edge);
    mop_up_case("both sides have pawns", "k7/p7/8/8/8/8/P7/R3K3 w - - 0 1", 0);
}

void test_null_move() {
    Position pos;
    pos.set_fen("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3");
    const std::uint64_t key = pos.key();
    const std::string fen = pos.fen();

    StateInfo st;
    pos.make_null_move(st);
    Position expected;
    expected.set_fen("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR b KQkq - 1 3");
    check(pos.key() == expected.key(), "null move: key matches the passed position");
    check(pos.side_to_move() == Color::Black && pos.ep_square() == -1,
          "null move: side flipped, ep square cleared");
    check(pos.is_consistent(), "null move: position consistent");

    pos.unmake_null_move(st);
    check(pos.key() == key && pos.fen() == fen, "null move: unmake restores the position");

    Position kp;
    kp.set_fen("8/5k2/8/8/8/8/4P3/4K3 w - - 0 1");
    check(!kp.has_non_pawn_material(Color::White), "king and pawn has no piece material");
    check(pos.has_non_pawn_material(Color::White), "start material has piece material");
}

// Finds the mate at `depth` with the full pruning suite on.
void mate_case(const char* name, const char* fen, const char* best_uci, int mate_in, int depth) {
    Position pos;
    pos.set_fen(fen);
    Searcher s(16);
    SearchLimits limits;
    limits.depth = depth;
    const Move best = s.search(pos, limits, {}, false);
    check(pos.move_to_uci(best) == best_uci,
          std::string(name) + ": played " + pos.move_to_uci(best) + ", expected " + best_uci);
    check(s.last_score() == Searcher::MATE - (2 * mate_in - 1),
          std::string(name) + ": score " + std::to_string(s.last_score()) + " is not mate in " +
              std::to_string(mate_in));
}

void test_mates() {
    mate_case("back rank", "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1", "d1d8", 1, 6);
    mate_case("rook sacrifice", "1r4k1/5ppp/8/8/8/8/3R1PPP/3R2K1 w - - 0 1", "d2d8", 2, 8);
    mate_case("smothered", "r1b3kr/ppp1Bp1p/1b6/n2P4/2p3q1/2Q2N2/P4PPP/RN2R1K1 w - - 1 0",
              "c3h8", 3, 10);

    // Fail-firm returns leave decisive scores alone: the same mates, found
    // and scored exactly, with every site on.
    const int rfp = params::ff_rfp_pct, sp = params::ff_stand_pat_pct,
              pc = params::ff_probcut_pct, md = params::ff_main_depth;
    params::ff_rfp_pct = params::ff_stand_pat_pct = params::ff_probcut_pct = 50;
    params::ff_main_depth = 1;
    mate_case("fail-firm: rook sacrifice", "1r4k1/5ppp/8/8/8/8/3R1PPP/3R2K1 w - - 0 1", "d2d8",
              2, 8);
    mate_case("fail-firm: smothered",
              "r1b3kr/ppp1Bp1p/1b6/n2P4/2p3q1/2Q2N2/P4PPP/RN2R1K1 w - - 1 0", "c3h8", 3, 10);
    params::ff_rfp_pct = rfp;
    params::ff_stand_pat_pct = sp;
    params::ff_probcut_pct = pc;
    params::ff_main_depth = md;
}

// The PV printed after the last iteration of a search of `fen` after
// `moves` (UCI), as a list of moves.
std::vector<std::string> printed_pv(Searcher& s, const char* fen,
                                    const std::vector<std::string>& moves, int depth) {
    Position pos;
    pos.set_fen(fen);
    std::vector<std::uint64_t> history;
    std::vector<StateInfo> states(moves.size());
    for (std::size_t i = 0; i < moves.size(); ++i) {
        history.push_back(pos.key());
        pos.make_move(find_move(pos, moves[i]), states[i]);
    }
    SearchLimits limits;
    limits.depth = depth;
    std::ostringstream out;
    auto* const old = std::cout.rdbuf(out.rdbuf());
    s.search(pos, limits, history, true);
    std::cout.rdbuf(old);

    std::string line, last;
    std::istringstream in(out.str());
    while (std::getline(in, line))
        if (line.starts_with("info depth")) last = line;
    std::vector<std::string> pv;
    std::istringstream words(last.substr(last.find(" pv") + 3));
    for (std::string w; words >> w;) pv.push_back(w);
    return pv;
}

// The PV stops where the game would be drawn, even when the hash table
// holds moves from there on (from an earlier search, as in a game).
void test_pv_stops_at_draws() {
    // Perpetual check, the cycle already played once: the line ends at the
    // first repetition, not at the third.
    const char* perpetual = "4Q1k1/6p1/8/8/8/2rr4/1q3PPP/6K1 b - - 0 1";
    Searcher s(16);
    printed_pv(s, perpetual, {"g8h7"}, 10);
    const auto rep = printed_pv(s, perpetual, {"g8h7", "e8h5", "h7g8", "h5e8", "g8h7"}, 10);
    check(rep == std::vector<std::string>{"e8h5"}, "pv: stops at a repetition");

    // Four plies from the fifty-move rule: at most four moves.
    Searcher t(16);
    printed_pv(t, "8/8/4k3/8/8/3K4/8/R7 w - - 0 80", {}, 12);
    const auto fifty = printed_pv(t, "8/8/4k3/8/8/3K4/8/R7 w - - 96 80", {}, 12);
    check(!fifty.empty() && fifty.size() <= 4, "pv: stops at the fifty-move rule");
}

// A stop requested (as from the UCI thread) ends the search at once with a
// legal move, and stays in force until cleared.
void test_stop_request() {
    Position pos;
    Searcher s(16);
    SearchLimits limits;
    limits.depth = 64;
    s.request_stop();
    const Move best = s.search(pos, limits, {}, false);
    check(find_move(pos, pos.move_to_uci(best)) != Move{}, "stop: legal move");
    check(s.nodes() <= 2048, "stop: search ended at the first check");
    s.clear_stop();
    limits.depth = 10;
    s.search(pos, limits, {}, false);
    check(s.nodes() > 2048, "stop: cleared, the next search runs");
}

// PGN reading and SAN: tags, comments, variations, castling both ways,
// disambiguation, promotion, and a second game in the same file.
void test_pgn() {
    std::istringstream in(
        "[Event \"t\"]\n"
        "[FEN \"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1\"]\n"
        "[Result \"*\"]\n"
        "\n"
        "1. O-O {+0.10/10 0.1s} O-O-O 2. Rfd1 (2. Ra2 Kb8) Rhe8 3. Kg2 *\n"
        "[Event \"t2\"]\n"
        "[Result \"1/2-1/2\"]\n"
        "\n"
        "1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 4. Bxc6 dxc6 5. O-O f6 1/2-1/2\n");
    const std::vector<pgn::Game> games = pgn::read(in);
    check(games.size() == 2, "pgn: two games");
    if (games.size() != 2) return;
    auto replay = [](const pgn::Game& g, Position& pos) {
        pos.set_fen(g.fen);
        for (const std::string& san : g.moves) {
            const Move m = pgn::parse_san(pos, san);
            if (m == Move{}) return false;
            StateInfo st;
            pos.make_move(m, st);
        }
        return true;
    };
    Position pos;
    check(games[0].moves.size() == 5 && replay(games[0], pos) &&
              pos.fen().rfind("2krr3/8/8/8/8/8/6K1/R2R4 b", 0) == 0,
          "pgn: castling and disambiguation replayed (" + pos.fen() + ")");
    check(games[1].moves.size() == 10 && games[1].result == "1/2-1/2" && replay(games[1], pos),
          "pgn: second game from the start position");
    pos.set_fen("8/P6k/8/8/8/8/8/K7 w - - 0 1");
    const Move promo = pgn::parse_san(pos, "a8=Q+");
    check(promo != Move{} && promo.promotion() == PieceType::Queen, "pgn: promotion");
    check(pgn::parse_san(pos, "Nf3") == Move{}, "pgn: no such move");
}

// The hash-table key carries the halfmove clock's bucket when
// FiftyHashWidth is set; the position's own key (repetitions) never does.
void test_tt_key() {
    Position a, b, c;
    a.set_fen("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    b.set_fen("4k3/8/8/8/8/8/8/R3K3 w - - 50 1");
    c.set_fen("4k3/8/8/8/8/8/8/R3K3 w - - 55 1");
    const int start = params::fifty_hash_start, width = params::fifty_hash_width;
    params::fifty_hash_width = 0;
    check(tt_key(a) == a.key() && tt_key(b) == b.key(), "tt_key: off, the position's key");
    params::fifty_hash_start = 10;
    params::fifty_hash_width = 10;
    check(a.key() == b.key(), "tt_key: the position's key ignores the clock");
    check(tt_key(a) == a.key(), "tt_key: below the start, the position's key");
    check(tt_key(a) != tt_key(b), "tt_key: clock 0 and 50 differ");
    check(tt_key(b) == tt_key(c), "tt_key: one bucket for 50 and 55");
    params::fifty_hash_start = start;
    params::fifty_hash_width = width;
}

// Time management's node share: the nodes counted under each root move add
// up to the root total, which is every node but the root visits.
void test_root_move_nodes() {
    Position pos;
    Searcher s(16);
    SearchLimits limits;
    limits.depth = 9;
    s.search(pos, limits, {}, false);
    MoveList moves;
    int count = 0;
    generate_legal(pos, moves, count);
    std::uint64_t sum = 0;
    for (int i = 0; i < count; ++i) sum += s.root_move_nodes(moves[static_cast<std::size_t>(i)]);
    check(sum == s.root_nodes_total(), "root nodes: the moves add up to the total");
    check(s.root_nodes_total() < s.nodes() && s.root_nodes_total() + 100 > s.nodes(),
          "root nodes: all nodes but the root visits (" + std::to_string(s.root_nodes_total()) +
              " of " + std::to_string(s.nodes()) + ")");
}

// Lazy SMP: helpers search alongside the main searcher and stop with it;
// with one thread the pool is the plain search (same nodes as a Searcher).
void test_thread_pool() {
    Position pos;
    SearchLimits limits;
    limits.depth = 10;
    ThreadPool pool(16);
    Searcher alone(16);
    pool.search(pos, limits, {}, false);
    alone.search(pos, limits, {}, false);
    check(pool.main().nodes() == alone.nodes(), "threads: one thread searches as a Searcher");

    pool.set_threads(4);
    pool.new_game();
    for (int i = 0; i < 3; ++i) {
        const Move best = pool.search(pos, limits, {}, false);
        check(find_move(pos, pos.move_to_uci(best)) != Move{}, "threads: legal move");
    }
    pos.set_fen("6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1");
    limits.depth = 8;
    check(pos.move_to_uci(pool.search(pos, limits, {}, false)) == "d1d8",
          "threads: back-rank mate");
    // A stop before the search: the helpers stop with the main searcher.
    pos.set_startpos();
    limits.depth = 64;
    pool.request_stop();
    pool.search(pos, limits, {}, false);
    pool.clear_stop();
    pool.set_threads(1);
    check(pool.threads() == 1, "threads: back to one");
}

// Pondering: a "go ponder" search ignores its clock until the ponderhit,
// then keeps to it, counted from the ponderhit; a stop ends it too.
void test_ponder() {
    using namespace std::chrono;
    Position pos;
    Searcher s(16);
    SearchLimits limits;
    limits.ponder = true;
    limits.movetime = 50;  // + Move Overhead: well under 100 ms once on the clock
    std::atomic<bool> done{false};
    Move best{};
    std::thread t([&] {
        best = s.search(pos, limits, {}, false);
        done = true;
    });
    std::this_thread::sleep_for(milliseconds(300));
    check(!done, "ponder: no limits before the ponderhit");
    const auto hit = steady_clock::now();
    s.ponderhit();
    while (!done && steady_clock::now() - hit < seconds(5))
        std::this_thread::sleep_for(milliseconds(1));
    t.join();
    const auto after = duration_cast<milliseconds>(steady_clock::now() - hit).count();
    check(after < 1000, "ponder: on the clock after the ponderhit (" + std::to_string(after) +
                            " ms)");
    // The move to ponder on next is legal after the best move.
    const Move reply = s.ponder_move(pos, best);
    StateInfo st;
    pos.make_move(best, st);
    check(reply != Move{} && pos.is_legal(reply), "ponder: a legal ponder move");
    pos.unmake_move(best, st);

    // Pondering on a move the opponent doesn't play: "stop".
    s.clear_ponderhit();
    done = false;
    std::thread t2([&] {
        s.search(pos, limits, {}, false);
        done = true;
    });
    std::this_thread::sleep_for(milliseconds(100));
    s.request_stop();
    t2.join();
    s.clear_stop();
    check(done.load(), "ponder: stopped");
}

// A clock with a millisecond or none left (or run below zero, which the
// UCI layer turns into 0) still means a clock: the search stops almost at
// once with a legal move, instead of running without a limit.
void test_empty_clock() {
    for (const int left : {0, 1}) {
        Position pos;
        Searcher s(16);
        SearchLimits limits;
        limits.time[0] = limits.time[1] = left;
        limits.movestogo = 1;
        const auto start = std::chrono::steady_clock::now();
        const Move best = s.search(pos, limits, {}, false);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        const std::string what = "clock " + std::to_string(left) + " ms";
        check(find_move(pos, pos.move_to_uci(best)) != Move{}, what + ": legal move");
        check(ms < 1000, what + ": search stopped (took " + std::to_string(ms) + " ms)");
    }
}

// FENs without the two clock fields (as in EPD) default them to 0 1; a
// clock field that isn't a number is an invalid FEN.
void test_fen_clocks() {
    Position pos;
    check(pos.set_fen("r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq -"),
          "fen: 4 fields accepted");
    check(pos.fen() == "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 1",
          "fen: missing clocks default to 0 1");
    check(pos.set_fen("8/8/8/8/8/8/8/K1k5 w - - 12"), "fen: 5 fields accepted");
    check(pos.halfmove_clock() == 12, "fen: halfmove clock read without full-move number");
    Position other;
    check(!other.set_fen("8/8/8/8/8/8/8/K1k5 w - - moves e2e4"), "fen: non-numeric clock rejected");
}

// One reversible move joins two positions (upcoming-repetition detection).
void test_reversible_move() {
    auto key = [](const char* fen) {
        Position p;
        p.set_fen(fen);
        return p;
    };
    // A black rook between a8 and d8, the other side to move: a match while
    // the path is clear, none with a piece on b8 in between.
    const Position a = key("r5k1/8/8/8/8/8/8/6K1 w - - 0 1");
    const Position b = key("3r2k1/8/8/8/8/8/8/6K1 b - - 0 1");
    check(a.reversible_move_to(b.key()) && b.reversible_move_to(a.key()),
          "repetition: rook move, clear path");
    const Position c = key("rN4k1/8/8/8/8/8/8/6K1 w - - 0 1");
    const Position d = key("1N1r2k1/8/8/8/8/8/8/6K1 b - - 0 1");
    check(!c.reversible_move_to(d.key()), "repetition: rook move, blocked path");
    // Two moves apart, or a pawn move: no match.
    const Position e = key("6k1/1r6/8/8/8/8/8/6K1 b - - 0 1");
    check(!a.reversible_move_to(e.key()), "repetition: not one move");
    const Position f = key("6k1/8/8/8/8/8/4P3/6K1 w - - 0 1");
    const Position g = key("6k1/8/8/8/8/4P3/8/6K1 b - - 0 1");
    check(!f.reversible_move_to(g.key()), "repetition: pawn moves aren't reversible");

    // In a game: Kg1-h1, Kg8-h8, Kh1-g1 leaves Black a move (Kh8-g8) from
    // the position three plies back.
    Position p;
    p.set_fen("6k1/8/8/8/8/8/8/R5K1 w - - 0 1");
    const std::uint64_t start = p.key();
    StateInfo st[3];
    const char* moves[3] = {"g1h1", "g8h8", "h1g1"};
    for (int i = 0; i < 3; ++i) p.make_move(find_move(p, moves[i]), st[i]);
    check(p.reversible_move_to(start), "repetition: king shuffle");
}

// The move picker hands out every legal move exactly once, whatever the
// hash move and killers are: legal ones, illegal ones from another
// position (as a hash collision could supply), or none. Perft never uses
// the picker, so it wouldn't notice a picker that drops a move or yields
// one twice (at the boundary between good and bad captures, say).
void test_picker_completeness() {
    const char* fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p2/4P3/6P1/8 w - - 0 1",
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
        "r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4",
        "4k3/8/8/8/8/8/4r3/4K3 w - - 0 1",
        "1k6/8/8/3q4/8/2N1B3/8/1K6 w - - 0 1",
    };
    auto sorted = [](std::vector<Move> v) {
        std::sort(v.begin(), v.end(), [](Move a, Move b) { return a.data < b.data; });
        return v;
    };
    auto legal_of = [](const Position& p) {
        MoveList moves;
        int count = 0;
        generate_legal(p, moves, count);
        return std::vector<Move>(moves.begin(), moves.begin() + count);
    };
    Searcher s(1);
    Position other;
    other.set_fen(fens[1]);
    const std::vector<Move> foreign = legal_of(other);  // mostly illegal elsewhere
    for (const char* fen : fens) {
        Position pos;
        pos.set_fen(fen);
        const std::vector<Move> legal = legal_of(pos);
        const std::vector<Move> expected = sorted(legal);
        std::vector<Move> quiets;
        for (Move m : legal)
            if (pos.piece_at(m.to()) == NO_PIECE && m.promotion() == PieceType::None) quiets.push_back(m);
        const Move k1 = quiets.size() > 0 ? quiets[0] : Move{};
        const Move k2 = quiets.size() > 1 ? quiets[1] : Move{};
        bool ok = sorted(s.picker_order(pos, Move{}, Move{}, Move{})) == expected;
        for (Move tt : legal) ok &= sorted(s.picker_order(pos, tt, k1, k2)) == expected;
        for (std::size_t i = 0; i + 2 < foreign.size(); i += 3)
            ok &= sorted(s.picker_order(pos, foreign[i], foreign[i + 1], foreign[i + 2])) == expected;
        check(ok, std::string("picker: every legal move exactly once in ") + fen);
    }

    // With QuietCheckBonus, and no history yet (every quiet scores alike),
    // the quiet checks come before the other quiets.
    const int bonus = params::quiet_check_bonus;
    params::quiet_check_bonus = 8000;
    Position pos;
    pos.set_fen("4k3/8/8/8/8/8/3N4/R3K3 w - - 0 1");
    const Position::CheckInfo ci = pos.check_info();
    bool checks_first = true, seen_plain = false;
    for (const Move m : Searcher(16).picker_order(pos, Move{}, Move{}, Move{})) {
        if (pos.piece_at(m.to()) != NO_PIECE) continue;
        const bool direct = ci.checking_from[pos.piece_at(m.from()) % 6] & bit(m.to());
        if (direct && seen_plain) checks_first = false;
        seen_plain |= !direct;
    }
    check(checks_first, "picker: quiet checks first with QuietCheckBonus");
    params::quiet_check_bonus = bonus;
}

void test_verification() {
    // Verify every pruning decision: counters must be consistent, and the
    // search must still return a legal move.
    Position pos;
    pos.set_fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    Searcher s(16);
    s.set_verify_rate(1);
    SearchLimits limits;
    limits.depth = 9;
    // Null move must get a chance at this depth whatever the tuned
    // minimum depth is (the tuned pruning left no null-move cutoffs at 7).
    const int nmp_min_depth = params::nmp_min_depth;
    params::nmp_min_depth = 3;
    const Move best = s.search(pos, limits, {}, false);
    params::nmp_min_depth = nmp_min_depth;
    check(find_move(pos, pos.move_to_uci(best)) != Move{}, "verification: legal best move");

    std::uint64_t verified = 0;
    for (int h = 0; h < prune::COUNT; ++h) {
        const auto& c = s.stats().c[static_cast<std::size_t>(h)];
        check(c.fired <= c.tried, std::string(prune::NAMES[h]) + ": fired <= tried");
        check(c.verified <= c.fired, std::string(prune::NAMES[h]) + ": verified <= fired");
        check(c.wrong <= c.verified, std::string(prune::NAMES[h]) + ": wrong <= verified");
        verified += c.verified;
    }
    check(s.stats().c[prune::LMR].fired > 0, "verification: LMR fired");
    check(s.stats().c[prune::NullMove].fired > 0, "verification: null move fired");
    check(verified > 0, "verification: decisions verified");
    // Capture reductions are a subset of all reductions.
    const auto& lmr = s.stats().c[prune::LMR];
    const auto& lmr_capture = s.stats().c[prune::LmrCapture];
    check(lmr_capture.tried <= lmr.tried && lmr_capture.fired <= lmr.fired &&
              lmr_capture.verified <= lmr.verified,
          "verification: lmr-capture within lmr");
    for (int k = 0; k < prune::FEATURE_COUNT; ++k) {
        const auto& f = s.stats().f[static_cast<std::size_t>(k)];
        check(f.fired <= f.tried, std::string(prune::FEATURE_NAMES[k]) + ": acted <= considered");
    }

    s.clear_stats();
    check(s.stats().c[prune::LMR].tried == 0, "stats cleared");
}

void test_label_sampling() {
    Position pos;
    pos.set_fen("r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10");
    Searcher s(16);
    std::vector<LabelSample> samples;
    s.set_label_sampling(&samples, {0, 4, 4, 4}, 12345);  // depths 1-3
    SearchLimits limits;
    limits.depth = 12;  // null-move subtrees reach depth 1 only from depth ~7 up
    s.search(pos, limits, {}, false);
    check(!samples.empty(), "labels: nodes sampled");
    bool any_null = false, any_pv = false;
    for (const LabelSample& ls : samples) {
        Position p;
        check(p.set_fen(ls.fen), "labels: sampled FEN parses");
        check(!p.in_check(p.side_to_move()), "labels: sample not in check");
        check(ls.depth >= 1 && ls.depth <= 3, "labels: depth within limit");
        check(ls.ply >= 1 && ls.alpha < ls.beta, "labels: ply and window");
        check((ls.node == prune::NodeType::PV) == (ls.beta - ls.alpha > 1),
              "labels: node type matches window");
        check(ls.static_eval - ls.correction == evaluate(p), "labels: raw eval matches the position");
        any_null |= ls.under_null;
        any_pv |= ls.node == prune::NodeType::PV;
    }
    check(any_null, "labels: some samples below a null move");
    check(any_pv, "labels: some PV samples");
}

// A label depends only on its position and depth, not on what the
// labeller searched before.
void test_label_rows() {
    const LabelSample a{"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
                        7, 3, 0, 0, 0, -10, 10, true, prune::NodeType::PV, false, false};
    const LabelSample b{"4rrk1/2p1b1p1/p1p3q1/4p3/2P2n1p/1P1NR2P/PB3PP1/3R1QK1 b - - 2 24",
                        5, 4, 0, 0, 0, 20, 21, false, prune::NodeType::Cut, true, false};
    Searcher fresh(2), used(2);
    const std::string alone = label_row(fresh, b);
    label_row(used, a);
    check(!alone.empty() && label_row(used, b) == alone, "labels: row independent of order");

    // Mate scores are kept, clipped, and flagged.
    const LabelSample m{"6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1", 3, 2, 0, 0, 0, -5, 5, true,
                        prune::NodeType::PV, false, false};
    const std::string row = label_row(fresh, m);
    std::vector<std::string> fields;
    for (std::size_t start = 0, end; start <= row.size(); start = end + 1) {
        end = row.find(',', start);
        if (end == std::string::npos) end = row.size();
        fields.push_back(row.substr(start, end - start));
    }
    check(fields.size() > 13 && fields[12] == std::to_string(LABEL_SCORE_CLIP) && fields[13] == "1",
          "labels: mate clipped and flagged");
}

void test_features() {
    Position pos;
    const features::Vector f = features::extract(pos);
    check(f[0] == 24, "features: start phase");
    check(f[1] == 0, "features: start material balance");
    check(f[2] == 3200 && f[3] == 3200, "features: start piece material");
    check(f[4] == 8 && f[5] == 8, "features: start pawns");
    check(f[6] == 0 && f[7] == 0 && f[8] == 0, "features: nothing hanging at the start");
    check(f[12] == 20, "features: start mobility");

    // Black's queen is attacked by a pawn; White can win it.
    pos.set_fen("4k3/8/8/3q4/4P3/8/8/4K3 w - - 0 1");
    const features::Vector g = features::extract(pos);
    check(g[7] == 900, "features: their queen threatened");
    check(g[8] == 900, "features: queen capture available");
    check(g[11] == 1, "features: passed pawn");
}

// Syzygy tablebases. Without tables (an empty path) nothing is probed; with
// the 3-4-5-piece tables in FUNCOMP_SYZYGY (CMake: -DSYZYGY_PATH=...),
// known results.
void test_tablebases() {
    Position pos;
    check(tb::init("") == 0 && tb::largest() == 0, "tablebases: empty path opens none");
    pos.set_fen("8/8/8/4k3/8/8/8/4KQ2 w - - 0 1");
    Searcher plain(16);
    SearchLimits limits;
    limits.depth = 6;
    plain.search(pos, limits, {}, false);
    check(plain.tb_hits() == 0, "tablebases: no probes without tables");

    const char* path = std::getenv("FUNCOMP_SYZYGY");
    if (!path || !*path) {
        std::cout << "Tablebase tests with tables skipped (FUNCOMP_SYZYGY not set).\n";
        return;
    }
    if (tb::init(path) < 5) {
        check(false, std::string("tablebases: no 5-piece tables in ") + path);
        return;
    }
    auto wdl = [&](const char* fen) {
        pos.set_fen(fen);
        return tb::probe_wdl(pos);
    };
    check(wdl("8/8/8/4k3/8/8/8/4KQ2 w - - 0 1") == tb::Wdl::Win, "tablebases: KQK won");
    check(wdl("8/8/8/4k3/8/8/8/4KQ2 b - - 0 1") == tb::Wdl::Loss, "tablebases: KQK lost");
    check(wdl("8/8/8/4k3/8/8/2b5/4KR2 w - - 0 1") == tb::Wdl::Draw, "tablebases: KRKB drawn");
    check(wdl("8/8/8/8/8/2k5/8/KBN5 w - - 0 1") == tb::Wdl::Win, "tablebases: KBNK won");
    check(wdl("8/8/8/4k3/8/8/2b5/4KR2 w - - 3 1") == tb::Wdl::Failed,
          "tablebases: no WDL probe with a running fifty-move count");

    // The same KBNK win with 90 half-moves gone: too slow for the rule.
    tb::RootRanking ranking;
    pos.set_fen("8/8/8/8/8/2k5/8/KBN5 w - - 90 1");
    check(tb::rank_root(pos, ranking) && !ranking.win && !ranking.loss,
          "tablebases: KBNK at 90 half-moves is a cursed win");
    pos.set_fen("8/8/8/8/8/2k5/8/KBN5 w - - 0 1");
    check(tb::rank_root(pos, ranking) && ranking.win && ranking.dtz,
          "tablebases: KBNK ranked a win by DTZ");
    // Every move matched, en passant included.
    pos.set_fen("8/8/8/k7/2Pp4/8/8/K7 b - c3 0 1");
    check(tb::rank_root(pos, ranking) &&
              std::any_of(ranking.moves.begin(), ranking.moves.end(),
                          [&](const tb::RankedMove& rm) { return rm.move.is_en_passant(); }),
          "tablebases: root ranking with an en passant capture");

    // Only one move wins (taking the bishop): the search plays it.
    pos.set_fen("8/8/8/4k3/8/8/3b4/4KR2 w - - 0 1");
    Searcher s(16);
    const Move best = s.search(pos, limits, {}, false);
    check(pos.move_to_uci(best) == "e1d2", "tablebases: the only winning move is played");

    // Six pieces at the root: the search probes once a capture leaves five.
    pos.set_fen("8/2p5/3k4/8/2r5/8/1Q2N3/K7 w - - 0 1");
    s.search(pos, limits, {}, false);
    check(s.tb_hits() > 0, "tablebases: probed inside the search");
    tb::init("");
}
}  // namespace

// LmrParentHist: a quiet reply of positive history to a quiet move of
// negative history is reduced less, the reverse more; zero is neutral, and
// the term is off at 0.
void test_lmr_parent_hist() {
    const int saved = params::lmr_parent_hist;
    prune::Context c;
    c.depth = 10;
    c.move_index = 10;
    c.quiet = true;
    c.parent_quiet = true;
    auto r = [&](int ours, int parents) {
        c.history = ours;
        c.parent_history = parents;
        return prune::lmr_reduction(c);
    };
    params::lmr_parent_hist = 0;
    const int base_pos = r(1, -1), base_neg = r(-1, 1);
    check(r(1, -1) == base_pos && r(-1, 1) == base_neg, "lmr parent hist: off, no change");
    params::lmr_parent_hist = 1024;
    check(r(1, -1) == base_pos - 1, "lmr parent hist: refutation reduced a ply less");
    check(r(-1, 1) == base_neg + 1, "lmr parent hist: poor reply reduced a ply more");
    check(r(0, -1) == r(0, 1) && r(1, 0) == base_pos && r(1, 1) == base_pos,
          "lmr parent hist: zero or same sign is neutral");
    c.parent_quiet = false;
    check(r(1, -1) == base_pos, "lmr parent hist: needs a quiet parent move");
    params::lmr_parent_hist = saved;
}

// Initiative: colour-symmetric (a position and its colour-flipped mirror
// score the same), only the tempo bonus at the start, and off at InitScale 0.
std::string flip_fen(const std::string& fen) {
    std::istringstream in(fen);
    std::string board, side, castling, ep, rest;
    in >> board >> side >> castling >> ep;
    std::getline(in, rest);
    std::vector<std::string> ranks;
    std::stringstream rs(board);
    for (std::string r; std::getline(rs, r, '/');) ranks.push_back(r);
    std::string flipped;
    for (auto it = ranks.rbegin(); it != ranks.rend(); ++it) {
        for (char ch : *it)
            flipped += std::isalpha(static_cast<unsigned char>(ch))
                           ? static_cast<char>(std::isupper(static_cast<unsigned char>(ch))
                                                   ? std::tolower(static_cast<unsigned char>(ch))
                                                   : std::toupper(static_cast<unsigned char>(ch)))
                           : ch;
        if (it + 1 != ranks.rend()) flipped += '/';
    }
    std::string cast;
    for (char ch : std::string("KQkq")) {
        const char from = std::isupper(static_cast<unsigned char>(ch))
                              ? static_cast<char>(std::tolower(static_cast<unsigned char>(ch)))
                              : static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (castling.find(from) != std::string::npos) cast += ch;
    }
    if (cast.empty()) cast = "-";
    if (ep != "-") ep[1] = ep[1] == '3' ? '6' : '3';
    return flipped + ' ' + (side == "w" ? "b" : "w") + ' ' + cast + ' ' + ep + rest;
}

void test_initiative() {
    const char* fens[] = {
        "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
        "r2q1rk1/pp2bppp/2n1pn2/3p4/2PP4/2N1PN2/PP2BPPP/R2Q1RK1 b - - 3 10",
        "2kr3r/ppq2ppp/2n1bn2/2b1p3/4P3/2N1BN2/PPPQBPPP/2KR3R w - - 6 12",
        "6k1/5ppp/8/3N4/8/8/5PPP/3R2K1 w - - 0 30",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "rnbqkb1r/pp1p1ppp/4pn2/2p5/2PP4/2N5/PP2PPPP/R1BQKBNR w KQkq c6 0 4",
        "4r1k1/1q3ppp/p7/1p1Q4/3n4/P4N2/1P3PPP/2R3K1 b - - 1 25",
    };
    for (const char* fen : fens) {
        Position a, b;
        a.set_fen(fen);
        b.set_fen(flip_fen(fen));
        check(initiative::evaluate(a) == initiative::evaluate(b),
              std::string("initiative: mirror-symmetric (") + fen + ")");
        check(evaluate(a) == evaluate(b), std::string("eval: mirror-symmetric (") + fen + ")");
    }
    Position start;
    check(initiative::evaluate(start) == params::init_tempo * params::init_scale / 100,
          "initiative: only the tempo bonus at the start");
    const int scale = params::init_scale;
    params::init_scale = 0;
    Position p;
    p.set_fen(fens[1]);
    check(initiative::evaluate(p) == 0, "initiative: off at InitScale 0");
    params::init_scale = scale;
    // The endgame taper: at phase 3 (a knight and a rook), 50% at phase 0
    // keeps (3 * 100 + 21 * 50) / 2400 of the untapered term.
    Position end;
    end.set_fen(fens[3]);
    const int pct = params::init_endgame_pct;
    params::init_endgame_pct = 100;
    const int full = initiative::evaluate(end);
    params::init_endgame_pct = 50;
    check(initiative::evaluate(end) == full * 1350 / 2400, "initiative: endgame taper");
    Position endm;
    endm.set_fen(flip_fen(fens[3]));
    check(initiative::evaluate(end) == initiative::evaluate(endm), "initiative: taper symmetric");
    params::init_endgame_pct = pct;
}

// Drawishness: opposite-coloured bishops and close pawnless material scale
// the eval toward a draw; same-coloured bishops and queen against rook (at
// a margin of 400, the hand-set one) don't.
void test_drawish() {
    const int ocb = params::draw_ocb_pct, pl = params::draw_pawnless_pct;
    const int margin = params::draw_pawnless_margin;
    params::draw_pawnless_margin = 400;
    auto eval_at = [](const char* fen, int pct_ocb, int pct_pl) {
        params::draw_ocb_pct = pct_ocb;
        params::draw_pawnless_pct = pct_pl;
        Position p;
        p.set_fen(fen);
        return evaluate(p);
    };
    const char* opposite = "4k3/pp3b2/8/8/8/8/PPP5/2B1K3 w - - 0 40";   // c1 dark, f7 light
    const char* same = "4k3/pp6/3b4/8/8/8/PPP5/2B1K3 w - - 0 40";       // c1 dark, d6 dark
    const char* rook_bishop = "4k3/8/8/3b4/8/8/8/R3K3 w - - 0 60";
    const char* queen_rook = "4k3/8/8/3r4/8/8/8/Q3K3 w - - 0 60";
    const int o100 = eval_at(opposite, 100, 100);
    check(eval_at(opposite, 50, 100) == o100 * 50 / 100, "drawish: opposite bishops halved");
    check(eval_at(same, 50, 100) == eval_at(same, 100, 100), "drawish: same-coloured bishops not");
    const int rb = eval_at(rook_bishop, 100, 100);
    check(eval_at(rook_bishop, 100, 50) == rb * 50 / 100, "drawish: rook vs bishop halved");
    check(eval_at(queen_rook, 100, 50) == eval_at(queen_rook, 100, 100),
          "drawish: queen vs rook not");
    params::draw_ocb_pct = ocb;
    params::draw_pawnless_pct = pl;
    params::draw_pawnless_margin = margin;
}

// Backrank Bingo: 324 distinct legal starts, the standard one among them;
// king and rooks at home, bishops on opposite colours, 20 legal moves.
void test_bingo() {
    const std::vector<std::string> starts = bingo_starts();
    check(starts.size() == 324, "bingo: 324 starts");
    std::set<std::string> distinct(starts.begin(), starts.end());
    check(distinct.size() == 324, "bingo: all distinct");
    check(distinct.count("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") == 1,
          "bingo: the standard start is one of them");
    int ok = 0;
    for (const std::string& fen : starts) {
        Position pos;
        if (!pos.set_fen(fen)) continue;
        MoveList moves;
        int count = 0;
        generate_legal(pos, moves, count);
        bool good = count == 20 && pos.fen() == fen;
        for (int c = 0; c < 2; ++c) {
            const int rank = c == 0 ? 0 : 7, base = c * 6;
            const Bitboard bishops = pos.pieces(static_cast<Piece>(base + 2));
            int light = 0;
            for (Bitboard b = bishops; b; b &= b - 1) {
                const int s = std::countr_zero(b);
                light += (file_of(s) + rank_of(s)) % 2;
                good = good && rank_of(s) == rank;
            }
            good = good && std::popcount(bishops) == 2 && light == 1;
            good = good && pos.pieces(static_cast<Piece>(base + 5)) == bit(sq(4, rank)) &&
                   pos.pieces(static_cast<Piece>(base + 3)) == (bit(sq(0, rank)) | bit(sq(7, rank)));
        }
        ok += good;
    }
    check(ok == 324, "bingo: legal, 20 moves, kings and rooks home, bishops opposite (" +
                         std::to_string(ok) + " of 324)");
}

// Continuation logging: off in normal builds (genconlog refuses); in a
// CONT_LOG build a short run writes nodes and rows that add up.
void test_cont_log() {
    ContLogOptions opt;
    opt.out = "cont_log_test";
    opt.games = 1;
    opt.random_plies = 4;
    opt.nodes = 4000;
    opt.rate = 4;
    std::ostringstream log;
    const std::uint64_t rows = generate_cont_log(opt, log);
    if (!CONT_LOG_BUILD) {
        check(rows == 0, "cont log: compiled out, genconlog refuses");
        return;
    }
    check(rows > 0, "cont log: rows written");
    std::ifstream nodes("cont_log_test.nodes.csv");
    std::string line;
    std::getline(nodes, line);
    std::uint64_t sum = 0;
    while (std::getline(nodes, line)) sum += std::stoull(line.substr(line.rfind(',') + 1));
    check(sum == rows, "cont log: the nodes' row counts add up");
}

// The learned-guide hook (prune::guide): a guide that returns the hand value
// leaves the search exactly as it is; every guided heuristic consults it,
// with a valid context; and its answers are held to the allowed band.
int guide_calls[prune::COUNT];
int guide_bad_context = 0;
int identity_guide(prune::Heuristic h, const prune::Context& c, int hand) {
    ++guide_calls[h];
    if (!c.pos || c.depth < 0) ++guide_bad_context;
    return hand;
}
int extreme_guide(prune::Heuristic, const prune::Context&, int hand) { return hand + 100000; }

void test_guide() {
    auto nodes_at = [](const char* fen) {
        Position pos;
        pos.set_fen(fen);
        Searcher s(16);
        s.new_game();
        SearchLimits limits;
        limits.depth = 10;
        s.search(pos, limits, {}, false);
        return s.nodes();
    };
    const char* fen = "r1bq1rk1/pp2bppp/2n1pn2/3p4/2PP4/2N1PN2/PP2BPPP/R2QKB1R w KQ - 2 8";
    const std::uint64_t plain = nodes_at(fen);
    std::fill(std::begin(guide_calls), std::end(guide_calls), 0);
    prune::guide = identity_guide;
    const std::uint64_t guided = nodes_at(fen);
    prune::guide = nullptr;
    check(guided == plain, "guide: an identity guide leaves the search unchanged (" +
                               std::to_string(guided) + " vs " + std::to_string(plain) + ")");
    for (const prune::Heuristic h : {prune::RFP, prune::Razor, prune::NullMove, prune::LMP,
                                     prune::Futility, prune::HistoryPrune, prune::SeeQuiet,
                                     prune::SeeCapture, prune::QsSee, prune::LMR})
        check(guide_calls[h] > 0, std::string("guide: consulted for ") + prune::NAMES[h]);
    check(guide_bad_context == 0, "guide: always given a position and a depth");

    Position pos;
    pos.set_fen(fen);
    prune::Context c;
    c.pos = &pos;
    c.depth = 5;
    c.move_index = 10;
    c.quiet = true;
    const int rfp = prune::rfp_margin(c);
    const int lmr = prune::lmr_reduction(c);
    prune::guide = extreme_guide;
    const int w = std::max(std::abs(rfp) * params::guide_margin_pct / 100, params::guide_margin_min);
    check(prune::rfp_margin(c) == rfp + w, "guide: a margin held to its band");
    check(prune::lmr_reduction(c) == lmr + params::guide_reduction_delta,
          "guide: a reduction held to its band");
    prune::guide = nullptr;
}

int main() {
    test_see();
    test_mop_up();
    test_null_move();
    test_mates();
    test_pv_stops_at_draws();
    test_stop_request();
    test_lmr_parent_hist();
    test_initiative();
    test_drawish();
    test_bingo();
    test_cont_log();
    test_guide();
    test_thread_pool();
    test_pgn();
    test_ponder();
    test_tt_key();
    test_root_move_nodes();
    test_empty_clock();
    test_fen_clocks();
    test_reversible_move();
    test_picker_completeness();
    test_verification();
    test_label_sampling();
    test_label_rows();
    test_features();
    test_tablebases();
    if (failures) {
        std::cerr << failures << " search test(s) failed.\n";
        return 1;
    }
    std::cout << "All search tests passed.\n";
    return 0;
}
