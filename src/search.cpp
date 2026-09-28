#include "search.h"
#include "eval.h"
#include "movegen.h"
#include "params.h"
#include "see.h"
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <bit>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

// ---------------------------------------------------------------------------
// Transposition table

void TranspositionTable::resize(std::size_t megabytes) {
    const std::size_t want = (megabytes << 20) / sizeof(Cluster);
    const std::size_t n = want ? std::bit_floor(want) : 1;
    clusters_.assign(n, Cluster{});
    mask_ = n - 1;
}

void TranspositionTable::clear() {
    std::fill(clusters_.begin(), clusters_.end(), Cluster{});
}

void TranspositionTable::store(std::uint64_t key, Move move, int score, int eval, int depth,
                               Bound bound, bool pv) {
    Cluster& c = clusters_[key & mask_];

    // The position's own entry if it has one; otherwise the least valuable
    // entry: an empty one, or the one with the lowest depth - weight * age
    // (params::tt_age_weight).
    Entry* slot = nullptr;
    for (Entry& e : c.entry)
        if (e.key == key && e.bound() != NONE) { slot = &e; break; }
    if (!slot) {
        int worst = INT_MAX;
        for (Entry& e : c.entry) {
            if (e.bound() == NONE) { slot = &e; break; }
            const int age = (generation_ - e.generation()) & 31;
            const int value = e.depth - params::tt_age_weight * age;
            if (value < worst) { worst = value; slot = &e; }
        }
    }
    Entry& e = *slot;

    // Keep a much deeper entry of the same position from this search: it
    // cost far more to produce. An exact score is worth having regardless.
    if (e.key == key && e.bound() != NONE && e.generation() == generation_ &&
        depth + params::tt_replace_margin + (pv ? params::tt_pv_replace_bonus : 0) < e.depth &&
        bound != EXACT)
        return;
    // Keep the old best move if this search of the same position found none
    // (every move failed low): it is still the best ordering guess we have.
    if (move == Move{} && e.key == key) move = e.move;
    e.key = key;
    e.move = move;
    e.score = static_cast<std::int16_t>(score);
    e.eval = static_cast<std::int16_t>(eval);
    e.depth = static_cast<std::int8_t>(depth);
    e.gen_bound = static_cast<std::uint8_t>(generation_ << 3 | (pv ? 4 : 0) | bound);
}

int TranspositionTable::hashfull() const {
    int used = 0, seen = 0;
    for (std::size_t c = 0; c < clusters_.size() && seen < 1000; ++c)
        for (const Entry& e : clusters_[c].entry) {
            used += e.bound() != NONE && e.generation() == generation_;
            ++seen;
        }
    return seen ? used * 1000 / seen : 0;
}

// ---------------------------------------------------------------------------
// Helpers

namespace {
using TT = TranspositionTable;

// Mate scores are stored relative to the node rather than the root, so an
// entry means the same thing wherever in the tree it is found again.
int score_to_tt(int score, int ply) {
    if (score > Searcher::MATE_BOUND) return score + ply;
    if (score < -Searcher::MATE_BOUND) return score - ply;
    return score;
}
int score_from_tt(int score, int ply) {
    if (score > Searcher::MATE_BOUND) return score - ply;
    if (score < -Searcher::MATE_BOUND) return score + ply;
    return score;
}

// A hash score is a better estimate of the node than its static eval when
// its bound says the true score lies beyond the eval on the score's side:
// a lower bound above the eval, an upper bound below it, or an exact score.
// Pruning decisions then use it. Mate scores are left out.
int sharpen_eval(int eval, int tt_score, TT::Bound bound) {
    if (bound == TT::NONE || std::abs(tt_score) >= Searcher::MATE_BOUND) return eval;
    const TT::Bound side = tt_score > eval ? TT::LOWER : TT::UPPER;
    return (bound & side) ? tt_score : eval;
}

// Move ordering: hash move, captures that don't lose material (by SEE) by
// victim value and then capture history, queen promotions, the two killer
// moves, the remaining quiet moves by history (butterfly and
// continuation), then losing captures, and underpromotions last.
constexpr int ORDER_VALUE[6] = {100, 320, 330, 500, 900, 0};
constexpr int TT_MOVE_SCORE = 10'000'000;
constexpr int CAPTURE_BASE = 1'000'000;
constexpr int QUEEN_PROMO_BONUS = 900'000;
constexpr int KILLER_1_SCORE = 800'000;
constexpr int KILLER_2_SCORE = 790'000;
constexpr int BAD_CAPTURE_BASE = -500'000;
constexpr int UNDERPROMO_SCORE = -1'000'000;

bool is_quiet(const Position& pos, Move m) {
    return pos.piece_at(m.to()) == NO_PIECE && !m.is_en_passant() &&
           m.promotion() == PieceType::None;
}

bool is_capture(const Position& pos, Move m) {
    return pos.piece_at(m.to()) != NO_PIECE || m.is_en_passant();
}

// Selection sort, one step at a time: brings the best remaining move to
// position i. Cheaper than a full sort because most nodes cut off early.
void pick_next(MoveList& moves, std::array<int, 256>& scores, int i, int count) {
    auto best = static_cast<std::size_t>(i);
    for (auto j = best + 1; j < static_cast<std::size_t>(count); ++j)
        if (scores[j] > scores[best]) best = j;
    std::swap(moves[static_cast<std::size_t>(i)], moves[best]);
    std::swap(scores[static_cast<std::size_t>(i)], scores[best]);
}

std::string score_string(int score) {
    if (score > Searcher::MATE_BOUND) return "mate " + std::to_string((Searcher::MATE - score + 1) / 2);
    if (score < -Searcher::MATE_BOUND) return "mate " + std::to_string(-(Searcher::MATE + score) / 2);
    return "cp " + std::to_string(score);
}
}

// ---------------------------------------------------------------------------
// Staged move picking
//
// The same order as scoring every legal move up front (see ORDER_VALUE
// above), produced in stages: the hash move before anything is generated;
// then captures and promotions are generated and scored, and handed out
// while they win material or promote to a queen; then the killers; then
// quiet moves are generated and handed out by history; then the losing
// captures and underpromotions left over from the capture stage. The hash
// move and killers are tried unseen, so each is checked for legality
// first, and skipped when it turns up again in its own stage.

class Searcher::MovePicker {
public:
    MovePicker(const Searcher& s, const Position& pos, Move tt_move, int ply,
               const ContTable* const cont[2], const ContTable* cont4)
        : s_(s), pos_(pos), tt_move_(tt_move), ply_(ply), cont_{cont[0], cont[1]}, cont4_(cont4) {
        const StackEntry& e = s.stack_[static_cast<std::size_t>(ply)];
        killers_[0] = e.killers[0];
        killers_[1] = e.killers[1];
    }

    // The move last handed out is a capture already known not to lose
    // material (its SEE >= 0 was computed when scoring it), so tests
    // against a threshold at or below 0 needn't run SEE again.
    bool good_capture() const { return good_capture_; }

