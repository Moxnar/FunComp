#pragma once
#include <vector>

// Search parameters, gathered in one place so they can be retuned when the
// evaluation changes (their right values depend on its scale). They are
// plain variables rather than constants so that a tuning build can set
// them through UCI options: configure with -DTUNE=ON and every entry in
// tunables() is advertised as a spin option, ready for SPSA.
//
// Of the pruning parameters, the search reads only the depth limits that
// decide whether a heuristic applies at all. Margins, thresholds and
// reductions are computed by the functions in pruning.h, which are where a
// learned guide takes over.
namespace params {

// Time management. The soft limit (no new iteration after it) is scaled
// by how settled the search looks, in percent: tm_stable_start with a best
// move that just changed, less tm_stable_step per iteration it has held,
// down to tm_stable_min; plus tm_drop_scale per 100 cp the score fell since
// the last iteration, at most tm_drop_max. The hard limit still caps it.
// The defaults turn the scaling off (always 100%): with 140/15/75/50/60 it
// tested at +1.4 +- 7.6 Elo at 8+0.08 (4022 games), so it waits for SPSA,
// preferably at a longer time control.
inline int tm_stable_start = 100;
inline int tm_stable_step = 0;
inline int tm_stable_min = 100;
inline int tm_drop_scale = 0;
inline int tm_drop_max = 0;

// Time kept in hand per move for GUI and operating-system latency, in
// milliseconds. Also the UCI option "Move Overhead" in every build.
inline int move_overhead = 30;

// The time allocated to a move: target = time left / tm_moves_to_go (when
// the GUI gives no moves-to-go) + tm_inc_pct percent of the increment. The
// hard limit is tm_hard_pct percent of the target, at most tm_hard_cap_pct
// percent of the time left plus the increment; no new iteration starts
// past tm_soft_pct percent of the target (scaled as above).
inline int tm_moves_to_go = 30;
inline int tm_inc_pct = 75;
inline int tm_hard_pct = 300;
inline int tm_hard_cap_pct = 50;
inline int tm_soft_pct = 50;

// Root aspiration windows.
inline int asp_min_depth = 4;   // first iteration searched with a window
inline int asp_delta = 25;      // initial half-width of the window, cp
inline int asp_widen_pct = 50;  // each failed window widens by this percent

// History heuristic. A quiet move that causes a cutoff at depth d gets a
// bonus of min(hist_bonus_mult * d * d, hist_bonus_max); quiets searched
// before it get the same amount as a penalty. Scores stay within
// +-hist_max, so they never outrank killers or captures.
inline int hist_bonus_mult = 32;
inline int hist_bonus_max = 1536;
inline int hist_max = 16384;
// The penalty for the quiet moves searched before the cutoff, likewise:
// min(hist_malus_mult * d * d, hist_malus_max). Capture history has its
// own bonus and penalty on the same pattern.
inline int hist_malus_mult = 32;
inline int hist_malus_max = 1536;
inline int capt_bonus_mult = 32;
inline int capt_bonus_max = 1536;
inline int capt_malus_mult = 32;
inline int capt_malus_max = 1536;
// Prior-countermove bonus: when every move of a node fails low, the
// opponent's quiet move that led there gets prior_cm_pct percent of the
// usual cutoff bonus in its butterfly and continuation histories. 0 turns
// it off.
inline int prior_cm_pct = 0;
// Capture history (same update rule) orders captures of equal victims;
// it enters the capture's score as history * capt_hist_mult / 1024 (1/8).
inline int capt_hist_mult = 128;
// Continuation history 4 plies back (same table and update rule) orders
// quiet moves, weighted cont_hist4_weight/256; the history that
// reductions and pruning read leaves it out. 0 turns it off, updates
// included. Off by default: at 128 it leaned negative (-3.1 +- 6.4 Elo,
// 5500 games).
inline int cont_hist4_weight = 0;

// Pawn history (quiet history by pawn structure) joins the quiet history
// that orders, reduces and prunes quiet moves, weighted pawn_hist_weight/256.
// 0 turns it off, updates included. Off by default: at 256 it leaned
// negative (-5.8 +- 8.3 Elo, 3224 games).
inline int pawn_hist_weight = 0;

// Transposition table replacement: an entry written in the current search
// is only overwritten by a search at most tt_replace_margin plies
// shallower (or an exact score for the same position).
inline int tt_replace_margin = 3;
// Replacement prefers the entry with the lowest depth - tt_age_weight * age.
inline int tt_age_weight = 8;
// A store from a node flagged ttPv may replace an entry of the same
// position up to tt_pv_replace_bonus plies deeper than others may: PV
// information is worth keeping fresh. 0 treats all stores alike (the
// default: at 2 it leaned negative, -4.9 +- 7.2 Elo, 4038 games).
inline int tt_pv_replace_bonus = 0;
// A node that fails low below a parent flagged ttPv inherits the flag
// (in its hash entry) when its depth exceeds tt_pv_inherit_depth. 64
// turns this off (the default: at 3 it was flat, -1.9 +- 7.5 Elo, 4038
// games).
inline int tt_pv_inherit_depth = 64;

// Pawn correction history. After each search of a node that isn't in
// check, the gap between its score and its raw static eval is folded into
// a running average kept per pawn structure (pawn key) and side to move;
// the average is added to later static evals in the same structure. Each
// update weighs min(depth + 1, corr_weight_max) / 256, and the correction
// is capped at +-corr_limit centipawns.
inline int corr_weight_max = 16;
inline int corr_limit = 400;
// Non-pawn correction history: the same per side to move and each
// colour's non-pawn pieces (king included), updated alike; their sum
// enters the eval weighted by corr_nonpawn_weight/256 (0 turns it off).
inline int corr_nonpawn_weight = 64;
// Reverse futility and futility margins grow by corr_margin_pct percent of
// the correction's size: where correction history moves the eval a lot,
// the eval is less sure. 0 turns it off (the default: at 50 it was flat,
// +2.1 +- 7.4 Elo, 4056 games).
inline int corr_margin_pct = 0;

// Fifty-move scaling: the corrected static eval is multiplied by
// (fifty_scale - halfmove clock) / fifty_scale, so it shrinks toward a
// draw as the clock runs (to half at 100 with 200) and the engine prefers
// moves that make progress. 0 turns it off.
inline int fifty_scale = 200;
// Correction history learns in unscaled units: the searched score is
// divided by the fifty-move scale before the raw eval is subtracted, so
// the scaling (applied after the correction) isn't learned as eval error.
// 0 learns the scaled score minus the raw eval (the default: at 1 it lost
// -9.2 +- 7.3 Elo, 4064 games; the leak may act as extra fifty-move pressure that
// FiftyScale was tuned with: retune the two together in 2.0).
inline int corr_unscale_fifty = 0;

// Upcoming-repetition detection (1: on, 0: off): a node from which one
// reversible move reaches an earlier position of its line is worth at
// least a draw.
inline int rep_in_reach = 1;

// Internal iterative reduction: a PV or cut node with no hash move and at
// least iir_min_depth plies left is searched one ply shallower. Without a
// hash move its ordering is poor, and the shallower search fills in one
// for the next iteration.
inline int iir_min_depth = 4;
inline int iir_reduction = 1;  // plies

// Check extensions: a checking move is searched a ply deeper if its static
// exchange is at least check_ext_see (0: checks that don't lose material).
inline int check_ext_see = 0;

// Singular extensions: at depth >= se_min_depth, if the hash move has a
// lower-bound or exact score from a search at most se_tt_depth_margin plies
// shallower, the other moves are searched to (depth - 1) / 2 against
// tt_score - se_margin/16 * depth. If none reaches it, the hash move is
// extended by a ply; if that bound is at least beta anyway, the node cuts.
// At most se_max_extensions singular extensions on any path from the root.
// At non-PV nodes, a hash move so singular that the others fall short by
// more than se_double_margin as well is extended by two plies instead, at
// most se_max_doubles times on a path (these count as one singular
// extension each towards se_max_extensions too).
// When the other moves do reach the bound but it is below beta, a hash move
// with a score of at least beta, or at a cut node, is searched se_negative
// plies shallower instead (a negative extension).
inline int se_min_depth = 8;
inline int se_tt_depth_margin = 3;
inline int se_margin = 32;  // in 1/16 cp per ply: 2 cp
inline int se_max_extensions = 4;
inline int se_double_margin = 25;
inline int se_max_doubles = 6;
inline int se_negative = 1;
// The probe searches the other moves (depth - 1) * se_probe_depth_pct / 100
// plies deep, and extensions stop past se_ply_mult times the root depth.
inline int se_probe_depth_pct = 50;
inline int se_ply_mult = 2;

// Reverse futility pruning (non-PV, depth <= rfp_max_depth): return the
// static eval if it beats beta by rfp_margin per ply, less rfp_improving
// when the eval is improving.
inline int rfp_max_depth = 8;
inline int rfp_margin = 75;
inline int rfp_improving = 60;
// At nodes flagged ttPv, the margin grows by rfp_tt_pv_margin: they may
// well be on the principal variation, where a static-eval cut is riskiest.
// 500 all but turns RFP off there; 0 treats them like any other node
// (the default: at 500 it leaned negative, -5.3 +- 7.4 Elo, 4052 games).
inline int rfp_tt_pv_margin = 0;

// Razoring (non-PV, depth <= razor_max_depth): if the static eval is
// razor_base + razor_mult * depth^2 below alpha, drop into quiescence and
// trust it when it confirms the fail low.
inline int razor_max_depth = 3;
inline int razor_base = 250;
inline int razor_mult = 200;

// Null-move pruning (non-PV, depth >= nmp_min_depth, eval >= beta).
// Reduction R, in 1/1024 ply: nmp_base + depth * nmp_depth_mult
// + min((eval - beta) * nmp_eval_mult / 100000 plies, nmp_eval_max), each
// term rounded down to whole plies for now, then the sum.
inline int nmp_min_depth = 3;
inline int nmp_base = 3072;
inline int nmp_depth_mult = 342;  // a ply per 3 of depth (exact below depth 128)
inline int nmp_eval_mult = 500;  // a ply per 200 cp
inline int nmp_eval_max = 3072;

// ProbCut (non-PV, depth >= probcut_min_depth): captures whose static
// exchange covers the gap from the eval to beta + probcut_margin (less
// probcut_improving when improving) are searched probcut_reduction plies
// shallower against that raised bound, after quiescence agrees; one that
// beats it cuts the node. probcut_min_depth 64 turns it off.
inline int probcut_min_depth = 5;
inline int probcut_margin = 100;
inline int probcut_improving = 40;
inline int probcut_reduction = 4;
// ProbCut is skipped when the hash table has a score below the raised bound
// from at least depth - probcut_tt_margin; its cuts are stored at that depth.
inline int probcut_tt_margin = 3;

// Alpha-side hash cut (non-PV, depth >= alpha_tt_min_depth): a hash entry
// from a search at most alpha_tt_depth_margin plies shallower whose upper
// bound is at least alpha_tt_margin below alpha fails the node low at once.
// alpha_tt_min_depth 64 turns it off (the default: at 5 it was flat, -0.7
// +- 6.1 Elo, 6016 games).
inline int alpha_tt_min_depth = 64;
inline int alpha_tt_depth_margin = 2;
inline int alpha_tt_margin = 100;

// Late move pruning (depth <= lmp_max_depth): stop trying quiet moves
// after (lmp_base + lmp_mult * depth^2) / 100 of them, and lmp_not_improving_pct
// percent of that when the eval is not improving. Moves that give check are
// spared unless lmp_checks is 1.
inline int lmp_max_depth = 8;
inline int lmp_base = 300;  // in 1/100 move
inline int lmp_not_improving_pct = 50;  // share of the count when not improving
inline int lmp_mult = 100;
inline int lmp_checks = 0;

// History pruning (lmr depth <= hist_prune_max_depth): skip a quiet move
// (not a check) whose combined history is below -hist_prune_mult * depth.
// hist_prune_max_depth -1 turns it off.
inline int hist_prune_max_depth = 5;
inline int hist_prune_mult = 1000;

// Futility pruning of quiet moves (reduced depth <= fp_max_depth): skip
// the move if static eval + fp_base + fp_mult * reduced depth <= alpha.
inline int fp_max_depth = 8;
inline int fp_base = 120;
inline int fp_mult = 100;

// SEE pruning (depth <= see_max_depth): skip captures losing more than
// see_capture_margin * depth, and quiets losing more than
// see_quiet_margin * reduced depth^2. Quiescence skips captures whose
// exchange falls below qs_see_threshold.
inline int see_max_depth = 8;
inline int see_capture_margin = 90;
inline int see_quiet_margin = 25;
inline int qs_see_threshold = 0;
// A capture's SEE pruning threshold is shifted by its capture history *
// see_capt_hist_mult / 65536 centipawns (at most see_capture_margin * depth
// either way): a capture that has often worked may lose more on paper. 0
// turns the shift off. Off by default: at 2048 (1/32) it tested flat (-0.8
// +- 6.4 Elo, 5510 games).
inline int see_capt_hist_mult = 0;
// Delta pruning in quiescence: skip a capture (not a check or promotion)
// when the stand-pat score plus the captured piece plus qs_delta_margin
// is still at most alpha. 32000 turns it off. Off by default: at 200 it
// tested marginal (+2.6 +- 6.4 Elo, LOS 78.5%, 5546 games).
inline int qs_delta_margin = 32000;
// In check, quiescence searches at most qs_quiet_evasions quiet evasions
// once a score that isn't mated is known (captures always). 256 turns
// the limit off.
inline int qs_quiet_evasions = 2;

// Late move reductions of quiet moves (and losing captures, see
// lmr_captures), from depth lmr_min_depth and move index lmr_min_moves
// (one later at PV nodes). The reduction is computed in 1/1024 ply
// (prune::LMR_SCALE) so that each term can be tuned finely, and truncated
// to whole plies at the end. Base reduction: lmr_base/100 + ln(depth) *
// ln(index) * lmr_mult/10^6 plies. Then, in 1/1024 ply: lmr_not_improving
// more when the eval isn't improving, lmr_cut_node more at cut nodes,
// lmr_pv_node less at PV nodes, lmr_killer less for killer moves, and
// history * lmr_hist_mult / 65536 less. Both scales are multipliers rather
// than divisors, so that SPSA's steps have roughly even effects. Adjustment
// terms (here and below) may be tuned below zero: SPSA may find that a term
// wants the opposite sign, e.g. killers reduced more rather than less.
inline int lmr_min_depth = 3;
inline int lmr_min_moves = 1;
inline int lmr_base = 78;
inline int lmr_mult = 439023;  // in millionths (hand value 444444 = 1/2.25)
inline int lmr_hist_mult = 8055;  // 1/1024 ply per 65536 of history (hand value 8192)
inline int lmr_not_improving = 1019;
inline int lmr_cut_node = 1028;
inline int lmr_pv_node = 1083;
inline int lmr_killer = 1067;
// With a known threat (see StackEntry::threat): reduce moves that address
// it lmr_threat_evade/1024 plies less, and moves that don't
// lmr_threat_ignore/1024 more. Off by default: with a ply each it tested
// neutral (-2.8 +- 10.1 Elo); kept as the hand-made baseline for a learned
// threat signal, and for SPSA.
inline int lmr_threat_evade = 0;
inline int lmr_threat_ignore = 0;
// Quiet moves that take a piece out of a static threat (attacked by a
// cheaper piece) are reduced lmr_static_threat/1024 plies less. Off by
// default: at a ply it leaned negative (stopped early); kept for research
// and SPSA.
inline int lmr_static_threat = 0;
// Non-PV nodes whose hash entry says they were on the principal variation
// (ttPv) are reduced lmr_tt_pv/1024 plies less: they may well return to it.
inline int lmr_tt_pv = 1008;
// When the hash move is a capture, quiet moves are reduced
// lmr_tt_capture/1024 plies more: the position's best line is likely
// tactical, and a quiet move is unlikely to beat it. At one ply it tested
// flat (-0.6 +- 6.5 Elo, 5558 games); the LMR SPSA moved it from 0 to 43.
inline int lmr_tt_capture = 43;
// After a reduced search fails high, the full re-search goes a ply deeper
// when it landed more than lmr_deeper_base + lmr_deeper_mult * reduction
// centipawns above alpha, and a ply shallower when it beat the best
// score so far by less than lmr_shallower (0 turns that off: the score
// is above alpha, and alpha is at least the best score). lmr_deeper_base
// 32000 (beyond any mate score) turns deepening off. At 35 + 5r and 5 it
// lost (-15.1 +- 8.9 Elo, H0, 3010 games): deepening fired on 19% of reduced
// fail-highs (fail-soft scores land far above alpha). The LMR SPSA, started
// nearly off at 300, left it there (296): rare, for huge swings only.
inline int lmr_deeper_base = 296;
inline int lmr_deeper_mult = 5;
inline int lmr_shallower = 0;
// Captures that lose material by static exchange (and neither check nor
// promote) are reduced like quiet moves when lmr_captures is 1, with
// lmr_bad_capture/1024 plies more (their history terms are zero). Other
// captures are never reduced. With the offset at 0 it tested flat (-0.9
// +- 6.5 Elo, 5514 games); the LMR SPSA (run with it on) set the offset to
// -574, just over half a ply less than a quiet move.
inline int lmr_captures = 1;
inline int lmr_bad_capture = -574;

// Clamps on a learned guide's output (see pruning.h). Margins may move by
// guide_margin_pct percent of the hand value, but at least guide_margin_min
// centipawns; reductions by guide_reduction_delta plies; the late-move
// pruning count by guide_margin_pct percent (at least one move). Not
// tunables: they bound a model, they don't play chess.
inline int guide_margin_pct = 50;
inline int guide_margin_min = 50;
inline int guide_reduction_delta = 2;

struct Tunable {
    const char* name;
    int* value;
    int min, max;
};

inline const std::vector<Tunable>& tunables() {
    static const std::vector<Tunable> list = {
        {"TmStableStart", &tm_stable_start, 50, 300},
        {"TmStableStep", &tm_stable_step, 0, 50},
        {"TmStableMin", &tm_stable_min, 25, 150},
        {"TmDropScale", &tm_drop_scale, 0, 200},
        {"TmDropMax", &tm_drop_max, 0, 200},
        {"TmMovesToGo", &tm_moves_to_go, 5, 100},
        {"TmIncPct", &tm_inc_pct, 0, 200},
        {"TmHardPct", &tm_hard_pct, 100, 1000},
        {"TmHardCapPct", &tm_hard_cap_pct, 10, 90},
        {"TmSoftPct", &tm_soft_pct, 10, 200},
        {"AspMinDepth", &asp_min_depth, 1, 16},
        {"AspDelta", &asp_delta, 5, 300},
        {"AspWidenPct", &asp_widen_pct, 10, 200},
        {"HistBonusMult", &hist_bonus_mult, 1, 256},
        {"HistBonusMax", &hist_bonus_max, 64, 8192},
        {"HistMax", &hist_max, 1024, 65536},
        {"HistMalusMult", &hist_malus_mult, 1, 256},
        {"HistMalusMax", &hist_malus_max, 64, 8192},
        {"CaptBonusMult", &capt_bonus_mult, 1, 256},
        {"CaptBonusMax", &capt_bonus_max, 64, 8192},
        {"CaptMalusMult", &capt_malus_mult, 1, 256},
        {"CaptMalusMax", &capt_malus_max, 64, 8192},
        {"PriorCmPct", &prior_cm_pct, 0, 400},
        {"CaptHistMult", &capt_hist_mult, 0, 1024},
        {"ContHist4Weight", &cont_hist4_weight, 0, 512},
        {"PawnHistWeight", &pawn_hist_weight, 0, 512},
        {"TtReplaceMargin", &tt_replace_margin, 0, 16},
        {"TtAgeWeight", &tt_age_weight, 0, 32},
        {"TtPvReplaceBonus", &tt_pv_replace_bonus, 0, 8},
        {"TtPvInheritDepth", &tt_pv_inherit_depth, 0, 64},
        {"CorrWeightMax", &corr_weight_max, 1, 128},
        {"CorrLimit", &corr_limit, 0, 1000},
        {"CorrNonPawnWeight", &corr_nonpawn_weight, 0, 512},
        {"CorrMarginPct", &corr_margin_pct, 0, 300},
        {"FiftyScale", &fifty_scale, 0, 1000},
        {"CorrUnscaleFifty", &corr_unscale_fifty, 0, 1},
        {"RepInReach", &rep_in_reach, 0, 1},
        {"IirMinDepth", &iir_min_depth, 2, 32},
        {"IirReduction", &iir_reduction, 0, 3},
        {"CheckExtSee", &check_ext_see, -1000, 500},
        {"SeMinDepth", &se_min_depth, 4, 32},
        {"SeTtDepthMargin", &se_tt_depth_margin, 0, 8},
        {"SeMargin", &se_margin, 0, 256},
        {"SeMaxExtensions", &se_max_extensions, 0, 16},
        {"SeDoubleMargin", &se_double_margin, 0, 300},
        {"SeMaxDoubles", &se_max_doubles, 0, 32},
        {"SeNegative", &se_negative, 0, 3},
        {"SeProbeDepthPct", &se_probe_depth_pct, 25, 75},
        {"SePlyMult", &se_ply_mult, 1, 4},
        {"RfpMaxDepth", &rfp_max_depth, 0, 16},
        {"RfpMargin", &rfp_margin, 10, 300},
        {"RfpImproving", &rfp_improving, 0, 200},
        {"RfpTtPvMargin", &rfp_tt_pv_margin, 0, 1000},
        {"RazorMaxDepth", &razor_max_depth, 0, 8},
        {"RazorBase", &razor_base, 0, 1000},
        {"RazorMult", &razor_mult, 0, 1000},
        {"NmpMinDepth", &nmp_min_depth, 1, 16},
        {"NmpBase", &nmp_base, 0, 8192},
        {"NmpDepthMult", &nmp_depth_mult, 0, 1024},
        {"NmpEvalMult", &nmp_eval_mult, 100, 4000},
        {"NmpEvalMax", &nmp_eval_max, 0, 8192},
        {"ProbCutMinDepth", &probcut_min_depth, 3, 64},
        {"ProbCutMargin", &probcut_margin, 0, 500},
        {"ProbCutImproving", &probcut_improving, -200, 200},
        {"ProbCutReduction", &probcut_reduction, 2, 8},
        {"ProbCutTtMargin", &probcut_tt_margin, 0, 8},
        {"AlphaTtMinDepth", &alpha_tt_min_depth, 2, 64},
        {"AlphaTtDepthMargin", &alpha_tt_depth_margin, 1, 8},
        {"AlphaTtMargin", &alpha_tt_margin, 0, 500},
        {"LmpMaxDepth", &lmp_max_depth, 0, 16},
        {"LmpBase", &lmp_base, 100, 2000},
        {"LmpNotImprovingPct", &lmp_not_improving_pct, 10, 100},
        {"LmpMult", &lmp_mult, 10, 400},
        {"LmpChecks", &lmp_checks, 0, 1},
        {"HistPruneMaxDepth", &hist_prune_max_depth, -1, 16},
        {"HistPruneMult", &hist_prune_mult, 0, 20000},
        {"FpMaxDepth", &fp_max_depth, 0, 16},
        {"FpBase", &fp_base, 0, 500},
        {"FpMult", &fp_mult, 10, 400},
        {"SeeMaxDepth", &see_max_depth, 0, 16},
        {"SeeCaptureMargin", &see_capture_margin, 0, 400},
        {"SeeQuietMargin", &see_quiet_margin, 0, 200},
        {"QsSeeThreshold", &qs_see_threshold, -500, 200},
        {"SeeCaptHistMult", &see_capt_hist_mult, 0, 8192},
        {"QsDeltaMargin", &qs_delta_margin, 0, 32000},
        {"QsQuietEvasions", &qs_quiet_evasions, 0, 256},
        {"LmrMinDepth", &lmr_min_depth, 1, 16},
        {"LmrMinMoves", &lmr_min_moves, 1, 16},
        {"LmrBase", &lmr_base, 0, 300},
        {"LmrMult", &lmr_mult, 150000, 1000000},
        {"LmrHistMult", &lmr_hist_mult, 0, 65536},
        {"LmrNotImproving", &lmr_not_improving, -2048, 3072},
        {"LmrCutNode", &lmr_cut_node, -2048, 3072},
        {"LmrPvNode", &lmr_pv_node, -2048, 3072},
        {"LmrKiller", &lmr_killer, -2048, 3072},
        {"LmrThreatEvade", &lmr_threat_evade, -2048, 4096},
        {"LmrThreatIgnore", &lmr_threat_ignore, -2048, 4096},
        {"LmrStaticThreat", &lmr_static_threat, -2048, 4096},
        {"LmrTtPv", &lmr_tt_pv, -2048, 4096},
        {"LmrTtCapture", &lmr_tt_capture, -2048, 3072},
        {"LmrDeeperBase", &lmr_deeper_base, 0, 32000},
        {"LmrDeeperMult", &lmr_deeper_mult, 0, 50},
        {"LmrShallower", &lmr_shallower, 0, 100},
        {"LmrCaptures", &lmr_captures, 0, 1},
        {"LmrBadCapture", &lmr_bad_capture, -2048, 3072},
    };
    return list;
}

}  // namespace params
