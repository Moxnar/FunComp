#pragma once
#include "eval.h"
#include "position.h"
#include "pruning.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct SearchLimits {
    int depth = 0;           // 0 = no depth limit
    int movetime = 0;        // ms; 0 = not given
    int time[2] = {-1, -1};  // wtime, btime in ms; -1 = not given
    int inc[2] = {0, 0};
    int movestogo = 0;
    std::uint64_t nodes = 0; // 0 = no node limit
    bool infinite = false;
};

// Transposition table shared by all searches of a game. Entries store the
// best move found, a score that is exact or a bound on the true score, and
// the node's static evaluation so it needn't be recomputed on a revisit.
//
// Entries come in clusters of four, one 64-byte cache line: a position can
// live in any entry of its cluster, so a deep entry and a fresh shallow one
// needn't evict each other. Each entry records the generation (the search)
// that wrote it, and whether the position was on or near the principal
// variation (see Searcher::StackEntry::tt_pv). A position already in the
// cluster is updated in place,
// unless the stored entry is from this search and much deeper
// (params::tt_replace_margin) and the new score isn't exact. Otherwise the
// entry with the lowest depth - params::tt_age_weight * age (age: searches since it was
// written) makes room: stale entries first, then shallow ones.
class TranspositionTable {
public:
    enum Bound : std::uint8_t { NONE = 0, UPPER = 1, LOWER = 2, EXACT = 3 };

    struct Entry {
        std::uint64_t key = 0;
        Move move{};
        std::int16_t score = 0;
        std::int16_t eval = 0;  // static eval, or Searcher::NO_EVAL in check
        std::int8_t depth = 0;
        std::uint8_t gen_bound = 0;  // generation << 3 | pv << 2 | bound

        Bound bound() const { return static_cast<Bound>(gen_bound & 3); }
        int generation() const { return gen_bound >> 3; }
        bool is_pv() const { return gen_bound & 4; }
    };

    static constexpr int CLUSTER_SIZE = 4;
    struct alignas(64) Cluster {
        Entry entry[CLUSTER_SIZE];
    };

    explicit TranspositionTable(std::size_t megabytes = 64) { resize(megabytes); }
    void resize(std::size_t megabytes);
    void clear();
    // Call at the start of each search: entries written before become stale.
    void new_search() { generation_ = (generation_ + 1) & 31; }
    // Permille of the first 1000 entries written in the current search
    // (UCI "hashfull").
    int hashfull() const;

    // Starts fetching a cluster into cache: call as soon as a key is known,
    // so the memory access overlaps with other work before the probe.
    void prefetch(std::uint64_t key) const { __builtin_prefetch(&clusters_[key & mask_]); }

    const Entry* probe(std::uint64_t key) const {
        for (const Entry& e : clusters_[key & mask_].entry)
            if (e.key == key && e.bound() != NONE) return &e;
        return nullptr;
    }
    void store(std::uint64_t key, Move move, int score, int eval, int depth, Bound bound,
               bool pv);

private:
    std::vector<Cluster> clusters_;
    std::size_t mask_ = 0;
    int generation_ = 0;  // 5 bits, wrapping
};
static_assert(sizeof(TranspositionTable::Entry) == 16);
static_assert(sizeof(TranspositionTable::Cluster) == 64);

// A node picked for label logging: the position, and what the search knew
// about it when it got there. None of the context can be recovered from
// the FEN afterwards. Scores are from the side to move's point of view.
struct LabelSample {
    std::string fen;
    int depth;               // depth the node was about to be searched to
    int ply;                 // distance from the root
    int static_eval;         // as the search used it: corrected
    int correction;          // static_eval minus the raw evaluation
    int search_eval;         // what pruning decided with: static_eval, or the
                             // hash score when its bound makes it sharper
    int alpha, beta;         // the node's window in the tree
    bool improving;
    prune::NodeType node;
    bool under_null;         // a null move lies between the root and here
    bool iir;                // searched one ply shallower than `depth` (internal
                             // iterative reduction, after node-level pruning)
};

