#pragma once
#include "params.h"
#include "types.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iosfwd>
#include <span>

class Position;

// Margins, thresholds and reductions for the search's pruning heuristics.
//
// The search never reads params:: directly for these. It fills in a
// Context describing the node (and, for move-level heuristics, the move)
// and asks one of the functions below. Each computes the hand-written
// formula from params.h. If a learned guide is installed, the guide sees
// the context and the hand value, and its answer is used instead, clamped
// to a window around the hand value so that a bad model can only bend the
// search, not break it.
namespace prune {

enum class NodeType : std::uint8_t { PV, Cut, All };

enum Heuristic : int {
    RFP,         // reverse futility pruning
    Razor,       // razoring
    NullMove,    // null-move pruning
    LMP,         // late move pruning
    Futility,    // futility pruning of quiet moves
    SeeQuiet,    // SEE pruning of quiet moves
    SeeCapture,  // SEE pruning of captures
    LMR,         // late move reductions
    QsSee,       // SEE pruning in quiescence
    ProbCut,     // ProbCut: a shallow search of good captures beats a raised beta
    HistoryPrune,  // pruning of quiet moves with bad history
    QsDelta,     // delta pruning in quiescence
    QsEvasion,   // quiet evasions in quiescence, once one was searched
    AlphaTtCut,  // a shallower hash upper bound well below alpha: fail low
    COUNT
};

inline constexpr const char* NAMES[COUNT] = {
    "rfp", "razor", "nullmove", "lmp", "futility", "see-quiet", "see-capture", "lmr", "qs-see",
    "probcut", "history", "qs-delta", "qs-evasion", "alpha-tt",
};

struct Context {
    // Node.
    const Position* pos = nullptr;
    int depth = 0;
    int ply = 0;
    int correction = 0;    // size of the static eval's correction (0 in check)
    int static_eval = 0;   // side to move's view, corrected, and sharpened by the
                           // hash score when its bound allows; meaningless in check
    int alpha = 0;
    int beta = 0;
    bool improving = false;
    bool in_check = false;
    NodeType node = NodeType::All;
    bool tt_pv = false;    // on the PV now, or when its hash entry was stored
    bool tt_capture = false;  // the hash move is a capture

    // Move (move-level heuristics only).
    Move move{};
    int move_index = 0;    // position in the ordered move list, from 0
    int lmr_depth = 0;     // depth left after the base late-move reduction
    int history = 0;       // quiet history: butterfly plus the two continuation histories
    int capture_history = 0;  // capture history, for captures (0 otherwise)
    bool quiet = false;
    bool killer = false;
    bool gives_check = false;  // computed for quiet moves only; false for captures
    // A null move failed here, and its refutation (the threat) is known.
    bool threatened = false;
    // The move addresses that threat: it moves the threatened piece or
    // blocks the threatening line.
    bool evades_threat = false;
    // The move takes one of our pieces out of a static threat (see
    // Position::threatened_by_lesser).
    bool escapes_threat = false;

