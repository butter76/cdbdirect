#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "rocksdb/db.h"
#include "rocksdb/iterator.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"

#include "fen2cdb.h"
#include "scan_utils.h"

#include "external/threadpool.hpp"

#include "chess.hpp"

using namespace TERARKDB_NAMESPACE;

// ============================================================================
// Tunable thresholds
// ============================================================================

constexpr int DECISIVE_CP          = 400;    // |eval| >= this to be "dead-won"
constexpr int MATE_THRESHOLD       = 15000;  // evals above this are mate scores
constexpr int GOOD_MOVE_CP         = 300;    // a move maintaining >= this is "good"
constexpr int MIN_PIECES           = 8;      // exclude 7-man TB territory
constexpr int MIN_EXPLORED_MOVES   = 2;      // need at least 2 CDB moves for sharpness
constexpr double GOOD_MOVE_BASE    = 0.1;    // probability factor per extra good move (1/10)
constexpr int OBVIOUS_THRESHOLD    = 200;    // SEE + material_balance above this (in cp) triggers downweight
constexpr double OBVIOUS_WEIGHT    = 1.0 / 50.0;  // downweight for obvious captures/material-up positions
constexpr std::int64_t BLOCK_CACHE_GB = 128;
constexpr int PROGRESS_INTERVAL    = 10'000'000;

// ============================================================================
// CP <-> Q conversion (tangent-based, from coroutine_search.hpp)
// ============================================================================

static constexpr double TAN_SCALE = 100.7066;
static constexpr double TAN_COEFF = 1.5637541897;

static double cp_to_q(int cp) {
  return std::atan(static_cast<double>(cp) / TAN_SCALE) / TAN_COEFF;
}

// ============================================================================
// Static Exchange Evaluation (SEE)
// ============================================================================

static constexpr int SEE_PIECE_VALUE[] = {
    100,   // PAWN
    300,   // KNIGHT
    300,   // BISHOP
    500,   // ROOK
    900,   // QUEEN
    20000, // KING
    0,     // NONE
};

static int see_piece_value(chess::PieceType pt) {
  return SEE_PIECE_VALUE[static_cast<int>(pt.internal())];
}

// Returns the least-valuable attacker from `attacker_bb`, removes it from both
// `attacker_bb` and `occ`, and returns its piece type.  NONE if empty.
static chess::PieceType pop_lva(const chess::Board &board,
                                chess::Bitboard &attacker_bb,
                                chess::Bitboard &occ,
                                chess::Color side) {
  using PT = chess::PieceType;
  static constexpr PT order[] = {PT::PAWN, PT::KNIGHT, PT::BISHOP,
                                 PT::ROOK, PT::QUEEN,  PT::KING};
  for (auto pt : order) {
    chess::Bitboard subset = attacker_bb & board.pieces(pt, side);
    if (subset) {
      chess::Bitboard bit = chess::Bitboard::fromSquare(chess::Square(subset.lsb()));
      attacker_bb ^= bit;
      occ ^= bit;
      return pt;
    }
  }
  return PT::NONE;
}

// Standard iterative SEE.  Returns the material gain/loss in centipawns from
// the perspective of the side making the capture.
static int see(const chess::Board &board, chess::Move move) {
  using namespace chess;

  Square to = move.to();
  Square from = move.from();
  Color stm = board.sideToMove();

  PieceType captured_pt = board.at(to).type();
  if (move.typeOf() == Move::ENPASSANT)
    captured_pt = PieceType::PAWN;

  int gain[32];
  int depth = 0;
  gain[0] = (captured_pt != PieceType::NONE) ? see_piece_value(captured_pt) : 0;

  PieceType attacker_pt = board.at(from).type();
  if (move.typeOf() == Move::PROMOTION)
    attacker_pt = PieceType(move.promotionType());

  Bitboard occ = board.occ();
  occ ^= Bitboard::fromSquare(from);
  if (move.typeOf() == Move::ENPASSANT) {
    Square ep_victim = Square(to.file(), from.rank());
    occ ^= Bitboard::fromSquare(ep_victim);
  }

  Bitboard all_attackers =
      (attacks::pawn(Color::WHITE, to) & board.pieces(PieceType::PAWN, Color::BLACK)) |
      (attacks::pawn(Color::BLACK, to) & board.pieces(PieceType::PAWN, Color::WHITE)) |
      (attacks::knight(to) & board.pieces(PieceType::KNIGHT)) |
      (attacks::bishop(to, occ) & (board.pieces(PieceType::BISHOP) | board.pieces(PieceType::QUEEN))) |
      (attacks::rook(to, occ) & (board.pieces(PieceType::ROOK) | board.pieces(PieceType::QUEEN))) |
      (attacks::king(to) & board.pieces(PieceType::KING));
  all_attackers &= occ;

  stm = ~stm;

  while (true) {
    ++depth;
    gain[depth] = see_piece_value(attacker_pt) - gain[depth - 1];

    if (std::max(-gain[depth - 1], gain[depth]) < 0)
      break;

    // pop_lva removes the attacker from both all_attackers and occ
    attacker_pt = pop_lva(board, all_attackers, occ, stm);
    if (attacker_pt == PieceType::NONE)
      break;

    // X-ray: removing that piece from occ may reveal sliders behind it
    if (attacker_pt == PieceType::PAWN || attacker_pt == PieceType::BISHOP ||
        attacker_pt == PieceType::QUEEN) {
      all_attackers |= attacks::bishop(to, occ) &
                       (board.pieces(PieceType::BISHOP) | board.pieces(PieceType::QUEEN));
    }
    if (attacker_pt == PieceType::ROOK || attacker_pt == PieceType::QUEEN) {
      all_attackers |= attacks::rook(to, occ) &
                       (board.pieces(PieceType::ROOK) | board.pieces(PieceType::QUEEN));
    }
    all_attackers &= occ;

    stm = ~stm;
  }

  while (--depth > 0) {
    gain[depth - 1] = -std::max(-gain[depth - 1], gain[depth]);
  }
  return gain[0];
}