class Searcher {
public:
    static constexpr int INF = 32000;
    static constexpr int MATE = 30000;
    static constexpr int MAX_PLY = 128;
    static constexpr int MATE_BOUND = MATE - MAX_PLY;  // scores beyond this are mates
    static constexpr int NO_EVAL = 32001;              // "no static eval" (in check)

    explicit Searcher(std::size_t hash_mb = 64) : tt_(hash_mb) {}

    void resize_hash(std::size_t megabytes) { tt_.resize(megabytes); }
    void new_game();

    // Iterative-deepening search. `history` holds the Zobrist keys of every
    // position of the game before `pos` (oldest first), for repetition
    // detection. With `verbose`, prints UCI "info" lines per iteration.
    // Returns Move{} only if `pos` has no legal moves.
    // For tests: every move the move picker hands out at the root of `pos`,
    // in order, given a hash move and two killers (any of which may be
    // illegal, as a hash collision could supply, or Move{}).
    std::vector<Move> picker_order(const Position& pos, Move tt_move, Move killer1, Move killer2);

    Move search(Position& pos, const SearchLimits& limits,
                const std::vector<std::uint64_t>& history, bool verbose = true);

    // Asks a running search, from another thread, to stop as soon as it
    // can; search() then returns the best move found so far. The request
    // stays set until clear_stop(), which the caller does before starting
    // a search (not search() itself: a stop sent just after "go" must not
    // be lost).
    void request_stop() { stop_request_.store(true, std::memory_order_relaxed); }
    void clear_stop() { stop_request_.store(false, std::memory_order_relaxed); }
    bool stop_requested() const { return stop_request_.load(std::memory_order_relaxed); }

    std::uint64_t nodes() const { return nodes_; }
    // Score of the last completed iteration of the last search.
    int last_score() const { return last_score_; }

    // Pruning statistics, accumulated over searches until cleared.
    const prune::Stats& stats() const { return stats_; }
    void clear_stats() { stats_.clear(); }
    // Verify 1 in `rate` pruning decisions by searching anyway (0 = off).
    // For diagnostics only: verification searches share the hash table,
    // killers and history with the real search, so they change its moves,
    // scores and node counts, and the statistics gathered alongside them
    // describe that altered search.
    void set_verify_rate(int rate) {
        verify_rate_ = rate > 0 ? static_cast<std::uint64_t>(rate) : 0;
    }

    // Label logging: while `sink` is set, a main-search node at depth d
    // (not in check, not the root) is appended to it with probability
    // 1 / rate_by_depth[d]. A rate of 0, or a depth past the end of the
    // list, means never.
    void set_label_sampling(std::vector<LabelSample>* sink,
                            std::vector<std::uint64_t> rate_by_depth, std::uint64_t seed) {
        label_sink_ = sink;
        label_rates_ = std::move(rate_by_depth);
        rng_ = seed | 1;
    }

private:
    using Clock = std::chrono::steady_clock;

    // Per-ply search state. static_eval is recorded at every main-search
    // node (NO_EVAL in check) for pruning decisions to build on.
    struct StackEntry {
        int static_eval = NO_EVAL;
        Move move{};         // move being searched from this ply
        Piece piece = NO_PIECE;  // the piece making `move`; NO_PIECE for a null move
        bool quiet = false;      // `move` is a quiet move (no capture or promotion)
        Move killers[2]{};   // quiet moves that recently caused cutoffs here
        bool no_prune = false;  // next entry is a verification re-search: don't prune
        // Set when this node's null move failed low: the opponent's best
        // reply to the pass (from the null position's hash entry, Move{} if
        // it has none) and the null search's fail-soft score. threat_score
        // is NO_EVAL when no null move failed low here. Late move reductions
        // go easier on moves that address the threat.
        Move threat{};
        int threat_score = NO_EVAL;
        // Set around a singular-extension search of this node: the move to
        // leave out. Such a search is only a probe of the other moves, so it
        // neither cuts on nor writes to the hash table and doesn't prune.
        Move excluded{};
        // Singular extensions on the path from the root to this node, and
        // how many of them were double.
        int extensions = 0;
        int doubles = 0;
        // This node is on the principal variation, or its hash entry says it
        // was when it was stored (the entry's PV bit). Such nodes are
        // reduced less: they are where the score is decided.
        bool tt_pv = false;
    };

