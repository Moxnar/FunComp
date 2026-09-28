#include "uci.h"
#include "bench.h"
#include "labels.h"
#include "position.h"
#include "movegen.h"
#include "perft.h"
#include "search.h"
#include "params.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
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

// A thread that lives as long as the UCI loop and runs one job at a time,
// sleeping in between. Starting a new thread for each "go" would be
// simpler, but on a busy machine a new thread can wait tens of
// milliseconds for its first time slice, all of it on the engine's clock:
// measured with 24 match games on 24 hardware threads, a median of 6 ms
// and up to 47 ms, where waking a waiting thread took 15 us.
class Worker {
public:
    Worker() : thread_([this] { run(); }) {}
    ~Worker() {
        {
            std::lock_guard lock(mutex_);
            quit_=true;
        }
        cv_.notify_all();
        thread_.join();  // after the job in hand, if any
    }

    // Starts `job` on the thread. The previous job must have finished.
    void start(std::function<void()> job) {
        {
            std::lock_guard lock(mutex_);
            job_=std::move(job);
            busy_=true;
        }
        cv_.notify_all();
    }
    bool busy() {
        std::lock_guard lock(mutex_);
        return busy_;
    }
    void wait() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock,[this] { return !busy_; });
    }

private:
    void run() {
        std::unique_lock lock(mutex_);
        while(true) {
            cv_.wait(lock,[this] { return job_ || quit_; });
            if(!job_) return;  // quitting, with nothing left to do
            auto job=std::move(job_);
            job_=nullptr;
            lock.unlock();
            job();
            lock.lock();
            busy_=false;
            cv_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::function<void()> job_;
    bool busy_=false, quit_=false;
    std::thread thread_;  // last: it starts running once the rest exists
};
}

void uci_loop() {
    Position pos;
    Searcher searcher;
    std::vector<std::uint64_t> history;  // keys of the game's earlier positions
    std::string line;

    // The search runs on its own thread, so that "stop" and "isready" are
    // answered while it thinks. Any other command first stops a running
    // search and waits for its bestmove (a GUI only sends them between
    // searches): so the search thread has the position, the history and
    // the searcher to itself, and "go infinite" can't be left waiting for
    // a "stop" that sits behind the command being handled.
    Worker search_thread;
    auto finish_search=[&] {
        if(search_thread.busy()) {
            searcher.request_stop();
            search_thread.wait();
            searcher.clear_stop();  // or it would cut short a later "bench"
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
        if(cmd=="ponderhit") continue;  // no pondering (no Ponder option)
        finish_search();  // "stop" is exactly this
        if(cmd=="quit") break;

        if(cmd=="uci") {
            std::cout << "id name FunComp 1.0\n";
            std::cout << "id author Moxnar\n";
            std::cout << "option name Hash type spin default 64 min 1 max 4096\n";
            std::cout << "option name Move Overhead type spin default " << params::move_overhead
                      << " min 0 max 5000\n";
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
            } else if(name=="Hash" && !value.empty()) {
                const long mb=std::stol(value);
                searcher.resize_hash(static_cast<std::size_t>(std::clamp(mb,1L,4096L)));
            } else if(name=="PruneVerify" && !value.empty()) {
                // Check 1 in N pruning decisions by searching anyway; see
                // "pstats". Diagnostic only: slows and alters the search.
                searcher.set_verify_rate(std::stoi(value));
            } else if(!value.empty()) {
                // Search parameters are always settable, even when a normal
                // build doesn't advertise them.
                for(const auto& t : params::tunables())
                    if(name==t.name) *t.value=std::clamp(std::stoi(value),t.min,t.max);
            }
        } else if(cmd=="ucinewgame") {
            pos.set_startpos();
            history.clear();
            searcher.new_game();
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
            }
            // Bare "go" with no limits searches until "stop", like "go infinite".
            const bool has_clock = limits.time[static_cast<int>(pos.side_to_move())] >= 0;
            if(!limits.depth && !limits.movetime && !limits.nodes && !has_clock) limits.infinite=true;
            searcher.clear_stop();
            search_thread.start([&searcher,&pos,&history,limits] {
                const Move best=searcher.search(pos,limits,history);
                // With "go infinite" the GUI decides when the search is over: no
                // bestmove before "stop", even if the search ran out first (a mate
                // found, or the maximum depth).
                if(limits.infinite)
                    while(!searcher.stop_requested())
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                std::cout << "bestmove " + (best==Move{} ? std::string("0000") : pos.move_to_uci(best)) + "\n"
                          << std::flush;
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
        } else if(cmd=="bench") {
            // "bench [depth]"
            int depth=BENCH_DEPTH;
            ss>>depth;
            bench(searcher,depth,std::cout);
        } else if(cmd=="pstats") {
            // "pstats" prints pruning statistics accumulated since the last
            // "pstats reset"; "pstats reset" clears them.
            std::string arg;
            if(ss>>arg && arg=="reset") searcher.clear_stats();
            else searcher.stats().print(std::cout);
        } else if(cmd=="genlabels") {
            // "genlabels out=<file> [in=<file>] [games=N] [randomplies=N]
            //  [depth=N] [sample=N] [depthscale=PCT] [maxdepth=N] [count=N]
            //  [seed=N] [hash=MB] [labelhash=MB]"
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
            }
            if(opt.out.empty()) std::cout << "info string genlabels needs out=<file>" << std::endl;
            else generate_labels(opt,std::cout);
        }
    }
    // End of input (the GUI went away): the search mustn't outlive the
    // searcher.
    finish_search();
}