    // The next move, or Move{} when there are no more.
    Move next() {
        good_capture_ = false;
        while (true) {
            switch (stage_) {
            case Stage::HashMove:
                stage_ = Stage::GenCaptures;
                if (tt_move_ != Move{} && pos_.is_legal(tt_move_)) return tt_move_;
                break;

            case Stage::GenCaptures:
                generate_captures(pos_, caps_, n_caps_);
                s_.score_moves(pos_, caps_, n_caps_, Move{}, ply_, cont_, cap_scores_);
                stage_ = Stage::GoodCaptures;
                break;

            case Stage::GoodCaptures:
                while (cap_i_ < n_caps_) {
                    pick_next(caps_, cap_scores_, cap_i_, n_caps_);
                    // Everything from here on loses material or underpromotes.
                    if (cap_scores_[static_cast<std::size_t>(cap_i_)] <= KILLER_1_SCORE) break;
                    const Move m = caps_[static_cast<std::size_t>(cap_i_++)];
                    if (m != tt_move_) {
                        // Scored above CAPTURE_BASE / 2: a capture that passed
                        // SEE (losing captures, even promoting, stay far below;
                        // a quiet queen promotion scores high without SEE).
                        good_capture_ =
                            is_capture(pos_, m) &&
                            cap_scores_[static_cast<std::size_t>(cap_i_ - 1)] >= CAPTURE_BASE / 2;
                        return m;
                    }
                }
                stage_ = Stage::Killer1;
                break;

            case Stage::Killer1:
            case Stage::Killer2: {
                const Move k = killers_[stage_ == Stage::Killer1 ? 0 : 1];
                stage_ = stage_ == Stage::Killer1 ? Stage::Killer2 : Stage::GenQuiets;
                if (k != Move{} && k != tt_move_ && is_quiet(pos_, k) && pos_.is_legal(k)) return k;
                break;
            }

            case Stage::GenQuiets:
                generate_quiets(pos_, quiets_, n_quiets_);
                for (int i = 0; i < n_quiets_; ++i) {
                    const Move m = quiets_[static_cast<std::size_t>(i)];
                    int score = s_.quiet_history(pos_, m, cont_);
                    if (cont4_)
                        score += (*cont4_)[piece_to(pos_.piece_at(m.from()), m.to())] *
                                 params::cont_hist4_weight / 256;
                    quiet_scores_[static_cast<std::size_t>(i)] = score;
                }
                stage_ = Stage::Quiets;
                break;

            case Stage::Quiets:
                while (quiet_i_ < n_quiets_) {
                    pick_next(quiets_, quiet_scores_, quiet_i_, n_quiets_);
                    const Move m = quiets_[static_cast<std::size_t>(quiet_i_++)];
                    if (m != tt_move_ && m != killers_[0] && m != killers_[1]) return m;
                }
                stage_ = Stage::BadCaptures;
                break;

            case Stage::BadCaptures:
                while (cap_i_ < n_caps_) {
                    pick_next(caps_, cap_scores_, cap_i_, n_caps_);
                    const Move m = caps_[static_cast<std::size_t>(cap_i_++)];
                    if (m != tt_move_) return m;
                }
                stage_ = Stage::Done;
                break;

            case Stage::Done:
                return Move{};
            }
        }
    }

private:
    enum class Stage {
        HashMove, GenCaptures, GoodCaptures, Killer1, Killer2, GenQuiets, Quiets,
        BadCaptures, Done
    };

    const Searcher& s_;
    const Position& pos_;
    Move tt_move_;
    int ply_;
    const ContTable* cont_[2];
    const ContTable* cont4_;  // replies to the move 4 plies back; ordering only
    Move killers_[2];
    bool good_capture_ = false;
    Stage stage_ = Stage::HashMove;

    MoveList caps_;
    std::array<int, 256> cap_scores_;
    int n_caps_ = 0, cap_i_ = 0;
    MoveList quiets_;
    std::array<int, 256> quiet_scores_;
    int n_quiets_ = 0, quiet_i_ = 0;
};

// ---------------------------------------------------------------------------
// Search

void Searcher::new_game() {
    tt_.clear();
    for (auto& side : history_)
        for (auto& from : side) from.fill(0);
    std::fill(pawn_corr_.begin(), pawn_corr_.end(), 0);
    std::fill(nonpawn_corr_.begin(), nonpawn_corr_.end(), 0);
    for (auto& t : cont_hist_) t.fill(0);
    std::fill(capt_hist_.begin(), capt_hist_.end(), 0);
    std::fill(pawn_hist_.begin(), pawn_hist_.end(), 0);
}

int Searcher::correction(const Position& pos) {
    const long long nonpawn =
        nonpawn_entry(pos, Color::White) + nonpawn_entry(pos, Color::Black);
    return correction_entry(pos) / CORR_GRAIN +
           static_cast<int>(nonpawn * params::corr_nonpawn_weight / (256LL * CORR_GRAIN));
}

int Searcher::corrected_eval(const Position& pos, int raw) {
    int corrected = raw + correction(pos);
    // Fifty-move scaling (see params.h): here rather than in the raw eval,
    // which the hash table keeps and whose key doesn't include the clock.
    if (params::fifty_scale)
        corrected = corrected * std::max(params::fifty_scale - pos.halfmove_clock(), 0) /
                    params::fifty_scale;
    return std::clamp(corrected, -MATE_BOUND + 1, MATE_BOUND - 1);
}

// Moves the corrections toward `diff` (searched score minus raw static
// eval), faster for deeper, more trustworthy searches. The tables add up,
// so each moves by the error that remains after all of them (not by its
// own distance to `diff`: then every table would learn the whole gap, and
// their sum would overshoot it). With the non-pawn weight at 0 this is the
// pawn table's plain running average.
void Searcher::update_correction(const Position& pos, int depth, int diff) {
    const int weight = std::min(depth + 1, params::corr_weight_max);
    const int limit = params::corr_limit * CORR_GRAIN;
    const long long target = static_cast<long long>(diff) * CORR_GRAIN;
    int& pawn = correction_entry(pos);
    int& white = nonpawn_entry(pos, Color::White);
    int& black = nonpawn_entry(pos, Color::Black);
    const long long total =
        pawn + (static_cast<long long>(white) + black) * params::corr_nonpawn_weight / 256;
    const long long error = target - total;
    auto update = [&](int& entry) {
        const long long updated = (static_cast<long long>(entry) * 256 + error * weight) / 256;
        entry = static_cast<int>(std::clamp<long long>(updated, -limit, limit));
    };
    update(pawn);
    update(white);
    update(black);
}

Searcher::ContTable* Searcher::cont_table(int ply, int back) {
    if (ply < back) return nullptr;
    const StackEntry& e = stack_[static_cast<std::size_t>(ply - back)];
    if (e.piece == NO_PIECE || e.move == Move{}) return nullptr;
    return &cont_hist_[piece_to(e.piece, e.move.to())];
}

// Butterfly history, the continuation histories and pawn history: the
// quiet move's standing in general, in reply to the last two moves, and in
// this pawn structure.
int Searcher::quiet_history(const Position& pos, Move m, const ContTable* const cont[2]) const {
    int s = history_[static_cast<std::size_t>(pos.side_to_move())]
                    [static_cast<std::size_t>(m.from())][static_cast<std::size_t>(m.to())];
    const std::size_t reply = piece_to(pos.piece_at(m.from()), m.to());
    for (int k = 0; k < 2; ++k)
        if (cont[k]) s += (*cont[k])[reply];
    if (params::pawn_hist_weight)
        s += pawn_hist_[pawn_hist_index(pos, m)] * params::pawn_hist_weight / 256;
    return s;
}

void Searcher::score_moves(const Position& pos, const MoveList& moves, int count, Move tt_move,
                           int ply, const ContTable* const cont[2],
                           std::array<int, 256>& scores) const {
    const Move* killers = stack_[static_cast<std::size_t>(ply)].killers;
    for (int i = 0; i < count; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        const Move m = moves[idx];
        if (m == tt_move) { scores[idx] = TT_MOVE_SCORE; continue; }

        const PieceType promo = m.promotion();
        const Piece victim = m.is_en_passant() ? WP : pos.piece_at(m.to());
        int s;
        if (victim != NO_PIECE) {
            // Least valuable attacker only breaks ties, until capture
            // history has something to say.
            const int attacker = pos.piece_at(m.from()) % 6;
            s = (see_at_least(pos, m, 0) ? CAPTURE_BASE : BAD_CAPTURE_BASE) +
                8 * ORDER_VALUE[victim % 6] +
                capt_hist_[capture_index(pos, m)] * params::capt_hist_mult / 1024 - attacker;
        } else if (promo != PieceType::None) {
            s = 0;
        } else if (m == killers[0]) {
            s = KILLER_1_SCORE;
        } else if (m == killers[1]) {
            s = KILLER_2_SCORE;
        } else {
            s = quiet_history(pos, m, cont);
        }
        if (promo == PieceType::Queen) s += QUEEN_PROMO_BONUS;
        else if (promo != PieceType::None) s = UNDERPROMO_SCORE;
        scores[idx] = s;
    }
}

