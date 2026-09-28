#include "eval.h"
#include "guide_features.h"
#include "labels.h"
#include "movegen.h"
#include "position.h"
#include "search.h"
#include "see.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
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
}

void test_verification() {
    // Verify every pruning decision: counters must be consistent, and the
    // search must still return a legal move.
    Position pos;
    pos.set_fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    Searcher s(16);
    s.set_verify_rate(1);
    SearchLimits limits;
    limits.depth = 7;
    const Move best = s.search(pos, limits, {}, false);
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
}  // namespace

int main() {
    test_see();
    test_null_move();
    test_mates();
    test_pv_stops_at_draws();
    test_stop_request();
    test_empty_clock();
    test_fen_clocks();
    test_reversible_move();
    test_picker_completeness();
    test_verification();
    test_label_sampling();
    test_label_rows();
    test_features();
    if (failures) {
        std::cerr << failures << " search test(s) failed.\n";
        return 1;
    }
    std::cout << "All search tests passed.\n";
    return 0;
}
