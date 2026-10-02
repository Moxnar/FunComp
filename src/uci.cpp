#include "uci.h"
#include "bench.h"
#include "bingo.h"
#include "cont_log.h"
#include "eval.h"
#include "initiative.h"
#include "labels.h"
#include "position.h"
#include "movegen.h"
#include "perft.h"
#include "search.h"
#include "params.h"
#include "tablebase.h"
#include "telemetry.h"
#include "threads.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
void print_board(const Position& p) {
    for(int r=7;r>=0;--r) {
        std::cout << r+1 << " ";
        for(int f=0;f<8;++f) {
            Piece pc=p.piece_at(sq(f,r));
            static constexpr char chars[]="PNBRQKpnbrqk";
            std::cout << (pc==NO_PIECE ? '.' : chars[pc]) << ' ';
        }
        std::cout << '\n';
    }
    std::cout << "  a b c d e f g h\n";
    std::cout << "FEN: " << p.fen() << '\n';
}

bool apply_uci_move(Position& pos, const std::string& uci, std::vector<std::uint64_t>& history) {
    MoveList moves;
    int count=0;
    generate_legal(pos,moves,count);
    for(int i=0;i<count;++i) {
        if(pos.move_to_uci(moves[i])==uci) {
            StateInfo st;
            history.push_back(pos.key());
            pos.make_move(moves[i],st);
            return true;
        }
    }
    return false;
}

// Match telemetry (TelemetryDir): this process's counters over all its
// searches, in a file of its own (many engine processes write at once),
// summed across files by tools/telemetry-sum.sh.
void write_telemetry(const std::string& dir, const prune::Stats& stats) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name = "telemetry-" + std::to_string(stamp) + "-" +
                             std::to_string(std::random_device{}()) + ".txt";
    std::ofstream out(std::filesystem::path(dir) / name);
    stats.write(out);
}
}