// A quiet move caused a beta cutoff: make it a killer at this ply, reward
// it in the history and continuation history tables, and penalize the
// quiet moves tried before it. The update pulls each score toward
// +-hist_max rather than adding blindly ("history gravity"), so the tables
// never saturate.
void Searcher::update_quiet_stats(const Position& pos, Move best, const Move* tried, int n_tried,
                                  int depth, int ply) {
    Move* killers = stack_[static_cast<std::size_t>(ply)].killers;
    if (killers[0] != best) {
        killers[1] = killers[0];
        killers[0] = best;
    }

    auto& hist = history_[static_cast<std::size_t>(pos.side_to_move())];
    ContTable* cont[3] = {cont_table(ply, 1), cont_table(ply, 2),
                          params::cont_hist4_weight ? cont_table(ply, 4) : nullptr};
    const int bonus = std::min(params::hist_bonus_mult * depth * depth, params::hist_bonus_max);
    const int malus = std::min(params::hist_malus_mult * depth * depth, params::hist_malus_max);
    auto gravity = [](int& h, int delta) { h += delta - h * std::abs(delta) / params::hist_max; };
    auto apply = [&](Move m, int delta) {
        gravity(hist[static_cast<std::size_t>(m.from())][static_cast<std::size_t>(m.to())], delta);
        const std::size_t reply = piece_to(pos.piece_at(m.from()), m.to());
        for (ContTable* t : cont)
            if (t) gravity((*t)[reply], delta);
        if (params::pawn_hist_weight) gravity(pawn_hist_[pawn_hist_index(pos, m)], delta);
    };
    apply(best, bonus);
    for (int i = 0; i < n_tried; ++i) apply(tried[i], -malus);
}

void Searcher::update_capture_stats(const Position& pos, Move best, const Move* tried,
                                    int n_tried, int depth) {
    const int bonus = std::min(params::capt_bonus_mult * depth * depth, params::capt_bonus_max);
    const int malus = std::min(params::capt_malus_mult * depth * depth, params::capt_malus_max);
    auto apply = [&](Move m, int delta) {
        int& h = capt_hist_[capture_index(pos, m)];
        h += delta - h * std::abs(delta) / params::hist_max;
    };
    if (best != Move{}) apply(best, bonus);
    for (int i = 0; i < n_tried; ++i) apply(tried[i], -malus);
}


long long Searcher::elapsed_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start_).count();
}

// Counts the node and reports whether the search must stop. The node limit
// is checked exactly, so "go nodes N" is reproducible; the clock and a
// stop request from another thread only every 2048 nodes, since reading
// the clock is comparatively slow.
bool Searcher::should_stop() {
    if (stopped_) return true;  // unwinding: calls made after a stop aren't nodes
    ++nodes_;
    if (node_limit_ && nodes_ >= node_limit_) stopped_ = true;
    else if ((nodes_ & 2047) == 0 &&
             ((hard_ms_ && elapsed_ms() >= hard_ms_) || stop_requested()))
        stopped_ = true;
    return stopped_;
}

void Searcher::set_time_limits(const Position& pos, const SearchLimits& limits) {
    soft_ms_ = hard_ms_ = 0;
    if (limits.infinite) return;

    const long long overhead = params::move_overhead;  // GUI and OS latency, ms
    if (limits.movetime > 0) {
        hard_ms_ = soft_ms_ = std::max(1LL, limits.movetime - overhead);
        return;
    }

    const int side = static_cast<int>(pos.side_to_move());
    if (limits.time[side] < 0) return;  // no clock given: depth-limited or unbounded

    const long long left = limits.time[side];
    const long long inc = limits.inc[side];
    const long long mtg = limits.movestogo > 0 ? limits.movestogo : params::tm_moves_to_go;

    // Target time for this move, and a hard ceiling for when an iteration
    // runs long. A new iteration is only started while under a share of
    // the target (half by default), since each one takes roughly two to
    // three times the last. The shares are percentages (see params.h).
    const long long target = left / mtg + inc * params::tm_inc_pct / 100;
    const long long ceiling = std::max(1LL, left - overhead);
    // At least 1 ms: with a millisecond or less left and no increment the
    // share of the clock rounds to 0, and 0 would mean no limit at all.
    const long long cap =
        std::max(1LL, std::min(ceiling, left * params::tm_hard_cap_pct / 100 + inc));
    hard_ms_ = std::clamp(target * params::tm_hard_pct / 100, 1LL, cap);
    soft_ms_ = std::min(target * params::tm_soft_pct / 100, hard_ms_);
    if (soft_ms_ <= 0) soft_ms_ = 1;
}

bool Searcher::is_draw(const Position& pos) const {
    // Fifty moves without a capture or pawn move: a draw, unless the move
    // that reached the limit gave mate: checkmate takes precedence.
    if (pos.halfmove_clock() >= 100) {
        if (!pos.in_check(pos.side_to_move())) return true;
        MoveList moves;
        int count = 0;
        generate_legal(pos, moves, count);
        return count > 0;
    }

    // keys_.back() is the position one ply ago. The same side is to move
    // every second ply, and nothing before the last capture or pawn move
    // (the halfmove clock) can repeat. A single repetition inside the tree
    // is scored as a draw, the usual simplification: if repeating is good
    // for one side, the opponent would repeat again.
    const std::uint64_t key = pos.key();
    const int n = static_cast<int>(keys_.size());
    const int limit = std::min(pos.halfmove_clock(), n - static_cast<int>(null_barrier_));
    for (int d = 4; d <= limit; d += 2)
        if (keys_[static_cast<std::size_t>(n - d)] == key) return true;
    return false;
}