// ============================================================================
// Material balance (in centipawns, positive = STM has more material)
// ============================================================================

static int material_balance(const chess::Board &board) {
  using namespace chess;
  auto count = [&](PieceType pt, Color c) -> int {
    return board.pieces(pt, c).count();
  };
  Color stm = board.sideToMove();
  Color opp = ~stm;
  int stm_mat = count(PieceType::PAWN, stm) * 100 +
                count(PieceType::KNIGHT, stm) * 300 +
                count(PieceType::BISHOP, stm) * 300 +
                count(PieceType::ROOK, stm) * 500 +
                count(PieceType::QUEEN, stm) * 900;
  int opp_mat = count(PieceType::PAWN, opp) * 100 +
                count(PieceType::KNIGHT, opp) * 300 +
                count(PieceType::BISHOP, opp) * 300 +
                count(PieceType::ROOK, opp) * 500 +
                count(PieceType::QUEEN, opp) * 900;
  return stm_mat - opp_mat;
}

// ============================================================================
// Worker: processes one key range of the DB
// ============================================================================

struct ScanStats {
  std::atomic<std::uint64_t> entries_scanned{0};
  std::atomic<std::uint64_t> positions_passed_hard{0};
  std::atomic<std::uint64_t> positions_output{0};
  std::atomic<std::uint64_t> dead_lost_output{0};
};