void uci_loop() {
    Position pos;
    ThreadPool pool;  // the searchers, one per thread
    std::string telemetry_dir;  // TelemetryDir: empty, no match telemetry
    std::vector<std::uint64_t> history;  // keys of the game's earlier positions
    std::string line;

    // The search runs on its own thread, so that "stop" and "isready" are
    // answered while it thinks. Any other command first stops a running
    // search and waits for its bestmove (a GUI only sends them between
    // searches): so the search thread has the position, the history and
    // the searchers to themselves, and "go infinite" can't be left waiting for
    // a "stop" that sits behind the command being handled.
    Worker search_thread;
    auto finish_search=[&] {
        if(search_thread.busy()) {
            pool.request_stop();
            search_thread.wait();
            pool.clear_stop();  // or it would cut short a later "bench"
        }
    };

    while(std::getline(std::cin,line)) {
        // Some Windows tools (PowerShell's pipe, .NET process streams) start
        // their input with a UTF-8 byte-order mark; it would hide the command.
        if(line.starts_with("\xEF\xBB\xBF")) line.erase(0,3);
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;

        if(cmd=="isready") {
            std::cout << "readyok\n" << std::flush;  // one write: a search may be printing
            continue;
        }
        // The opponent played the move we were pondering on: the search goes
        // on, now on the clock.
        if(cmd=="ponderhit") { pool.ponderhit(); continue; }
        finish_search();  // "stop" is exactly this
        if(cmd=="quit") break;

        if(cmd=="uci") {
            std::cout << "id name FunComp 2.0\n";
            std::cout << "id author Moxnar\n";
            std::cout << "option name Hash type spin default 64 min 1 max 4096\n";
            std::cout << "option name Threads type spin default 1 min 1 max " << MAX_THREADS << '\n';
            // Only tells the GUI it may send "go ponder"; nothing to set.
            std::cout << "option name Ponder type check default false\n";
            std::cout << "option name Move Overhead type spin default " << params::move_overhead
                      << " min 0 max 5000\n";
            std::cout << "option name SyzygyPath type string default <empty>\n";
            // Counters over the whole session, written at exit (for matches).
            std::cout << "option name TelemetryDir type string default <empty>\n";
            std::cout << "option name SyzygyProbeLimit type spin default " << tb::probe_limit
                      << " min 0 max 7\n";
            std::cout << "option name SyzygyProbeDepth type spin default " << tb::probe_depth
                      << " min 1 max 100\n";
#ifdef TUNE
            // Diagnostics and tuning only; still settable in any build.
            std::cout << "option name PruneVerify type spin default 0 min 0 max 1000000\n";
            for(const auto& t : params::tunables())
                std::cout << "option name " << t.name << " type spin default " << *t.value
                          << " min " << t.min << " max " << t.max << '\n';
#endif
            std::cout << "uciok" << std::endl;
        } else if(cmd=="setoption") {
            // setoption name <id> [value <x>]; the id may contain spaces
            // ("Move Overhead"), so it runs up to "value".
            std::string tok, name, value;
            std::string* field=nullptr;
            while(ss>>tok) {
                if(tok=="name") field=&name;
                else if(tok=="value") field=&value;
                else if(field) {
                    if(!field->empty()) *field += ' ';
                    *field += tok;
                }
            }
            if(name=="Move Overhead" && !value.empty()) {
                params::move_overhead=std::clamp(std::stoi(value),0,5000);
            } else if(name=="Threads" && !value.empty()) {
                pool.set_threads(std::clamp(std::stoi(value),1,MAX_THREADS));
            } else if(name=="Hash" && !value.empty()) {
                const long mb=std::stol(value);
                pool.resize_hash(static_cast<std::size_t>(std::clamp(mb,1L,4096L)));
            } else if(name=="TelemetryDir") {
                telemetry_dir = value=="<empty>" ? std::string() : value;
            } else if(name=="SyzygyPath") {
                const int pieces=tb::init(value);
                if(pieces)
                    std::cout << "info string Syzygy tablebases found, up to " << pieces
                              << " pieces\n" << std::flush;
                else if(!value.empty() && value!="<empty>")
                    std::cout << "info string no Syzygy tablebases found in " << value
                              << '\n' << std::flush;
            } else if(name=="SyzygyProbeLimit" && !value.empty()) {
                tb::probe_limit=std::clamp(std::stoi(value),0,7);
            } else if(name=="SyzygyProbeDepth" && !value.empty()) {
                tb::probe_depth=std::clamp(std::stoi(value),1,100);
            } else if(name=="PruneVerify" && !value.empty()) {
                // Check 1 in N pruning decisions by searching anyway; see
                // "pstats". Diagnostic only: slows and alters the search.
                pool.main().set_verify_rate(std::stoi(value));
            } else if(!value.empty()) {
                // Search parameters are always settable, even when a normal
                // build doesn't advertise them.
                for(const auto& t : params::tunables())
                    if(name==t.name) *t.value=std::clamp(std::stoi(value),t.min,t.max);
            }
        } else if(cmd=="ucinewgame") {
            pos.set_startpos();
            history.clear();
            pool.new_game();
        } else if(cmd=="position") {
            std::string arg;
            ss >> arg;
            history.clear();
            if(arg=="startpos") {
                pos.set_startpos();
            }
            // A FEN runs up to "moves" (its clock fields are optional: GUIs
            // and EPD books often leave them out). An invalid one leaves the
            // position unchanged, and its moves are ignored rather than
            // played on the wrong position.
            std::string token;
            bool valid=true;
            if(arg=="fen") {
                std::string fen;
                while(ss>>token && token!="moves") {
                    if(!fen.empty()) fen += ' ';
                    fen += token;
                }
                valid = pos.set_fen(fen);
                if(!valid) std::cerr << "info string invalid FEN\n";
            } else {
                ss>>token;
            }

            if(valid && token=="moves") {
                while(ss>>token) {
                    if(!apply_uci_move(pos,token,history))
                        std::cerr << "info string illegal move " << token << "\n";
                }
            }
        } else if(cmd=="go") {
            SearchLimits limits;
            std::string arg;
            while(ss>>arg) {
                if(arg=="depth") ss>>limits.depth;
                else if(arg=="movetime") ss>>limits.movetime;
                // A clock that has run below zero (some GUIs send that) is
                // no time left, not "no clock": -1 means no clock.
                else if(arg=="wtime") { ss>>limits.time[0]; limits.time[0]=std::max(limits.time[0],0); }
                else if(arg=="btime") { ss>>limits.time[1]; limits.time[1]=std::max(limits.time[1],0); }
                else if(arg=="winc") { ss>>limits.inc[0]; limits.inc[0]=std::max(limits.inc[0],0); }
                else if(arg=="binc") { ss>>limits.inc[1]; limits.inc[1]=std::max(limits.inc[1],0); }
                else if(arg=="movestogo") ss>>limits.movestogo;
                else if(arg=="nodes") ss>>limits.nodes;
                else if(arg=="infinite") limits.infinite=true;
                else if(arg=="ponder") limits.ponder=true;
            }
            // Bare "go" with no limits searches until "stop", like "go infinite".
            const bool has_clock = limits.time[static_cast<int>(pos.side_to_move())] >= 0;
            if(!limits.depth && !limits.movetime && !limits.nodes && !has_clock) limits.infinite=true;
            pool.clear_stop();
            pool.clear_ponderhit();
            search_thread.start([&pool,&pos,&history,limits] {
                const Move best=pool.search(pos,limits,history);
                // With "go infinite" the GUI decides when the search is over: no
                // bestmove before "stop", even if the search ran out first (a mate
                // found, or the maximum depth). Nor while pondering, before the
                // ponderhit (a ponderhit after a finished search answers at once).
                auto waiting=[&] {
                    if(pool.stop_requested()) return false;
                    return limits.infinite || (limits.ponder && !pool.ponderhit_received());
                };
                while(waiting()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                std::string out="bestmove "+(best==Move{} ? std::string("0000") : pos.move_to_uci(best));
                const Move reply=pool.main().ponder_move(pos,best);
                if(reply!=Move{}) {
                    // The reply is a move in the position after `best`.
                    StateInfo st;
                    pos.make_move(best,st);
                    out+=" ponder "+pos.move_to_uci(reply);
                    pos.unmake_move(best,st);
                }
                std::cout << out+"\n" << std::flush;
                // Only now: the move mustn't wait on helpers still stopping.
                // Every later command waits for this job, so for them too.
                pool.wait_helpers();
            });
        } else if(cmd=="perft") {
            // "perft <depth>" uses the hash table; "perft <depth> nohash" doesn't.
            static std::unique_ptr<PerftTable> tt;
            int depth=1;
            ss>>depth;
            std::string opt;
            const bool hashed = !(ss>>opt && opt=="nohash");
            if(hashed && !tt) tt=std::make_unique<PerftTable>(256);
            const auto start=std::chrono::steady_clock::now();
            std::uint64_t nodes;
            if(hashed) {
                nodes=perft(pos,depth,*tt);
            } else {
                nodes=perft(pos,depth);
            }
            const double secs=std::chrono::duration<double>(
                std::chrono::steady_clock::now()-start).count();
            std::cout << "nodes " << nodes << " time " << static_cast<long long>(secs*1000)
                      << "ms nps " << static_cast<long long>(secs>0 ? static_cast<double>(nodes)/secs : 0)
                      << '\n';
        } else if(cmd=="d") {
            print_board(pos);
        } else if(cmd=="eval") {
            // Static evaluation with the initiative term broken down (not
            // corrected, not fifty-move scaled).
            initiative::print_trace(pos, std::cout);
            std::cout << "info string static eval " << evaluate(pos) << " cp (side to move)"
                      << std::endl;
        } else if(cmd=="bench") {
            // "bench [depth]"
            int depth=BENCH_DEPTH;
            ss>>depth;
            bench(pool.main(),depth,std::cout);
        } else if(cmd=="pstats") {
            // "pstats" prints pruning statistics accumulated since the last
            // "pstats reset"; "pstats reset" clears them.
            std::string arg;
            if(ss>>arg && arg=="reset") pool.main().clear_stats();
            else pool.main().stats().print(std::cout);
        } else if(cmd=="telemetry") {
            // "telemetry in=<pgn> [positions=N] [nodes=N] [verify=N]
            //  [minply=N] [seed=N]": counters on positions from a PGN's
            // games; see telemetry.h.
            TelemetryOptions opt;
            std::string tok;
            while(ss>>tok) {
                const auto eq=tok.find('=');
                if(eq==std::string::npos) continue;
                const std::string key=tok.substr(0,eq), val=tok.substr(eq+1);
                if(key=="in") opt.in=val;
                else if(key=="positions") opt.positions=std::max(1,std::stoi(val));
                else if(key=="nodes") opt.nodes=std::stoull(val);
                else if(key=="verify") opt.verify=std::max(0,std::stoi(val));
                else if(key=="minply") opt.min_ply=std::max(0,std::stoi(val));
                else if(key=="seed") opt.seed=std::stoull(val);
            }
            if(opt.in.empty()) std::cout << "info string telemetry needs in=<pgn>" << std::endl;
            else game_telemetry(pool.main(),opt,std::cout);
        } else if(cmd=="genlabels") {
            // "genlabels out=<file> [in=<file>] [games=N] [randomplies=N]
            //  [depth=N] [sample=N] [depthscale=PCT] [maxdepth=N] [count=N]
            //  [seed=N] [hash=MB] [labelhash=MB] [labelnodes=N]"
            // Writes pruning labels; see labels.h.
            LabelOptions opt;
            std::string tok;
            while(ss>>tok) {
                const auto eq=tok.find('=');
                if(eq==std::string::npos) continue;
                const std::string key=tok.substr(0,eq), val=tok.substr(eq+1);
                if(key=="out") opt.out=val;
                else if(key=="in") opt.in=val;
                else if(key=="games") opt.games=std::stoi(val);
                else if(key=="randomplies") opt.random_plies=std::stoi(val);
                else if(key=="depth") opt.depth=std::stoi(val);
                else if(key=="sample") opt.sample_rate=std::stoi(val);
                else if(key=="depthscale") opt.depth_scale=std::stoi(val);
                else if(key=="maxdepth") opt.max_label_depth=std::clamp(std::stoi(val),1,Searcher::MAX_PLY);
                else if(key=="count") opt.count=std::stoull(val);
                else if(key=="seed") opt.seed=std::stoull(val);
                else if(key=="hash") opt.hash_mb=std::clamp(std::stoi(val),1,4096);
                else if(key=="labelhash") opt.label_hash_mb=std::clamp(std::stoi(val),1,4096);
                else if(key=="labelnodes") opt.label_node_cap=std::stoull(val);
            }
            if(opt.out.empty()) std::cout << "info string genlabels needs out=<file>" << std::endl;
            else generate_labels(opt,std::cout);
        } else if(cmd=="genbingo") {
            // "genbingo out=<file> [plies=N] [perstart=N] [nodes=N]
            //  [maxscore=CP] [seed=N] [hash=MB]": Backrank Bingo openings;
            // see bingo.h.
            BingoOptions opt;
            std::string tok;
            while(ss>>tok) {
                const auto eq=tok.find('=');
                if(eq==std::string::npos) continue;
                const std::string key=tok.substr(0,eq), val=tok.substr(eq+1);
                if(key=="out") opt.out=val;
                else if(key=="plies") opt.plies=std::max(0,std::stoi(val));
                else if(key=="perstart") opt.per_start=std::max(1,std::stoi(val));
                else if(key=="nodes") opt.nodes=std::stoull(val);
                else if(key=="maxscore") opt.max_score=std::stoi(val);
                else if(key=="seed") opt.seed=std::stoull(val);
                else if(key=="hash") opt.hash_mb=std::clamp(std::stoi(val),1,4096);
            }
            if(opt.out.empty()) std::cout << "info string genbingo needs out=<file>" << std::endl;
            else generate_bingo(opt,std::cout);
        } else if(cmd=="genconlog") {
            // "genconlog out=<prefix> [games=N] [randomplies=N] [nodes=N]
            //  [mindepth=N] [rate=N] [depthscale=PCT] [seed=N] [hash=MB]":
            // continuation logs from self-play; see cont_log.h.
            ContLogOptions opt;
            std::string tok;
            while(ss>>tok) {
                const auto eq=tok.find('=');
                if(eq==std::string::npos) continue;
                const std::string key=tok.substr(0,eq), val=tok.substr(eq+1);
                if(key=="out") opt.out=val;
                else if(key=="games") opt.games=std::max(1,std::stoi(val));
                else if(key=="randomplies") opt.random_plies=std::max(0,std::stoi(val));
                else if(key=="nodes") opt.nodes=std::stoull(val);
                else if(key=="mindepth") opt.min_depth=std::clamp(std::stoi(val),1,63);
                else if(key=="rate") opt.rate=std::max(1,std::stoi(val));
                else if(key=="depthscale") opt.depth_scale=std::max(100,std::stoi(val));
                else if(key=="seed") opt.seed=std::stoull(val);
                else if(key=="hash") opt.hash_mb=std::clamp(std::stoi(val),1,4096);
            }
            if(opt.out.empty()) std::cout << "info string genconlog needs out=<prefix>" << std::endl;
            else generate_cont_log(opt,std::cout);
        }
    }
    // End of input (the GUI went away): the search mustn't outlive the
    // searchers.
    finish_search();
    if(!telemetry_dir.empty()) write_telemetry(telemetry_dir,pool.total_stats());
}
