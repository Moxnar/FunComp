#pragma once
#include <cstdint>
#include <iosfwd>
#include <string>

class Searcher;
struct LabelSample;

// Label logging for the learned pruning guide.
//
// Driver searches (self-play games, or the positions in a file) run with
// label sampling on. A node with d plies left (not the root, not in check,
// d <= max_label_depth) is picked with probability 1 / rate(d), where
// rate(d) = sample_rate / (depth_scale / 100)^(d - 1): deeper nodes are
// much rarer in the tree, so they are sampled more often to balance the
// depths. Each picked node is recorded with its search context, then
// searched on its own to depth d by a separate searcher whose hash table
// and history are cleared first, so that a label depends only on its
// position and depth. One CSV row per node:
//
//   fen, depth, ply, static_eval, correction, search_eval, alpha, beta,
//   improving, node, under_null, iir, score, mate, <guide_features::NAMES...>
//
// All scores are from the side to move's view. static_eval is the node's
// static eval, correction history included; correction is how much of it
// came from correction history, a rough measure of how unreliable the raw
// eval has been in this pawn structure. search_eval is the eval pruning
// decided with: static_eval, or the hash score where its bound made it a
// sharper estimate. alpha and beta are the node's window in the driver's
// tree (so search_eval - beta is what reverse futility and null-move
// pruning compare with their margins); node is pv, cut or all; under_null is 1
// if the node lies below a null move. depth is the depth node-level pruning
// saw; iir is 1 if the node's moves were then searched a ply shallower
// (internal iterative reduction). score is the depth-d search result,
// clipped to +-LABEL_SCORE_CLIP, with mate = 1 when it was a mate score.
//
// The labelling search prunes with the current margins, so labels depend
// on them: each round of retuned margins calls for regenerated data. The
// parameters in force are written next to the CSV, in <out>.params.

inline constexpr int LABEL_SCORE_CLIP = 2000;

struct LabelOptions {
    std::string out;               // output CSV path (required)
    std::string in;                // file of FENs or EPD lines; empty = self-play
    int games = 100;               // self-play games, when `in` is empty
    int random_plies = 8;          // random opening moves per self-play game
    int depth = 10;                // depth of the driver searches
    int sample_rate = 256;         // 1 in this many depth-1 nodes
    int depth_scale = 200;         // percent: each ply deeper is sampled this much more
    int max_label_depth = 8;       // deepest node depth sampled
    std::uint64_t count = 0;       // stop after this many rows (0 = no limit)
    std::uint64_t seed = 1;
    int hash_mb = 16;              // driver searcher
    int label_hash_mb = 2;         // labelling searcher, cleared before every label
};

// Returns the number of rows written. Progress and errors go to `log` as
// UCI "info string" lines.
std::uint64_t generate_labels(const LabelOptions& opt, std::ostream& log);

// The CSV header, and one row: `sample` labelled with `labeller` (whose
// hash table and history are cleared first). Empty if the sample can't be
// labelled (bad FEN, or in check).
std::string label_header();
std::string label_row(Searcher& labeller, const LabelSample& sample);