    // Extra features for a learned guide. Empty in the hand-written search;
    // a guide that needs more than the fields above computes them from
    // `pos` (see guide_features.h) or has the search fill this in.
    std::span<const float> features{};
};

// A learned guide: returns its own value for heuristic `h` at `ctx`, given
// the hand-written value. Installed by setting prune::guide.
using Guide = int (*)(Heuristic h, const Context& ctx, int hand_value);
inline Guide guide = nullptr;

namespace detail {
inline int guided(Heuristic h, const Context& c, int hand, int lo, int hi) {
    if (!guide) [[likely]] return hand;
    return std::clamp(guide(h, c, hand), lo, hi);
}
inline int guided_margin(Heuristic h, const Context& c, int hand) {
    const int w = std::max(std::abs(hand) * params::guide_margin_pct / 100,
                           params::guide_margin_min);
    return guided(h, c, hand, hand - w, hand + w);
}
inline int guided_reduction(Heuristic h, const Context& c, int hand) {
    return guided(h, c, hand, hand - params::guide_reduction_delta,
                  hand + params::guide_reduction_delta);
}

// Base late-move reductions by depth and move index, in 1/LMR_SCALE ply.
extern std::array<std::array<std::int32_t, 64>, 64> lmr_table;
}  // namespace detail

// Late-move reductions are summed in 1/LMR_SCALE ply, so that every term
// can be tuned in fractions of a ply, and rounded to whole plies once.
constexpr int LMR_SCALE = 1024;

// Rebuilds tables derived from params (the late-move reduction table).
// Call before each search, so that changed parameters take effect.
void init();

// Base late-move reduction for the index-th move at `depth`, in
// 1/LMR_SCALE ply.
inline int lmr_base_reduction_scaled(int depth, int move_index) {
    return detail::lmr_table[static_cast<std::size_t>(std::min(depth, 63))]
                            [static_cast<std::size_t>(std::min(move_index, 63))];
}

// The same in whole plies, for the depth estimates of move pruning.
inline int lmr_base_reduction(int depth, int move_index) {
    return lmr_base_reduction_scaled(depth, move_index) / LMR_SCALE;
}

// Prune the node if static_eval - margin >= beta.
inline int rfp_margin(const Context& c) {
    const int hand = params::rfp_margin * c.depth - (c.improving ? params::rfp_improving : 0) +
                     c.correction * params::corr_margin_pct / 100 +
                     (c.tt_pv ? params::rfp_tt_pv_margin : 0);
    return detail::guided_margin(RFP, c, hand);
}

// Try razoring if static_eval + margin < alpha.
inline int razor_margin(const Context& c) {
    const int hand = params::razor_base + params::razor_mult * c.depth * c.depth;
    return detail::guided_margin(Razor, c, hand);
}

// Depth reduction R for the null-move search (searched at depth - 1 - R).
inline int nmp_reduction(const Context& c) {
    // In 1/1024 ply (see params.h), each term still rounded down to whole
    // plies as before fractions.
    constexpr int scale = 1024;
    const int depth_term = c.depth * params::nmp_depth_mult / scale * scale;
    const int eval_term = (c.static_eval - c.beta) * params::nmp_eval_mult / 100000 * scale;
    const int r = params::nmp_base + depth_term + std::min(eval_term, params::nmp_eval_max);
    return detail::guided_reduction(NullMove, c, r / scale);
}

// Quiet moves from this move index on are skipped.
inline int lmp_threshold(const Context& c) {
    const int hand = (params::lmp_base + params::lmp_mult * c.depth * c.depth) / 100 *
                     (c.improving ? 100 : params::lmp_not_improving_pct) / 100;
    const int w = std::max(hand * params::guide_margin_pct / 100, 1);
    return detail::guided(LMP, c, hand, hand - w, hand + w);
}

// Skip a quiet move if static_eval + margin <= alpha.
inline int futility_margin(const Context& c) {
    const int hand = params::fp_base + params::fp_mult * c.lmr_depth +
                     c.correction * params::corr_margin_pct / 100;
    return detail::guided_margin(Futility, c, hand);
}

// Quiet moves with a combined history below this are skipped.
inline int history_prune_threshold(const Context& c) {
    const int hand = -params::hist_prune_mult * c.depth;
    return detail::guided_margin(HistoryPrune, c, hand);
}

// Skip a move whose static exchange falls below this.
inline int see_threshold(const Context& c) {
    if (c.quiet) {
        const int hand = -params::see_quiet_margin * c.lmr_depth * c.lmr_depth;
        return detail::guided_margin(SeeQuiet, c, hand);
    }
    // Captures: a margin per ply, shifted by the capture's history (a
    // capture that has worked may look worse on paper), at most by the
    // margin itself either way.
    const int margin = params::see_capture_margin * c.depth;
    const int shift =
        std::clamp(c.capture_history * params::see_capt_hist_mult / 65536, -margin, margin);
    const int hand = -margin - shift;
    return detail::guided_margin(SeeCapture, c, hand);
}

// Quiescence skips captures whose static exchange falls below this.
inline int qs_see_threshold(const Context& c) {
    return detail::guided_margin(QsSee, c, params::qs_see_threshold);
}

// Late-move reduction in plies; the caller clamps it to the depth left.
inline int lmr_reduction(const Context& c) {
    int r = lmr_base_reduction_scaled(c.depth, c.move_index);  // 1/LMR_SCALE ply
    if (!c.improving) r += params::lmr_not_improving;
    if (c.node == NodeType::Cut) r += params::lmr_cut_node;
    if (c.node == NodeType::PV) r -= params::lmr_pv_node;
    if (c.tt_pv && c.node != NodeType::PV) r -= params::lmr_tt_pv;
    if (c.killer) r -= params::lmr_killer;
    if (c.tt_capture) r += params::lmr_tt_capture;
    if (!c.quiet) r += params::lmr_bad_capture;  // a losing capture (see LmrCaptures)
    r -= static_cast<int>(static_cast<long long>(c.history) * params::lmr_hist_mult / 65536);
    if (c.threatened)
        r += c.evades_threat ? -params::lmr_threat_evade : params::lmr_threat_ignore;
    if (c.escapes_threat) r -= params::lmr_static_threat;
    // Truncated, as the base table used to be: with whole-ply adjustments
    // this gives the old whole-ply base reductions.
    return detail::guided_reduction(LMR, c, r / LMR_SCALE);
}

// How often each heuristic was tried (its preconditions held and its test
// was made), fired (it pruned or reduced), and, when verification is on,
// how many firings were checked by searching anyway and how many of those
// turned out wrong: the pruned node or move would have changed the result.
// For LMR, "fired" counts reductions and "wrong" a reduced search that
// failed low where the full-depth search would not have; re-searches
// after a reduced fail high are counted separately.
struct Stats {
    struct Counter {
        std::uint64_t tried = 0, fired = 0, verified = 0, wrong = 0;
    };
    std::array<Counter, COUNT> c{};
    std::uint64_t lmr_researches = 0;

    void clear() { *this = Stats{}; }
    // One "info string" line per heuristic, as a table.
    void print(std::ostream& out) const;
};

}  // namespace prune