static void scan_range(DB *db, const std::string &range_start,
                       const std::string &range_end, int thread_id,
                       const std::string &output_dir, ScanStats &stats) {
  std::string out_path =
      output_dir + "/tactical_positions." +
      (thread_id < 100 ? (thread_id < 10 ? "00" : "0") : "") +
      std::to_string(thread_id) + ".tsv";
  std::ofstream out(out_path);
  if (!out.is_open()) {
    std::cerr << "Thread " << thread_id << ": failed to open " << out_path
              << std::endl;
    return;
  }

  std::mt19937 rng(thread_id * 999983ULL + 42);
  std::uniform_real_distribution<double> unif(0.0, 1.0);

  std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
  it->Seek(range_start);

  std::uint64_t local_entries = 0;

  for (; it->Valid(); it->Next()) {
    const Slice k = it->key();

    // Stop if we've left our range
    if (k.compare(Slice(range_end)) >= 0)
      break;

    ++local_entries;
    if (local_entries % 1'000'000 == 0) {
      stats.entries_scanned.fetch_add(1'000'000, std::memory_order_relaxed);
    }

    // --- Hard filter: key prefix ---
    if (k.size() == 0 || k.data()[0] != 'h')
      continue;

    // --- Decode move/score pairs ---
    const Slice v = it->value();
    std::vector<StrPair> scored_moves;
    get_hash_values(v.ToString(), scored_moves);
    if (scored_moves.empty())
      continue;

    // Separate moves from ply-distance marker, compute backpropagated evals
    struct MoveEval {
      std::string uci;
      int eval; // from parent's perspective (after backprop)
    };
    std::vector<MoveEval> moves;
    moves.reserve(scored_moves.size());

    for (const auto &p : scored_moves) {
      if (p.first == "a0a0")
        continue;
      int child = std::stoi(p.second);
      int eval = backprop_score(child);
      moves.push_back({p.first, eval});
    }

    if (static_cast<int>(moves.size()) < MIN_EXPLORED_MOVES)
      continue;

    // --- Reconstruct FEN ---
    std::string hex_key = bin2hex(k.ToString().substr(1));
    std::string fen = cbhexfen2fen(hex_key);

    // --- Hard filter: piece count ---
    if (count_pieces_in_fen_board(fen) < MIN_PIECES)
      continue;

    // --- Hard filter: standard castling ---
    if (!obeys_standard_castling_constraints(fen))
      continue;

    // --- Sort moves by eval descending ---
    std::sort(moves.begin(), moves.end(),
              [](const MoveEval &a, const MoveEval &b) {
                return a.eval > b.eval;
              });

    int best_eval = moves[0].eval;

    // --- Decisive eval filter (dead-won for STM only) ---
    // Dead-lost positions are generated as children of dead-won positions,
    // so we only need to find positions where STM is winning.
    bool is_winning_mate = (best_eval >= MATE_THRESHOLD);
    if (!is_winning_mate && best_eval < DECISIVE_CP)
      continue;

    stats.positions_passed_hard.fetch_add(1, std::memory_order_relaxed);

    // --- Count "good" moves (maintain winning advantage) ---
    int good_count = 0;
    std::vector<MoveEval> winning_moves;
    for (const auto &m : moves) {
      bool mate_good = (m.eval >= MATE_THRESHOLD);
      bool cp_good = (m.eval >= GOOD_MOVE_CP);
      if (mate_good || cp_good) {
        ++good_count;
        winning_moves.push_back(m);
      }
    }

    if (good_count == 0 || good_count > 2)
      continue;

    // --- Good-move downweighting: 1/10 for 2 good moves, skip >2 ---
    double prob = (good_count == 2) ? GOOD_MOVE_BASE : 1.0;

    // --- Obviousness downweighting via (SEE + material balance) ---
    // Positions where the best move is a safe capture AND the winning side
    // is already up in material are "obvious" -- downweight them.
    chess::Board board(fen);
    chess::Move best_move = chess::uci::uciToMove(board, moves[0].uci);

    int see_val = 0;
    if (board.isCapture(best_move))
      see_val = see(board, best_move);

    int mat_bal = material_balance(board);
    int obviousness = see_val + mat_bal;

    if (obviousness > OBVIOUS_THRESHOLD)
      prob *= OBVIOUS_WEIGHT;

    // --- Roll the dice ---
    if (unif(rng) > prob)
      continue;

    // --- Emit dead-won position (type W) ---
    out << fen << "\tW\t";
    for (size_t i = 0; i < winning_moves.size(); ++i) {
      if (i > 0)
        out << ',';
      out << winning_moves[i].uci << ':' << winning_moves[i].eval;
    }
    out << '\n';
    stats.positions_output.fetch_add(1, std::memory_order_relaxed);

    // --- Generate opponent-side positions ---
    // 70%: play the best winning move (dead-lost for opponent)
    // 30%: play a random legal move (may or may not be losing)
    constexpr double BEST_MOVE_PROB = 0.7;

    chess::Movelist all_legal;
    chess::movegen::legalmoves(all_legal, board);

    bool play_best = (unif(rng) < BEST_MOVE_PROB);

    chess::Move chosen;
    int chosen_eval = 0;
    std::string chosen_uci;

    if (play_best) {
      chosen_uci = winning_moves[0].uci;
      chosen = chess::uci::uciToMove(board, chosen_uci);
      chosen_eval = winning_moves[0].eval;
    } else {
      if (all_legal.empty())
        goto skip_child;
      int rand_idx = static_cast<int>(rng() % all_legal.size());
      chosen = all_legal[rand_idx];
      chosen_uci = chess::uci::moveToUci(chosen);
      // Look up this move's eval in CDB data if available
      chosen_eval = 0;
      for (const auto &m : moves) {
        if (m.uci == chosen_uci) {
          chosen_eval = m.eval;
          break;
        }
      }
    }

    {
      board.makeMove<true>(chosen);

      auto [reason, result] = board.isGameOver();
      bool skip = (reason == chess::GameResultReason::CHECKMATE ||
                   reason == chess::GameResultReason::STALEMATE);

      if (!skip) {
        const char *tag = play_best ? "L" : "R";
        out << board.getFen(false) << '\t' << tag << '\t'
            << chosen_uci << ':' << chosen_eval << '\n';
        stats.dead_lost_output.fetch_add(1, std::memory_order_relaxed);
      }

      board.unmakeMove(chosen);
    }
    skip_child:;
  }

  // Flush remaining entry count
  stats.entries_scanned.fetch_add(local_entries % 1'000'000,
                                  std::memory_order_relaxed);
  out.close();
}

// ============================================================================
// Progress reporter (runs in its own thread)
// ============================================================================

