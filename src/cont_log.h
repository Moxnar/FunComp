#pragma once
#include <cstdint>
#include <fstream>
#include <iosfwd>
#include <random>
#include <string>
#include <vector>

// Continuation logging: the groundwork for Try 4, a risk/reward oracle that
// asks, partway through a node's move loop, whether the rest is worth its
// cost (ROADMAP.md, "Continuation logging and oracle ceiling", where the
// columns were settled before any data). Off unless a ContLog is installed
// in a Searcher; then sampled nodes (not the root, not in a verification
// search or singular probe, at least min_depth plies left) record one row
// per move searched, with the search context as that move starts, and when
// the node finishes each row gets its outcome: the raw signed gain after
// it, the first later move that raised alpha, the nodes the rest cost, the
// node's bound. Nodes cut short (time, or a multi-cut return) are dropped.
// The position is stored once per node (nodes file); rows reference it.
// The root writes one row per completed iteration.
//
// Files: <prefix>.nodes.csv, <prefix>.rows.csv, <prefix>.root.csv and
// <prefix>.params (git commit, options, sampling rates, every tunable).

struct ContRow {
    // Move-list position.
    int index = 0;            // the move's place in the order (dropped quiets counted)
    int searched_before = 0;  // moves searched before it
    int stage = 0;            // the picker's stage when it was handed out
    int pruned[6] = {};       // pruned so far: LMP, history, futility, SEE quiet, SEE capture, underpromotion
    bool lmp_skipping = false;
    // The move itself, as an input, and its summary.
    int piece = 0, from = 0, to = 0, promotion = 0;
    bool capture = false, gives_check = false, killer = false, hash_move = false;
    int see = 0, history = 0, capture_history = 0, lmr_reduction = 0;
    // The loop so far.
    int alpha = 0;
    int best_before = 0, second_before = 0;  // NO_SCORE when none yet
    int near_best = 0;           // searched moves within 25 cp of the best
    int raised_before = 0;       // moves that raised alpha so far
    int researches_before = 0;   // LMR re-searches so far
    std::uint64_t nodes_before = 0;  // nodes spent in the node before this move
    // Filled once the move has been searched.
    int score = 0;
    bool raised = false;
};

struct ContNode {
    std::string fen;
    // Horizon and how the node was reached.
    int depth = 0, ply = 0, root_depth = 0, seldepth = 0;
    int parent_reduction = 0;      // LMR reduction of the move that led here
    bool parent_provisional = false;  // a reduced or null-window test the parent repeats
    int extensions = 0;
    // Window, eval and context.
    int node_type = 0;  // 0 PV, 1 cut, 2 all
    int alpha = 0, beta = 0;
    int raw_eval = 0, static_eval = 0, eval = 0, correction = 0;
    bool improving = false, in_check = false, threat = false;
    bool tt_hit = false;
    int tt_depth = -1, tt_bound = 0, tt_score = 0;
    bool tt_move = false;
    int phase = 0, material = 0, halfmove = 0;
    // Parent context.
    int parent_index = 0;
    bool parent_quiet = false;
    int parent_history = 0;
    // The node's legal moves, by kind.
    int legal = 0, legal_captures = 0, legal_quiets = 0;
    std::uint64_t nodes_start = 0;
    int researches = 0;  // LMR re-searches so far, kept up to date by the search
    std::vector<ContRow> rows;
};

struct ContRoot {
    int depth = 0;
    std::string best;
    int score = 0, score_change = 0, stability = 0;
    std::uint64_t nodes = 0, iteration_nodes = 0;
    long long elapsed_ms = 0, soft_ms = 0, hard_ms = 0;
    int node_share = 0;  // percent of the root moves' nodes under the best move
};

class ContLog {
public:
    // Nodes with d plies left are sampled 1 in rate(d) = base_rate /
    // (depth_scale / 100)^(d - min_depth): deeper nodes are rarer in the
    // tree, so they are sampled more often to balance the depths.
    ContLog(const std::string& prefix, int min_depth, int base_rate, int depth_scale,
            std::uint64_t seed);
    bool ok() const { return nodes_ && rows_ && root_; }
    int min_depth() const { return min_depth_; }
    bool sample(int depth);
    void set_position(int game, int game_ply) {
        game_ = game;
        game_ply_ = game_ply;
    }
    void write_node(const ContNode& n, int final_best, int bound, std::uint64_t nodes_total);
    void write_root(const ContRoot& r);
    std::uint64_t nodes_written() const { return next_node_; }
    std::uint64_t rows_written() const { return rows_written_; }
    const std::vector<std::uint64_t>& rates() const { return rates_; }

    static constexpr int NO_SCORE = -32000;

private:
    std::ofstream nodes_, rows_, root_;
    int min_depth_;
    std::vector<std::uint64_t> rates_;  // by depth
    std::mt19937_64 rng_;
    int game_ = 0, game_ply_ = 0;
    std::uint64_t next_node_ = 0, rows_written_ = 0;
};

struct ContLogOptions {
    std::string out;            // file prefix (required)
    int games = 10;
    int random_plies = 8;
    std::uint64_t nodes = 50000;  // per move: fixed nodes, independent of machine load
    int min_depth = 3;          // AskMinDepth's starting value
    int rate = 64;              // 1 in this many nodes at min_depth
    int depth_scale = 200;
    std::uint64_t seed = 1;
    int hash_mb = 16;
};

// Self-play games with logging on, on-policy: the real search with its
// hash table and histories as they are (cleared between games). Returns
// the number of rows written.
std::uint64_t generate_cont_log(const ContLogOptions& opt, std::ostream& log);
