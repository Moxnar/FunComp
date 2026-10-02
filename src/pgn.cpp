#include "pgn.h"
#include "movegen.h"
#include <cctype>

namespace pgn {

namespace {

constexpr const char* START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// The value of a tag line `[Name "value"]`, if it is tag `name`.
bool tag_value(const std::string& line, const std::string& name, std::string& value) {
    if (line.compare(1, name.size() + 1, name + " ") != 0) return false;
    const auto open = line.find('"');
    const auto close = line.rfind('"');
    if (open == std::string::npos || close <= open) return false;
    value = line.substr(open + 1, close - open - 1);
    return true;
}

bool is_result(const std::string& t) {
    return t == "1-0" || t == "0-1" || t == "1/2-1/2" || t == "*";
}

// Splits movetext into move tokens, dropping comments, variations, NAGs,
// move numbers and results. `depth` counts open variations across lines.
void add_moves(const std::string& text, int& depth, bool& in_comment,
               std::vector<std::string>& moves) {
    std::string token;
    auto flush = [&] {
        // Move numbers may be glued to the move ("8...Nc7").
        std::size_t i = 0;
        while (i < token.size() && (std::isdigit(static_cast<unsigned char>(token[i])) ||
                                    token[i] == '.'))
            ++i;
        std::string t = token.substr(i);
        if (!t.empty() && depth == 0 && t[0] != '$' && !is_result(token)) moves.push_back(t);
        token.clear();
    };
    for (const char c : text) {
        if (in_comment) {
            if (c == '}') in_comment = false;
            continue;
        }
        if (c == '{') { flush(); in_comment = true; }
        else if (c == ';') { flush(); return; }  // comment to the end of the line
        else if (c == '(') { flush(); ++depth; }
        else if (c == ')') { flush(); if (depth > 0) --depth; }
        else if (std::isspace(static_cast<unsigned char>(c))) flush();
        else token += c;
    }
    flush();
}

}  // namespace

std::vector<Game> read(std::istream& in) {
    std::vector<Game> games;
    Game game;
    bool has_moves = false, has_tags = false;
    int depth = 0;
    bool in_comment = false;
    auto finish = [&] {
        if (has_tags || has_moves) {
            if (game.fen.empty()) game.fen = START_FEN;
            games.push_back(game);
        }
        game = Game{};
        has_moves = has_tags = false;
        depth = 0;
        in_comment = false;
    };
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!in_comment && !line.empty() && line[0] == '[') {
            if (has_moves) finish();  // tags after movetext start a new game
            has_tags = true;
            std::string value;
            if (tag_value(line, "FEN", value)) game.fen = value;
            else if (tag_value(line, "Result", value)) game.result = value;
            continue;
        }
        const std::size_t before = game.moves.size();
        add_moves(line, depth, in_comment, game.moves);
        has_moves |= game.moves.size() > before;
    }
    finish();
    return games;
}

Move parse_san(const Position& pos, const std::string& san) {
    std::string s = san;
    while (!s.empty() && (s.back() == '+' || s.back() == '#' || s.back() == '!' ||
                          s.back() == '?'))
        s.pop_back();
    MoveList moves;
    int count = 0;
    generate_legal(pos, moves, count);
    const auto legal = [&](int i) { return moves[static_cast<std::size_t>(i)]; };

    // Castling: the king's two-square move.
    if (s == "O-O" || s == "0-0" || s == "O-O-O" || s == "0-0-0") {
        const int step = s.size() == 3 ? 2 : -2;
        for (int i = 0; i < count; ++i) {
            const Move m = legal(i);
            if (pos.piece_at(m.from()) % 6 == static_cast<int>(PieceType::King) &&
                m.to() == m.from() + step)
                return m;
        }
        return Move{};
    }

    PieceType promo = PieceType::None;
    if (const auto eq = s.find('='); eq != std::string::npos && eq + 1 < s.size()) {
        const char p = s[eq + 1];
        promo = p == 'Q' ? PieceType::Queen : p == 'R' ? PieceType::Rook
              : p == 'B' ? PieceType::Bishop : p == 'N' ? PieceType::Knight : PieceType::None;
        s.erase(eq);
    }
    PieceType type = PieceType::Pawn;
    if (!s.empty() && std::string("KQRBN").find(s[0]) != std::string::npos) {
        type = s[0] == 'K' ? PieceType::King : s[0] == 'Q' ? PieceType::Queen
             : s[0] == 'R' ? PieceType::Rook : s[0] == 'B' ? PieceType::Bishop
                           : PieceType::Knight;
        s.erase(0, 1);
    }
    std::string rest;
    for (const char c : s)
        if (c != 'x' && c != '-') rest += c;
    if (rest.size() < 2) return Move{};
    const char file = rest[rest.size() - 2], rank = rest[rest.size() - 1];
    if (file < 'a' || file > 'h' || rank < '1' || rank > '8') return Move{};
    const int to = sq(file - 'a', rank - '1');
    const std::string from_hint = rest.substr(0, rest.size() - 2);  // file and/or rank

    Move found{};
    int matches = 0;
    for (int i = 0; i < count; ++i) {
        const Move m = legal(i);
        if (m.to() != to || m.promotion() != promo) continue;
        if (pos.piece_at(m.from()) % 6 != static_cast<int>(type)) continue;
        bool ok = true;
        for (const char h : from_hint) {
            if (h >= 'a' && h <= 'h') ok &= file_of(m.from()) == h - 'a';
            else if (h >= '1' && h <= '8') ok &= rank_of(m.from()) == h - '1';
            else ok = false;
        }
        if (ok) {
            found = m;
            ++matches;
        }
    }
    return matches == 1 ? found : Move{};
}

}  // namespace pgn