bool Searcher::repetition_in_reach(const Position& pos, int ply) const {
    // Positions an odd number of plies back have the other side to move, so
    // one move by either side can join them to this one. Only those on the
    // search path (i < ply) count: before the root, a single repetition
    // isn't a draw, and repetition flags aren't kept.
    const int n = static_cast<int>(keys_.size());
    const int end = std::min(pos.halfmove_clock(), n - static_cast<int>(null_barrier_));
    for (int i = 3; i <= end && i < ply; i += 2)
        if (pos.reversible_move_to(keys_[static_cast<std::size_t>(n - i)])) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Pruning support

std::uint64_t Searcher::next_random() {
    // xorshift64*
    rng_ ^= rng_ >> 12;
    rng_ ^= rng_ << 25;
    rng_ ^= rng_ >> 27;
    return rng_ * 0x2545F4914F6CDD1DULL;
}

bool Searcher::sample_verify() {
    return verify_rate_ && !verify_depth_ && next_random() % verify_rate_ == 0;
}

void Searcher::record_verified(prune::Heuristic h, bool wrong) {
    if (stopped_) return;  // the verification search was cut short
    ++stats_.c[h].verified;
    if (wrong) ++stats_.c[h].wrong;
}

// The node's static eval is better than the last one for the same side to
// move, two plies up (four, if that node was in check). Positions getting
// better are less likely to fail low, so the search prunes their moves
// less and cuts their nodes more readily. With nothing to compare against,
// assume improving.
bool Searcher::improving(int ply, int eval) const {
    for (int back = 2; back <= 4 && back <= ply; back += 2) {
        const int prev = stack_[static_cast<std::size_t>(ply - back)].static_eval;
        if (prev != NO_EVAL) return eval > prev;
    }
    return true;
}

// Searches a node again, with node-level pruning disabled, to check a
// decision that pruned it. Same ply, same window.
int Searcher::verify_node(Position& pos, int depth, int alpha, int beta, int ply,
                          bool cut_node) {
    ++verify_depth_;
    stack_[static_cast<std::size_t>(ply)].no_prune = true;
    const int v = negamax(pos, depth, alpha, beta, ply, cut_node);
    --verify_depth_;
    return v;
}

// Searches a pruned move anyway, at full depth with a null window, and
// records whether it would have raised alpha.
void Searcher::verify_pruned_move(Position& pos, Move m, prune::Heuristic h, int depth,
                                  int alpha, int ply) {
    ++verify_depth_;
    StateInfo st;
    keys_.push_back(pos.key());
    stack_[static_cast<std::size_t>(ply)].move = m;
    stack_[static_cast<std::size_t>(ply)].piece = pos.piece_at(m.from());
    stack_[static_cast<std::size_t>(ply)].quiet = is_quiet(pos, m);
    stack_[static_cast<std::size_t>(ply + 1)].extensions = stack_[static_cast<std::size_t>(ply)].extensions;
    stack_[static_cast<std::size_t>(ply + 1)].doubles = stack_[static_cast<std::size_t>(ply)].doubles;
    pos.make_move(m, st);
    eval_.push(st.dirty);
    const int score = -negamax(pos, depth - 1, -alpha - 1, -alpha, ply + 1, true);
    eval_.pop();
    pos.unmake_move(m, st);
    keys_.pop_back();
    --verify_depth_;
    record_verified(h, score > alpha);
}

// ---------------------------------------------------------------------------
// Alpha-beta

int Searcher::qsearch(Position& pos, int alpha, int beta, int ply) {
    if (should_stop()) return 0;
    seldepth_ = std::max(seldepth_, ply);
    if (ply >= MAX_PLY - 1) return eval_.evaluate(pos);

    const bool pv_node = beta - alpha > 1;
    const int alpha_orig = alpha;

    // Hash table. Any entry is deep enough to cut with, quiescence being
    // depth 0; as in the main search, not at PV nodes.
    Move tt_move{};
    int tt_eval = NO_EVAL;
    int tt_depth = -1;
    int tt_score = 0;
    TT::Bound tt_bound = TT::NONE;
    bool tt_pv = pv_node;
    if (const auto* e = tt_.probe(pos.key())) {
        tt_move = e->move;
        tt_eval = e->eval;
        tt_depth = e->depth;
        tt_score = score_from_tt(e->score, ply);
        tt_bound = e->bound();
        tt_pv = tt_pv || e->is_pv();
        if (!pv_node) {
            if (tt_bound == TT::EXACT ||
                (tt_bound == TT::LOWER && tt_score >= beta) ||
                (tt_bound == TT::UPPER && tt_score <= alpha))
                return tt_score;
        }
    }
    // Store a result, unless the table already holds a deeper search of
    // this position: a quiescence result would only replace it with less.
    auto store = [&](int score, Move best_move, int raw_eval) {
        if (tt_depth > 0) return;
        const auto bound = score >= beta ? TT::LOWER : score > alpha_orig ? TT::EXACT : TT::UPPER;
        tt_.store(pos.key(), best_move, score_to_tt(score, ply), raw_eval, 0, bound, tt_pv);
    };

    const bool in_check = pos.in_check(pos.side_to_move());
    MoveList moves;
    int count = 0;
    int best;
    int raw_eval = NO_EVAL;

    if (in_check) {
        // No standing pat in check: every evasion is searched, so mates at
        // the horizon are seen.
        generate_legal(pos, moves, count);
        if (count == 0) return -MATE + ply;
        best = -INF;
    } else {
        // Stand pat: the side to move can usually do at least as well as
        // the static evaluation by declining every capture.
        raw_eval = tt_eval != NO_EVAL ? tt_eval : eval_.evaluate(pos);
        best = sharpen_eval(corrected_eval(pos, raw_eval), tt_score, tt_bound);
        if (best >= beta) {
            store(best, Move{}, raw_eval);
            return best;
        }
        alpha = std::max(alpha, best);
        generate_captures(pos, moves, count);
    }

    std::array<int, 256> scores;
    // No continuation context: quiescence moves don't record theirs on the
    // stack, so the entries a ply or two up may belong to another line.
    const ContTable* const no_cont[2] = {nullptr, nullptr};
    score_moves(pos, moves, count, tt_move, ply, no_cont, scores);
    Move best_move{};

    prune::Context ctx;
    ctx.pos = &pos;
    ctx.ply = ply;
    ctx.static_eval = best;
    ctx.beta = beta;
    ctx.in_check = in_check;
    ctx.node = beta - alpha > 1 ? prune::NodeType::PV : prune::NodeType::All;
    // Built when delta pruning first needs it.
    Position::CheckInfo check_info;
    bool have_check_info = false;
    int quiet_evasions = 0;  // quiet evasions searched, in check

    for (int i = 0; i < count; ++i) {
        pick_next(moves, scores, i, count);
        const Move m = moves[static_cast<std::size_t>(i)];
        if (!in_check) {
            if (m.promotion() != PieceType::None && m.promotion() != PieceType::Queen)
                continue;  // underpromotions never matter for tactics outside check

            ctx.move = m;
            ctx.move_index = i;
            ctx.alpha = alpha;

            // Delta pruning: even winning the captured piece outright, plus a
            // margin for what else the capture might gain, leaves the score
            // below alpha. Not for promotions, nor for checks, which can win
            // more than the piece.
            if (m.promotion() == PieceType::None && std::abs(alpha) < MATE_BOUND) {
                const Piece victim = m.is_en_passant() ? WP : pos.piece_at(m.to());
                const int optimistic =
                    ctx.static_eval + ORDER_VALUE[victim % 6] + params::qs_delta_margin;
                count_tried(prune::QsDelta);
                if (optimistic <= alpha) {
                    if (!have_check_info) {
                        check_info = pos.check_info();
                        have_check_info = true;
                    }
                    if (!pos.gives_check(m, check_info)) {
                        count_fired(prune::QsDelta);
                        if (sample_verify())
                            verify_pruned_move(pos, m, prune::QsDelta, 1, alpha, ply);
                        best = std::max(best, optimistic);
                        continue;
                    }
                }
            }

            // A capture that loses material in the exchange can't raise a
            // score that already stands pat.
            count_tried(prune::QsSee);
            // Scoring already ran SEE against 0 for captures other than the
            // hash move: reuse it where it settles the test either way.
            const int threshold = prune::qs_see_threshold(ctx);
            const bool scored = m != tt_move && is_capture(pos, m);
            const bool good = scores[static_cast<std::size_t>(i)] >= CAPTURE_BASE / 2;
            const bool passes = scored && good && threshold <= 0    ? true
                                : scored && !good && threshold >= 0 ? false
                                                                     : see_at_least(pos, m, threshold);
            if (!passes) {
                count_fired(prune::QsSee);
                if (sample_verify()) verify_pruned_move(pos, m, prune::QsSee, 1, alpha, ply);
                continue;
            }
        } else if (is_quiet(pos, m) && best > -MATE_BOUND) {
            // In check, once a line that isn't mated is known and enough
            // quiet evasions have been searched, the rest are skipped: the
            // captures that evade come first, and the point here is only to
            // see mates at the horizon.
            count_tried(prune::QsEvasion);
            if (quiet_evasions >= params::qs_quiet_evasions) {
                count_fired(prune::QsEvasion);
                if (sample_verify()) verify_pruned_move(pos, m, prune::QsEvasion, 1, alpha, ply);
                continue;
            }
        }
        if (in_check && is_quiet(pos, m)) ++quiet_evasions;

        StateInfo st;
        pos.make_move(m, st);
        tt_.prefetch(pos.key());
        eval_.push(st.dirty);
        const int score = -qsearch(pos, -beta, -alpha, ply + 1);
        eval_.pop();
        pos.unmake_move(m, st);
        if (stopped_) return 0;

        if (score > best) {
            best = score;
            if (score > alpha) {
                alpha = score;
                best_move = m;
                if (alpha >= beta) break;
            }
        }
    }
    store(best, best_move, raw_eval);
    return best;
}

int Searcher::probcut(Position& pos, int depth, int pc_beta, int eval, Move tt_move, int ply,
                      bool cut_node, Move& cut_move) {
    // Captures (and queen promotions) whose static exchange alone makes up
    // the gap from the eval to pc_beta: the hash move first, then by the
    // value of the piece taken.
    MoveList moves;
    int count = 0;
    generate_captures(pos, moves, count);  // captures and promotions, legal
    std::array<int, 256> scores;
    int n = 0;
    for (int i = 0; i < count; ++i) {
        const Move m = moves[static_cast<std::size_t>(i)];
        if (!is_capture(pos, m) && m.promotion() != PieceType::Queen) continue;
        if (!see_at_least(pos, m, pc_beta - eval)) continue;
        const Piece victim = m.is_en_passant() ? WP : pos.piece_at(m.to());
        moves[static_cast<std::size_t>(n)] = m;
        scores[static_cast<std::size_t>(n)] =
            m == tt_move ? INT_MAX : victim == NO_PIECE ? 0 : ORDER_VALUE[victim % 6];
        ++n;
    }

    // Each is checked by quiescence first, which is cheap and usually
    // enough to refute it, then by a search params::probcut_reduction
    // plies shallower.
    StackEntry& ss = stack_[static_cast<std::size_t>(ply)];
    for (int i = 0; i < n; ++i) {
        pick_next(moves, scores, i, n);
        const Move m = moves[static_cast<std::size_t>(i)];
        StateInfo st;
        keys_.push_back(pos.key());
        ss.move = m;
        ss.piece = pos.piece_at(m.from());
        ss.quiet = false;  // captures and promotions only
        stack_[static_cast<std::size_t>(ply + 1)].extensions = ss.extensions;
        stack_[static_cast<std::size_t>(ply + 1)].doubles = ss.doubles;
        pos.make_move(m, st);
        tt_.prefetch(pos.key());
        eval_.push(st.dirty);
        int v = -qsearch(pos, -pc_beta, -pc_beta + 1, ply + 1);
        if (v >= pc_beta && !stopped_)
            v = -negamax(pos, depth - params::probcut_reduction, -pc_beta, -pc_beta + 1, ply + 1,
                         !cut_node);
        eval_.pop();
        pos.unmake_move(m, st);
        keys_.pop_back();
        if (stopped_) return 0;
        if (v >= pc_beta) {
            cut_move = m;
            return v;
        }
    }
    return -INF;
}

// `cut_node` is the expectation that a non-PV node fails high (a "cut"
// node) rather than low (an "all" node): the children of an all node are
// expected to be cut nodes, and the first child of a cut node an all node.
int Searcher::negamax(Position& pos, int depth, int alpha, int beta, int ply, bool cut_node) {
    if (should_stop()) return 0;
    seldepth_ = std::max(seldepth_, ply);

    const bool root = ply == 0;
    const bool pv_node = beta - alpha > 1;
    StackEntry& ss = stack_[static_cast<std::size_t>(ply)];
    const bool no_prune = std::exchange(ss.no_prune, false);
    const Move excluded = ss.excluded;  // set by a singular-extension probe
    ss.threat = Move{};
    ss.threat_score = NO_EVAL;
    if (!root && is_draw(pos)) return 0;
    if (ply >= MAX_PLY - 1) return eval_.evaluate(pos);

    // Upcoming repetition: if one reversible move can bring back a position
    // from earlier in this line, a draw is at hand, so the node is worth at
    // least that to the side to move.
    if (!root && params::rep_in_reach && alpha < 0 && repetition_in_reach(pos, ply)) {
        alpha = 0;
        if (alpha >= beta) return alpha;
    }

    // Mate distance pruning: from here, the best conceivable result is
    // mating on the next move and the worst is being mated right now. If
    // the window lies outside that range, no search can get into it.
    if (!root) {
        alpha = std::max(alpha, -MATE + ply);
        beta = std::min(beta, MATE - ply - 1);
        if (alpha >= beta) return alpha;
    }

    // In check, quiescence searches every evasion, so a node in check can
    // safely drop into it. Checks are extended where they are given, and
    // only when they don't lose material (see CheckExtSee).
    const bool in_check = pos.in_check(pos.side_to_move());
    if (depth <= 0) return qsearch(pos, alpha, beta, ply);

    Move tt_move{};
    int tt_eval = NO_EVAL;
    int tt_score = 0;
    int tt_depth = -1;
    TT::Bound tt_bound = TT::NONE;
    bool tt_was_pv = false;
    if (const auto* e = tt_.probe(pos.key())) {
        tt_move = e->move;
        tt_eval = e->eval;
        tt_score = score_from_tt(e->score, ply);
        tt_depth = e->depth;
        tt_bound = e->bound();
        tt_was_pv = e->is_pv();
        // No cutoffs in PV nodes: they would cut the principal variation
        // short and hide how the score was reached. Nor in a probe that
        // leaves a move out: the entry's score counts that move.
        if (excluded != Move{}) {
            tt_score = 0;
            tt_bound = TT::NONE;
        } else if (!pv_node && e->depth >= depth) {
            if (tt_bound == TT::EXACT ||
                (tt_bound == TT::LOWER && tt_score >= beta) ||
                (tt_bound == TT::UPPER && tt_score <= alpha))
                return tt_score;
        }
    }

    // A singular-extension probe re-enters this ply: it keeps the flag of
    // the node it probes.
    if (excluded == Move{}) ss.tt_pv = pv_node || tt_was_pv;

    // The hash table keeps the raw eval, so the correction applied is always
    // the current one. static_eval (corrected) is the node's own estimate,
    // which `improving` and correction history compare against; `eval` may
    // be sharpened by the hash score, and is what pruning decides with.
    const int raw_eval = in_check ? NO_EVAL : tt_eval != NO_EVAL ? tt_eval : eval_.evaluate(pos);
    ss.static_eval = in_check ? NO_EVAL : corrected_eval(pos, raw_eval);
    const int static_eval = ss.static_eval;
    const int eval = in_check ? NO_EVAL : sharpen_eval(static_eval, tt_score, tt_bound);

    prune::Context ctx;
    ctx.pos = &pos;
    ctx.depth = depth;
    ctx.ply = ply;
    ctx.static_eval = eval;
    ctx.correction = in_check ? 0 : std::abs(correction(pos));
    ctx.alpha = alpha;
    ctx.beta = beta;
    ctx.improving = !in_check && improving(ply, static_eval);
    ctx.tt_pv = ss.tt_pv;
    ctx.tt_capture = tt_move != Move{} && is_capture(pos, tt_move);
    ctx.in_check = in_check;
    ctx.node = pv_node ? prune::NodeType::PV : cut_node ? prune::NodeType::Cut
                                                        : prune::NodeType::All;

    // Internal iterative reduction (see params.h). Decided here, applied
    // after node-level pruning, which works with the full depth.
    const bool probing = excluded != Move{};  // a singular-extension probe
    const bool iir = !root && !probing && depth >= params::iir_min_depth && tt_move == Move{} &&
                     (pv_node || cut_node);

    if (label_sink_ && !in_check && !root && !verify_depth_ && !probing &&
        static_cast<std::size_t>(depth) < label_rates_.size()) {
        const std::uint64_t rate = label_rates_[static_cast<std::size_t>(depth)];
        if (rate && next_random() % rate == 0)
            label_sink_->push_back({pos.fen(), depth, ply, static_eval, static_eval - raw_eval,
                                    eval, alpha, beta, ctx.improving, ctx.node,
                                    null_barrier_ != 0, iir});
    }

    // Node-level pruning, only at non-PV nodes: their window is null, so
    // all that's asked is whether the node fails high or low, and a good
    // enough guess will do. Never in check, where the static eval means
    // little.
    if (!pv_node && !in_check && !no_prune && !probing) {
        // Alpha-side hash cut: a search a few plies shallower already put an
        // upper bound on this node well below alpha. Fail low without
        // searching (the mirror of ProbCut's hash-table test; see params.h).
        if (depth >= params::alpha_tt_min_depth && std::abs(alpha) < MATE_BOUND &&
            (tt_bound == TT::UPPER || tt_bound == TT::EXACT) &&
            tt_depth >= depth - params::alpha_tt_depth_margin && tt_depth < depth) {
            count_tried(prune::AlphaTtCut);
            if (tt_score <= alpha - params::alpha_tt_margin) {
                count_fired(prune::AlphaTtCut);
                if (sample_verify())
                    record_verified(prune::AlphaTtCut,
                                    verify_node(pos, depth, alpha, beta, ply, cut_node) > alpha);
                return tt_score;
            }
        }

        // Reverse futility pruning: the eval is so far above beta that a
        // shallow search is very unlikely to bring it back below.
        if (depth <= params::rfp_max_depth && std::abs(beta) < MATE_BOUND) {
            count_tried(prune::RFP);
            if (eval - prune::rfp_margin(ctx) >= beta) {
                count_fired(prune::RFP);
                if (sample_verify())
                    record_verified(prune::RFP,
                                    verify_node(pos, depth, alpha, beta, ply, cut_node) < beta);
                return eval;
            }
        }

        // Razoring: the eval is so far below alpha that only a tactic could
        // save the node. Quiescence looks for one; if it fails low too,
        // believe it. Not against a mate score: quiescence failing to reach
        // a mate is no proof that the node can't.
        if (depth <= params::razor_max_depth && std::abs(alpha) < MATE_BOUND) {
            count_tried(prune::Razor);
            if (eval + prune::razor_margin(ctx) < alpha) {
                const int v = qsearch(pos, alpha, alpha + 1, ply);
                if (stopped_) return 0;
                if (v <= alpha) {
                    count_fired(prune::Razor);
                    if (sample_verify())
                        record_verified(prune::Razor,
                                        verify_node(pos, depth, alpha, beta, ply, cut_node) > alpha);
                    return v;
                }
            }
        }

        // Null-move pruning: let the opponent move twice. If a reduced
        // search still fails high, a real move would almost surely do
        // better still. Not after another null move, and not without
        // pieces, where zugzwang makes passing a real advantage.
        if (depth >= params::nmp_min_depth && eval >= beta && std::abs(beta) < MATE_BOUND &&
            ply > 0 && stack_[static_cast<std::size_t>(ply - 1)].move != Move{} &&
            pos.has_non_pawn_material(pos.side_to_move())) {
            count_tried(prune::NullMove);
            const int r = std::max(prune::nmp_reduction(ctx), 0);

            StateInfo st;
            keys_.push_back(pos.key());
            pos.make_null_move(st);
            tt_.prefetch(pos.key());
            eval_.push(st.dirty);
            ss.move = Move{};
            ss.piece = NO_PIECE;
            stack_[static_cast<std::size_t>(ply + 1)].extensions = ss.extensions;
            stack_[static_cast<std::size_t>(ply + 1)].doubles = ss.doubles;
            const std::size_t barrier = std::exchange(null_barrier_, keys_.size());
            const std::uint64_t null_key = pos.key();
            int v = -negamax(pos, depth - 1 - r, -beta, -beta + 1, ply + 1, !cut_node);
            null_barrier_ = barrier;
            eval_.pop();
            pos.unmake_null_move(st);
            keys_.pop_back();
            if (stopped_) return 0;

            if (v >= beta) {
                count_fired(prune::NullMove);
                if (v > MATE_BOUND) v = beta;  // a mate found by passing isn't proven
                if (sample_verify())
                    record_verified(prune::NullMove,
                                    verify_node(pos, depth, alpha, beta, ply, cut_node) < beta);
                return v;
            }

            // The pass failed: whatever refuted it is a threat.
            const auto* e = tt_.probe(null_key);
            ss.threat = e ? e->move : Move{};
            ss.threat_score = v;
        }

        // ProbCut: if a good capture beats beta by a wide margin in a search
        // a few plies shallower, the full-depth search would almost surely
        // fail high too. Not when the hash table already says the node falls
        // short of the raised bound at nearly this depth.
        if (depth >= params::probcut_min_depth && std::abs(beta) < MATE_BOUND) {
            const int pc_beta =
                beta + params::probcut_margin - (ctx.improving ? params::probcut_improving : 0);
            if (!(tt_bound != TT::NONE && tt_depth >= depth - params::probcut_tt_margin &&
                  tt_score < pc_beta)) {
                count_tried(prune::ProbCut);
                Move cut_move{};
                const int v = probcut(pos, depth, pc_beta, eval, tt_move, ply, cut_node, cut_move);
                if (stopped_) return 0;
                if (v >= pc_beta) {
                    count_fired(prune::ProbCut);
                    tt_.store(pos.key(), cut_move, score_to_tt(v, ply), raw_eval,
                              depth - params::probcut_tt_margin,
                              TT::LOWER, ss.tt_pv);
                    if (sample_verify())
                        record_verified(prune::ProbCut,
                                        verify_node(pos, depth, alpha, beta, ply, cut_node) < beta);
                    return v;
                }
            }
        }
    }

    if (iir) {
        depth -= params::iir_reduction;
        ctx.depth = depth;  // move-level pruning and LMR work with the reduced depth
    }

    const ContTable* const cont[2] = {cont_table(ply, 1), cont_table(ply, 2)};
    // Continuation history 4 plies back only orders quiet moves.
    const ContTable* const cont4 = params::cont_hist4_weight ? cont_table(ply, 4) : nullptr;
    MovePicker picker(*this, pos, tt_move, ply, cont, cont4);
    int legal_moves = 0;

    const int alpha_orig = alpha;
    int best = -INF;
    Move best_move{};
    std::array<Move, 256> quiets_tried;
    int n_quiets = 0;
    std::array<Move, 256> captures_tried;
    int n_captures = 0;
    int searched = 0;
    bool skip_quiets = false;
    // Our pieces attacked by cheaper ones, computed at the first quiet move.
    Bitboard static_threats = 0;
    bool have_static_threats = false;
    // Built on the first move: every move is tested for check.
    Position::CheckInfo check_info;
    bool have_check_info = false;

    for (Move m; (m = picker.next()) != Move{};) {
        const int i = legal_moves++;  // position in the move order
        if (m == excluded) continue;
        const bool quiet = is_quiet(pos, m);
        // Only quiet moves are ever pruned or reduced on account of checks.
        if (!have_check_info) {
            check_info = pos.check_info();
            have_check_info = true;
        }
        const bool any_check = pos.gives_check(m, check_info);
        // Pruning and reductions only consider checks by quiet moves.
        const bool gives_check = quiet && any_check;

        // Once late move pruning has triggered, the remaining quiet moves
        // go without further ado (checks too, if LmpChecks says so).
        if (skip_quiets && quiet && (params::lmp_checks || !gives_check)) {
            count_tried(prune::LMP);
            count_fired(prune::LMP);
            if (sample_verify()) verify_pruned_move(pos, m, prune::LMP, depth, alpha, ply);
            continue;
        }

        ctx.move = m;
        ctx.move_index = i;
        ctx.alpha = alpha;
        ctx.quiet = quiet;
        ctx.killer = quiet && (m == ss.killers[0] || m == ss.killers[1]);
        ctx.history = quiet ? quiet_history(pos, m, cont) : 0;
        ctx.capture_history = !quiet && is_capture(pos, m) ? capt_hist_[capture_index(pos, m)] : 0;
        ctx.lmr_depth = std::max(depth - 1 - prune::lmr_base_reduction(depth, i), 0);
        ctx.gives_check = gives_check;
        // Static threats: only computed for LmrStaticThreat or a guide.
        if (quiet && !have_static_threats && (params::lmr_static_threat || prune::guide)) {
            static_threats = pos.threatened_by_lesser(pos.side_to_move());
            have_static_threats = true;
        }
        ctx.escapes_threat = quiet && (static_threats & bit(m.from()));
        ctx.threatened = ss.threat != Move{};
        ctx.evades_threat = ctx.threatened &&
                            (m.from() == ss.threat.to() ||
                             (squares_between(ss.threat.from(), ss.threat.to()) & bit(m.to())));

        // Move-level pruning, anywhere but the root, once a move has been
        // searched and didn't lose to a mate: there is then a real score
        // to fall back on if everything else is skipped. Checks are spared
        // futility pruning, and late move pruning unless LmpChecks is set:
        // a checking sacrifice is exactly the quiet move that a bad static
        // eval says to skip.
        if (!root && !in_check && best > -MATE_BOUND) {
            if (quiet) {
                // Late move pruning: quiet moves this far down the ordering
                // hardly ever matter at low depth.
                if (depth <= params::lmp_max_depth && (params::lmp_checks || !gives_check)) {
                    count_tried(prune::LMP);
                    if (i >= prune::lmp_threshold(ctx)) {
                        skip_quiets = true;
                        count_fired(prune::LMP);
                        if (sample_verify())
                            verify_pruned_move(pos, m, prune::LMP, depth, alpha, ply);
                        continue;
                    }
                }
                // History pruning: at low depth, a quiet move whose history
                // says it keeps failing isn't worth a search.
                if (ctx.lmr_depth <= params::hist_prune_max_depth && !ctx.gives_check) {
                    count_tried(prune::HistoryPrune);
                    if (ctx.history < prune::history_prune_threshold(ctx)) {
                        count_fired(prune::HistoryPrune);
                        if (sample_verify())
                            verify_pruned_move(pos, m, prune::HistoryPrune, depth, alpha, ply);
                        continue;
                    }
                }
                // Futility pruning: a quiet move can't gain enough to reach
                // alpha from this static eval.
                if (ctx.lmr_depth <= params::fp_max_depth && !ctx.gives_check) {
                    count_tried(prune::Futility);
                    if (eval + prune::futility_margin(ctx) <= alpha) {
                        count_fired(prune::Futility);
                        if (sample_verify())
                            verify_pruned_move(pos, m, prune::Futility, depth, alpha, ply);
                        continue;
                    }
                }
                // SEE pruning: the moved piece just gets taken.
                if (ctx.lmr_depth <= params::see_max_depth) {
                    count_tried(prune::SeeQuiet);
                    if (!see_at_least(pos, m, prune::see_threshold(ctx))) {
                        count_fired(prune::SeeQuiet);
                        if (sample_verify())
                            verify_pruned_move(pos, m, prune::SeeQuiet, depth, alpha, ply);
                        continue;
                    }
                }
            } else if (depth <= params::see_max_depth) {
                // SEE pruning of captures that lose too much material.
                count_tried(prune::SeeCapture);
                const int threshold = prune::see_threshold(ctx);
                if (!(picker.good_capture() && threshold <= 0) && !see_at_least(pos, m, threshold)) {
                    count_fired(prune::SeeCapture);
                    if (sample_verify())
                        verify_pruned_move(pos, m, prune::SeeCapture, depth, alpha, ply);
                    continue;
                }
            }
        }

        // Singular extension (see params.h): is the hash move the only
        // good move here? Search the others, shallower, against a bound
        // just below its score. If none reaches it, extend the hash move
        // (by two if they all fall well short, see SeDoubleMargin);
        // if even that bound beats beta, some other move does too: cut.
        // If the others reach it but it is below beta, the hash move isn't
        // special: when it was expected to fail high anyway (its score is
        // at least beta, or this is a cut node), search it shallower.
        int extension = 0;
        if (!root && !probing && m == tt_move && depth >= params::se_min_depth &&
            ply < params::se_ply_mult * root_depth_ && ss.extensions < params::se_max_extensions &&
            tt_depth >= depth - params::se_tt_depth_margin &&
            (tt_bound == TT::LOWER || tt_bound == TT::EXACT) &&
            std::abs(tt_score) < MATE_BOUND) {
            const int s_beta = tt_score - params::se_margin * depth / 16;
            const Move threat = ss.threat;  // the probe re-enters this ply
            const int threat_score = ss.threat_score;
            ss.excluded = m;
            const int v = negamax(pos, (depth - 1) * params::se_probe_depth_pct / 100, s_beta - 1,
                                  s_beta, ply, cut_node);
            ss.excluded = Move{};
            ss.threat = threat;
            ss.threat_score = threat_score;
            if (stopped_) return 0;
            if (v < s_beta)
                extension = !pv_node && v < s_beta - params::se_double_margin &&
                                    ss.doubles < params::se_max_doubles
                                ? 2
                                : 1;
            else if (s_beta >= beta)
                return s_beta;
            else if (tt_score >= beta || cut_node)
                extension = -params::se_negative;
        }

        // A check that doesn't lose material is searched a ply deeper (not on
        // top of a singular extension, nor after a negative one; only singular
        // extensions count towards the per-path cap).
        const int check_ext = any_check && see_at_least(pos, m, params::check_ext_see) ? 1 : 0;

        // A capture that loses material by static exchange (and neither
        // checks nor promotes) may be reduced like a quiet move
        // (LmrCaptures). Decided before the move is made, which SEE needs.
        const bool losing_capture =
            params::lmr_captures && !quiet && !any_check && m.promotion() == PieceType::None &&
            depth >= params::lmr_min_depth && i >= params::lmr_min_moves && !see_at_least(pos, m, 0);

        StateInfo st;
        keys_.push_back(pos.key());
        ss.move = m;
        ss.piece = pos.piece_at(m.from());
        ss.quiet = quiet;
        pos.make_move(m, st);
        tt_.prefetch(pos.key());
        eval_.push(st.dirty);
        ++searched;

        // Principal variation search: the first move gets the full window.
        // Every later one is first tested with a null window around alpha,
        // which only answers "is it better than what we have?"; the rare
        // move that says yes is searched again with the full window.
        int new_depth =
            depth - 1 + (extension < 0 ? extension : std::max(extension, check_ext));
        stack_[static_cast<std::size_t>(ply + 1)].extensions = ss.extensions + (extension > 0);
        stack_[static_cast<std::size_t>(ply + 1)].doubles = ss.doubles + (extension == 2);
        int score;
        if (searched == 1) {
            score = -negamax(pos, new_depth, -beta, -alpha, ply + 1, !pv_node && !cut_node);
        } else {
            // Late move reductions: quiet moves late in the ordering are
            // searched shallower first, and at full depth only if the
            // reduced search says they beat alpha.
            int r = 0;
            if (depth >= params::lmr_min_depth && !in_check &&
                i >= params::lmr_min_moves + (pv_node ? 1 : 0) &&
                (quiet ? !ctx.gives_check : losing_capture)) {
                count_tried(prune::LMR);
                // At least one ply left to search (and no clamp with lo > hi
                // if LmrMinDepth is tuned down to 1).
                r = std::max(std::min(prune::lmr_reduction(ctx), new_depth - 1), 0);
            }
            if (r > 0) {
                count_fired(prune::LMR);
                score = -negamax(pos, new_depth - r, -alpha - 1, -alpha, ply + 1, true);
                if (score > alpha) {
                    if (!verify_depth_) ++stats_.lmr_researches;
                    // How far above alpha the reduced search landed says how
                    // much to trust it: well above, re-search a ply deeper
                    // than planned; barely above the best score so far, a ply
                    // shallower (see params.h). The PV re-search below keeps
                    // the adjusted depth.
                    const int reduced = new_depth - r;
                    new_depth += (score > alpha + params::lmr_deeper_base +
                                              params::lmr_deeper_mult * r) -
                                 (score < best + params::lmr_shallower);
                    if (new_depth > reduced)
                        score = -negamax(pos, new_depth, -alpha - 1, -alpha, ply + 1, !cut_node);
                } else if (sample_verify()) {
                    ++verify_depth_;
                    const int full =
                        -negamax(pos, new_depth, -alpha - 1, -alpha, ply + 1, !cut_node);
                    --verify_depth_;
                    record_verified(prune::LMR, full > alpha);
                }
            } else {
                score = -negamax(pos, new_depth, -alpha - 1, -alpha, ply + 1, !cut_node);
            }
            if (pv_node && score > alpha && score < beta)
                score = -negamax(pos, new_depth, -beta, -alpha, ply + 1, false);
        }

        eval_.pop();
        pos.unmake_move(m, st);
        keys_.pop_back();
        if (stopped_) return 0;  // result is incomplete; don't store it

        if (score > best) {
            best = score;
            if (score > alpha) {
                alpha = score;
                best_move = m;
                // Only a move that beat alpha is known to be good; at the
                // root this is what gets played if time runs out.
                if (root) {
                    root_best_ = m;
                    root_best_score_ = score;
                }
                if (alpha >= beta) {
                    if (quiet) update_quiet_stats(pos, m, quiets_tried.data(), n_quiets, depth, ply);
                    update_capture_stats(pos, is_capture(pos, m) ? m : Move{}, captures_tried.data(),
                                         n_captures, depth);
                    break;
                }
            }
        }
        if (quiet) quiets_tried[static_cast<std::size_t>(n_quiets++)] = m;
        else if (is_capture(pos, m)) captures_tried[static_cast<std::size_t>(n_captures++)] = m;
    }

    if (legal_moves == 0) return in_check ? -MATE + ply : 0;

    // A singular-extension probe only reports: nothing is learned from or
    // stored for a node searched without one of its moves. With no other
    // move at all, the left-out one is singular by definition.
    if (probing) return searched ? best : alpha;

    const auto bound = best >= beta ? TT::LOWER : best > alpha_orig ? TT::EXACT : TT::UPPER;

    // Prior countermove: every move here failed low, so the opponent's quiet
    // move that led here held against all of them. Reward it in its side's
    // butterfly and continuation histories (see params::prior_cm_pct).
    if (bound == TT::UPPER && params::prior_cm_pct && ply >= 1) {
        const StackEntry& prev = stack_[static_cast<std::size_t>(ply - 1)];
        if (prev.quiet && prev.move != Move{} && prev.piece != NO_PIECE) {
            const int bonus =
                std::min(params::hist_bonus_mult * depth * depth, params::hist_bonus_max) *
                params::prior_cm_pct / 100;
            auto gravity = [&](int& h) { h += bonus - h * bonus / params::hist_max; };
            const Color side = opposite(pos.side_to_move());
            gravity(history_[static_cast<std::size_t>(side)][static_cast<std::size_t>(
                prev.move.from())][static_cast<std::size_t>(prev.move.to())]);
            const std::size_t reply = piece_to(prev.piece, prev.move.to());
            for (int back = 1; back <= 2; ++back)
                if (ContTable* t = cont_table(ply - 1, back)) gravity((*t)[reply]);
        }
    }
    // A node that fails low below a flagged parent keeps the flag when it
    // was searched deep enough: it is the parent's refutation line, likely
    // to come back to the PV (see params::tt_pv_inherit_depth).
    if (bound == TT::UPPER && !ss.tt_pv && ply >= 1 && depth > params::tt_pv_inherit_depth &&
        stack_[static_cast<std::size_t>(ply - 1)].tt_pv)
        ss.tt_pv = true;

    // Teach the correction history what the static eval got wrong. Only
    // where the score says something about it: not in check (no eval),
    // not when a capture or promotion was best (the eval never claims to
    // see tactics), not a mate, and not a bound on the wrong side of the
    // eval (a fail high below it, or a fail low above it, says nothing
    // about how far off it was).
    if (!in_check && (best_move == Move{} || is_quiet(pos, best_move)) &&
        std::abs(best) < MATE_BOUND && !(bound == TT::LOWER && best <= static_eval) &&
        !(bound == TT::UPPER && best >= static_eval)) {
        // The score carries the fifty-move scaling, which is applied after
        // the correction: undo it first (CorrUnscaleFifty), or the table
        // learns the scaling as eval error. Only while the scale is at
        // least 1/2, where undoing it amplifies noise at most twofold.
        int target = best;
        const int clock = pos.halfmove_clock();
        const int left = params::fifty_scale - clock;
        const bool scaled = params::corr_unscale_fifty && params::fifty_scale && clock > 0;
        if (!scaled || 2 * left >= params::fifty_scale) {
            if (scaled) target = best * params::fifty_scale / left;
            update_correction(pos, depth, target - raw_eval);
        }
    }

    tt_.store(pos.key(), best_move, score_to_tt(best, ply), raw_eval, depth, bound, ss.tt_pv);
    return best;
}

std::vector<Move> Searcher::principal_variation(Position& pos, int max_len) {
    // Follows hash moves from the root, up to the first position after it
    // that the search scores as a draw (fifty moves, or a repetition of the
    // game or of the line itself): the game would end there, and the score
    // says so. Each move is checked against the legal moves, since a hash
    // collision could supply a bogus one. keys_ holds the game history, as
    // is_draw expects.
    std::vector<Move> pv;
    std::vector<StateInfo> states(static_cast<std::size_t>(max_len));
    const std::size_t base = keys_.size();
    while (static_cast<int>(pv.size()) < max_len && (pv.empty() || !is_draw(pos))) {
        const auto* e = tt_.probe(pos.key());
        if (!e || e->move == Move{}) break;

        MoveList moves;
        int count = 0;
        generate_legal(pos, moves, count);
        if (std::find(moves.begin(), moves.begin() + count, e->move) == moves.begin() + count) break;

        keys_.push_back(pos.key());
        pos.make_move(e->move, states[pv.size()]);
        pv.push_back(e->move);
    }
    keys_.resize(base);
    for (auto i = pv.size(); i-- > 0;) pos.unmake_move(pv[i], states[i]);
    return pv;
}

Move Searcher::search(Position& pos, const SearchLimits& limits,
                      const std::vector<std::uint64_t>& history, bool verbose) {
    start_ = Clock::now();
    nodes_ = 0;
    node_limit_ = limits.nodes;
    stopped_ = false;
    eval_.reset(pos);
    for (auto& e : stack_) e = StackEntry{};
    keys_ = history;
    keys_.reserve(history.size() + MAX_PLY);
    null_barrier_ = 0;
    verify_depth_ = 0;
    last_score_ = 0;
    prune::init();
    tt_.new_search();
    set_time_limits(pos, limits);

    MoveList root_moves;
    int root_count = 0;
    generate_legal(pos, root_moves, root_count);
    if (root_count == 0) return Move{};
    Move best = root_moves[0];

    // Only one legal move: no point thinking long on the clock, but a
    // short search still reports a score (match runners adjudicate on it,
    // and GUIs show it).
    const bool forced = root_count == 1 && hard_ms_;

    const int max_depth = forced                ? std::min(4, MAX_PLY - 1)
                          : limits.depth > 0 ? std::min(limits.depth, MAX_PLY - 1)
                                             : MAX_PLY - 1;

    int prev_score = 0;
    int stability = 0;   // iterations the best move has held
    Move prev_best{};
    for (int depth = 1; depth <= max_depth; ++depth) {
        root_best_ = Move{};
        seldepth_ = 0;

        // Aspiration window: expect the score to stay near the last one and
        // search a narrow window around it, which prunes more. If the score
        // lands outside, widen that side and search again. Mate scores
        // jump too far for a window to help.
        int delta = params::asp_delta;
        int alpha = -INF, beta = INF;
        if (depth >= params::asp_min_depth && std::abs(prev_score) < MATE_BOUND) {
            alpha = std::max(prev_score - delta, -INF);
            beta = std::min(prev_score + delta, INF);
        }
        int score;
        while (true) {
            root_depth_ = depth;
            score = negamax(pos, depth, alpha, beta, 0, false);
            if (stopped_) break;
            if (score <= alpha) {
                beta = (alpha + beta) / 2;
                alpha = std::max(score - delta, -INF);
            } else if (score >= beta) {
                beta = std::min(score + delta, INF);
            } else {
                break;
            }
            delta += delta * params::asp_widen_pct / 100;
        }

        // One "info" line: depth, score, counters and a principal variation.
        // Written in one piece: the UCI thread may be answering "isready".
        auto report = [&](int d, int s, const std::vector<Move>& pv) {
            const long long ms = elapsed_ms();
            std::ostringstream line;
            line << "info depth " << d << " seldepth " << std::max(seldepth_, d)
                 << " score " << score_string(s)
                 << " nodes " << nodes_ << " nps "
                 << (ms > 0 ? nodes_ * 1000 / static_cast<std::uint64_t>(ms) : nodes_)
                 << " hashfull " << tt_.hashfull() << " time " << ms << " pv";
            for (const Move m : pv) line << ' ' << pos.move_to_uci(m);
            line << '\n';
            std::cout << line.str() << std::flush;
        };

        if (stopped_) {
            // A partial iteration still searched the previous best move
            // first (it is the hash move). root_best_ is only set by a move
            // that beat alpha, so if one did, it is a real improvement and
            // safe to play; otherwise keep the last completed result.
            if (root_best_ != Move{} && root_best_ != best) {
                best = root_best_;
                // Report it, so that the move played heads the last PV the
                // GUI saw. The rest of the line comes from the hash table,
                // where the partial iteration left it.
                if (verbose) {
                    std::vector<Move> pv{best};
                    StateInfo st;
                    keys_.push_back(pos.key());
                    pos.make_move(best, st);
                    if (!is_draw(pos))
                        for (const Move m : principal_variation(pos, depth - 1)) pv.push_back(m);
                    pos.unmake_move(best, st);
                    keys_.pop_back();
                    report(depth, root_best_score_, pv);
                }
            }
            break;
        }
        best = root_best_;
        const int score_before = prev_score;
        prev_score = score;
        last_score_ = score;

        if (verbose) report(depth, score, principal_variation(pos, depth));

        // Time: stop before a new iteration once past the soft limit, scaled
        // by how settled the search looks (see params.h). A best move that
        // keeps changing, or a falling score, earns more time; one that has
        // held for several iterations, less.
        if (soft_ms_) {
            stability = best == prev_best ? stability + 1 : 0;
            prev_best = best;
            const int drop = depth > 1 ? std::max(score_before - score, 0) : 0;
            const long long pct =
                std::max(params::tm_stable_start - params::tm_stable_step * stability,
                         params::tm_stable_min) +
                std::min(params::tm_drop_scale * drop / 100, params::tm_drop_max);
            const long long soft = std::min(soft_ms_ * pct / 100, hard_ms_);
            if (elapsed_ms() >= soft) break;
        }
        // A forced mate found within this depth can't get any shorter.
        if (score > MATE_BOUND && MATE - score <= depth) break;
    }
    return best;
}

std::vector<Move> Searcher::picker_order(const Position& pos, Move tt_move, Move killer1,
                                         Move killer2) {
    stack_[0].killers[0] = killer1;
    stack_[0].killers[1] = killer2;
    const ContTable* const cont[2] = {nullptr, nullptr};
    MovePicker picker(*this, pos, tt_move, 0, cont, nullptr);
    std::vector<Move> order;
    for (Move m; (m = picker.next()) != Move{};) order.push_back(m);
    return order;
}