static void progress_thread(const ScanStats &stats,
                            std::atomic<bool> &done) {
  auto t0 = std::chrono::steady_clock::now();
  while (!done.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    auto now = std::chrono::steady_clock::now();
    double elapsed =
        std::chrono::duration<double>(now - t0).count();
    std::uint64_t scanned = stats.entries_scanned.load(std::memory_order_relaxed);
    std::uint64_t out = stats.positions_output.load(std::memory_order_relaxed);
    std::uint64_t lost = stats.dead_lost_output.load(std::memory_order_relaxed);
    double rate = elapsed > 0 ? scanned / elapsed / 1e6 : 0;
    std::cerr << "\r[" << std::fixed << std::setprecision(0) << elapsed
              << "s] scanned " << scanned / 1'000'000 << "M entries ("
              << std::setprecision(1) << rate << "M/s), output " << out
              << " won + " << lost << " lost     " << std::flush;
  }
  std::cerr << std::endl;
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char *argv[]) {
  std::string output_dir = ".";
  int num_threads = std::min(128, static_cast<int>(std::thread::hardware_concurrency()));

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if ((arg == "--threads" || arg == "-t") && i + 1 < argc) {
      num_threads = std::stoi(argv[++i]);
    } else if ((arg == "--output-dir" || arg == "-o") && i + 1 < argc) {
      output_dir = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cerr << "Usage: cdb_tactical [options]\n"
                << "  -t, --threads N      Number of scan threads (default: min(128, nproc))\n"
                << "  -o, --output-dir DIR Output directory (default: .)\n"
                << "  -h, --help           Show this help\n"
                << "\nDB path compiled in: " << CHESSDB_PATH << "\n"
                << "\nTunable thresholds (recompile to change):\n"
                << "  DECISIVE_CP        = " << DECISIVE_CP << "\n"
                << "  GOOD_MOVE_CP       = " << GOOD_MOVE_CP << "\n"
                << "  MIN_PIECES         = " << MIN_PIECES << "\n"
                << "  MIN_EXPLORED_MOVES = " << MIN_EXPLORED_MOVES << "\n"
                << "  OBVIOUS_THRESHOLD  = " << OBVIOUS_THRESHOLD << " cp\n"
                << "  OBVIOUS_WEIGHT     = " << OBVIOUS_WEIGHT << "\n"
                << "  GOOD_MOVE_BASE     = " << GOOD_MOVE_BASE << "\n";
      return 0;
    }
  }

  std::cerr << "Opening DB at: " << CHESSDB_PATH << std::endl;

  BlockBasedTableOptions table_options;
  table_options.block_cache =
      NewLRUCache(BLOCK_CACHE_GB * 1024LL * 1024LL * 1024LL);
  Options options;
  options.IncreaseParallelism();
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));

  DB *db = nullptr;
  Status s = DB::OpenForReadOnly(options, CHESSDB_PATH, &db);
  if (!s.ok()) {
    std::cerr << "Failed to open DB: " << s.ToString() << std::endl;
    return 1;
  }

  std::cerr << "DB opened. Scanning with " << num_threads
            << " threads, output to " << output_dir << "/" << std::endl;

  // Partition key space: all keys start with 'h' (0x68).  The second byte
  // ranges over 0x00-0xFF.  We split that range across threads.
  ScanStats stats;
  std::atomic<bool> done{false};

  // Start progress reporter
  std::thread reporter(progress_thread, std::cref(stats), std::ref(done));

  {
    ThreadPool pool(num_threads);
    int step = 256 / num_threads;
    int remainder = 256 % num_threads;

    int cursor = 0;
    for (int t = 0; t < num_threads; ++t) {
      int range_size = step + (t < remainder ? 1 : 0);
      int range_lo = cursor;
      int range_hi = cursor + range_size;
      cursor = range_hi;

      // Build start/end keys: "h" + one byte
      std::string start_key = std::string(1, 'h') + static_cast<char>(range_lo);
      std::string end_key;
      if (range_hi >= 256) {
        end_key = "i"; // past all 'h' keys
      } else {
        end_key = std::string(1, 'h') + static_cast<char>(range_hi);
      }

      pool.enqueue(scan_range, db, start_key, end_key, t, output_dir,
                   std::ref(stats));
    }
    // ThreadPool destructor joins all threads
  }

  done.store(true, std::memory_order_relaxed);
  reporter.join();

  std::uint64_t total_won = stats.positions_output.load();
  std::uint64_t total_lost = stats.dead_lost_output.load();
  std::uint64_t total_scanned = stats.entries_scanned.load();
  std::cerr << "\nDone. Scanned " << total_scanned << " entries.\n"
            << "Output: " << total_won << " dead-won positions, " << total_lost
            << " dead-lost positions.\n"
            << "Files: " << output_dir << "/tactical_positions.*.tsv\n";

  delete db;
  return 0;
}