    // Continuation history: how well a quiet move (by its piece and
    // destination) has done in reply to an earlier move (also by piece and
    // destination), one ply back (countermove history) or two (follow-up
    // history). One table serves both. Same units and update rule as
    // history_. On the heap: 12 * 64 * 12 * 64 ints.
    using ContTable = std::array<int, 12 * 64>;  // indexed by the reply's piece * 64 + to
    std::vector<ContTable> cont_hist_ = std::vector<ContTable>(12 * 64, ContTable{});
    // The table for replies to the move made `back` plies before `ply`, or
    // nullptr if there is none (the root, or a null move).
    ContTable* cont_table(int ply, int back);
    static std::size_t piece_to(Piece p, int to) {
        return static_cast<std::size_t>(p) * 64 + static_cast<std::size_t>(to);
    }

    // Capture history: how well a capture has done, by moving piece,
    // destination and captured piece type. Same units and update rule as
    // history_.
    std::vector<int> capt_hist_ = std::vector<int>(12 * 64 * 6, 0);
    // Pawn history: quiet history by pawn structure (a slot of the pawn
    // key), moving piece and destination; same units and update rule as
    // history_. It joins quiet_history() weighted by params::pawn_hist_weight.
    static constexpr std::size_t PAWN_HIST_SIZE = 1024;
    std::vector<int> pawn_hist_ = std::vector<int>(PAWN_HIST_SIZE * 12 * 64, 0);
    static std::size_t pawn_hist_index(const Position& pos, Move m) {
        return (pos.pawn_key() & (PAWN_HIST_SIZE - 1)) * 12 * 64 +
               piece_to(pos.piece_at(m.from()), m.to());
    }
    static std::size_t capture_index(const Position& pos, Move m) {
        const Piece victim = m.is_en_passant() ? WP : pos.piece_at(m.to());
        return piece_to(pos.piece_at(m.from()), m.to()) * 6 + static_cast<std::size_t>(victim % 6);
    }

    TranspositionTable tt_;
    Evaluator eval_;
    std::array<StackEntry, MAX_PLY + 1> stack_{};
    // Quiet-move history, [side to move][from][to].
    std::array<std::array<std::array<int, 64>, 64>, 2> history_{};
    // Pawn correction history (see params.h): per side to move and pawn key
    // slot, the average of searched score minus raw static eval, in units
    // of 1/CORR_GRAIN centipawn. On the heap, being too big for a stack
    // that already holds a Searcher or two.
    static constexpr std::size_t CORR_SIZE = 16384;
    static constexpr int CORR_GRAIN = 256;
    std::vector<int> pawn_corr_ = std::vector<int>(2 * CORR_SIZE, 0);
    // Non-pawn correction history, the same per side to move and each
    // colour's non-pawn key: [side to move][colour][slot].
    std::vector<int> nonpawn_corr_ = std::vector<int>(2 * 2 * CORR_SIZE, 0);
    std::vector<std::uint64_t> keys_;  // game history + current search path
    std::uint64_t nodes_ = 0;
    std::uint64_t node_limit_ = 0;     // 0 = none
    bool stopped_ = false;
    std::atomic<bool> stop_request_{false};  // see request_stop()
    Clock::time_point start_;
    long long soft_ms_ = 0, hard_ms_ = 0;  // 0 = unlimited
    Move root_best_{};
    int root_best_score_ = 0;  // search score of root_best_
    int last_score_ = 0;
    int root_depth_ = 0;  // depth of the current iteration
    int seldepth_ = 0;    // deepest ply reached in it (UCI "seldepth")
    // First index of keys_ after the most recent null move on the search
    // path. Repetitions are only looked for from there on: a position
    // "repeated" across a null move was never reached over the board.
    std::size_t null_barrier_ = 0;

    prune::Stats stats_;
    std::uint64_t verify_rate_ = 0;  // 0 = no verification
    int verify_depth_ = 0;           // > 0 inside a verification search
    std::vector<LabelSample>* label_sink_ = nullptr;
    std::vector<std::uint64_t> label_rates_;  // by depth; see set_label_sampling
    std::uint64_t rng_ = 0x9e3779b97f4a7c15ULL;

    int negamax(Position& pos, int depth, int alpha, int beta, int ply, bool cut_node);
    int qsearch(Position& pos, int alpha, int beta, int ply);
    bool improving(int ply, int eval) const;
    int& correction_entry(const Position& pos) {
        return pawn_corr_[static_cast<std::size_t>(pos.side_to_move()) * CORR_SIZE +
                          (pos.pawn_key() & (CORR_SIZE - 1))];
    }
    int& nonpawn_entry(const Position& pos, Color c) {
        return nonpawn_corr_[(static_cast<std::size_t>(pos.side_to_move()) * 2 +
                              static_cast<std::size_t>(c)) * CORR_SIZE +
                             (pos.nonpawn_key(c) & (CORR_SIZE - 1))];
    }
    // The raw static eval plus the corrections for the position's pawns
    // and, weighted by params::corr_nonpawn_weight, its other pieces.
    int corrected_eval(const Position& pos, int raw);
    // The correction corrected_eval() adds (pawn and non-pawn tables), before
    // fifty-move scaling.
    int correction(const Position& pos);
    void update_correction(const Position& pos, int depth, int diff);

    // Statistics are not counted inside verification searches, which
    // would count the same decisions twice.
    std::uint64_t next_random();
    bool sample_verify();
    void count_tried(prune::Heuristic h) { if (!verify_depth_) ++stats_.c[h].tried; }
    void count_fired(prune::Heuristic h) { if (!verify_depth_) ++stats_.c[h].fired; }
    void record_verified(prune::Heuristic h, bool wrong);
    int verify_node(Position& pos, int depth, int alpha, int beta, int ply, bool cut_node);
    void verify_pruned_move(Position& pos, Move m, prune::Heuristic h, int depth, int alpha,
                            int ply);

    bool should_stop();
    bool is_draw(const Position& pos) const;
    // One reversible move from here reaches a position earlier on the
    // current search path (see negamax).
    bool repetition_in_reach(const Position& pos, int ply) const;
    long long elapsed_ms() const;
    void set_time_limits(const Position& pos, const SearchLimits& limits);
    // Hands out a node's moves one at a time, best first, generating them
    // in stages (see search.cpp) so that a node which cuts off early never
    // pays for the moves it didn't need.
    class MovePicker;

    // `cont` holds the continuation tables for this node (either may be
    // nullptr); quiet moves are scored by history plus both.
    void score_moves(const Position& pos, const MoveList& moves, int count, Move tt_move,
                     int ply, const ContTable* const cont[2],
                     std::array<int, 256>& scores) const;
    int quiet_history(const Position& pos, Move m, const ContTable* const cont[2]) const;
    void update_quiet_stats(const Position& pos, Move best, const Move* tried, int n_tried,
                            int depth, int ply);
    // After a cutoff: reward `best` if it is a capture (Move{} otherwise),
    // and penalize the captures searched before it.
    void update_capture_stats(const Position& pos, Move best, const Move* tried, int n_tried,
                              int depth);
    std::vector<Move> principal_variation(Position& pos, int max_len);
    // ProbCut's move loop (see negamax): the score of the first good
    // capture that beats pc_beta in a reduced search, and the move in
    // cut_move; below pc_beta if none does.
    int probcut(Position& pos, int depth, int pc_beta, int eval, Move tt_move, int ply,
                bool cut_node, Move& cut_move);
};
