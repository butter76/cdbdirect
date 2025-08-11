#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rocksdb/db.h"
#include "rocksdb/iterator.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"

#include "cdbdirect.h"
#include "fen2cdb.h"

using namespace TERARKDB_NAMESPACE;

// Reuse the same backprop logic as in cdbdirect.cpp
static int backprop_score_for_scan(int child_score) {
  if (child_score == -30001)
    return 0;                       // TB draw/stalemate
  if (child_score >= 15000)
    return -child_score + 1;        // forced loss for side to move
  if (child_score <= -15000)
    return -child_score - 1;        // forced win for side to move
  return -child_score;              // regular eval
}

static void print_match(const std::string &fen,
                        const std::pair<std::string, int> &best,
                        std::uint64_t matched_count,
                        std::uint64_t total_count) {
  double frac = total_count ? (static_cast<double>(matched_count) / static_cast<double>(total_count)) : 0.0;
  std::cout << fen << " | best " << best.first << " = " << best.second
            << " | out=" << matched_count << "/" << total_count
            << " (" << std::fixed << std::setprecision(6) << (frac * 100.0) << "%)"
            << std::endl;
}

static int count_pieces_in_fen_board(const std::string &fen) {
  size_t space_pos = fen.find(' ');
  const std::string board = fen.substr(0, space_pos == std::string::npos ? fen.size() : space_pos);
  int count = 0;
  for (char c : board) {
    if (std::isalpha(static_cast<unsigned char>(c))) ++count;
  }
  return count;
}

static bool obeys_standard_castling_constraints(const std::string &fen) {
  // Split FEN fields
  size_t p1 = fen.find(' ');
  if (p1 == std::string::npos) return false;
  size_t p2 = fen.find(' ', p1 + 1);
  if (p2 == std::string::npos) return false;
  size_t p3 = fen.find(' ', p2 + 1);
  if (p3 == std::string::npos) return false;
  const std::string board = fen.substr(0, p1);
  const std::string castling = fen.substr(p2 + 1, p3 - (p2 + 1));

  // Parse board into 64 squares, a8..h1
  char squares[64];
  std::memset(squares, ' ', sizeof(squares));
  int idx = 0;
  for (char c : board) {
    if (c == '/') continue;
    if (c >= '1' && c <= '8') {
      idx += c - '0';
    } else if (std::isalpha(static_cast<unsigned char>(c))) {
      if (idx >= 0 && idx < 64) squares[idx++] = c;
    }
  }

  auto piece_at = [&](char file, int rank) -> char {
    int col = file - 'a';
    int row = 8 - rank; // rank 8 -> row 0, rank 1 -> row 7
    int i = row * 8 + col;
    if (i < 0 || i >= 64) return ' ';
    return squares[i];
  };

  // Only enforce constraints for flags that are present
  if (castling.find('K') != std::string::npos) {
    if (piece_at('e', 1) != 'K' || piece_at('h', 1) != 'R') return false;
  }
  if (castling.find('Q') != std::string::npos) {
    if (piece_at('e', 1) != 'K' || piece_at('a', 1) != 'R') return false;
  }
  if (castling.find('k') != std::string::npos) {
    if (piece_at('e', 8) != 'k' || piece_at('h', 8) != 'r') return false;
  }
  if (castling.find('q') != std::string::npos) {
    if (piece_at('e', 8) != 'k' || piece_at('a', 8) != 'r') return false;
  }
  return true;
}

int main() {
  // Open DB directly here because we need iterator access
  BlockBasedTableOptions table_options;
  table_options.block_cache = NewLRUCache(32 * 1024 * 1024 * 1024LL);
  Options options;
  options.IncreaseParallelism();
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));

  DB *db = nullptr;
  Status s = DB::OpenForReadOnly(options, CHESSDB_PATH, &db);
  if (!s.ok()) {
    std::cerr << s.ToString() << std::endl;
    return 1;
  }

  std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));

  // Iterate over all entries whose key starts with 'h' (hash entries)
  std::uint64_t total_positions = 0;
  std::uint64_t matched_positions = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const Slice k = it->key();
    if (k.size() == 0 || k.data()[0] != 'h') {
      continue; // skip non-hash records
    }

    const Slice v = it->value();

    // Decode move/score list
    std::vector<StrPair> scoredMoves;
    get_hash_values(v.ToString(), scoredMoves);
    if (scoredMoves.empty()) continue;
    ++total_positions;

    // Track best move after backprop and capture ply distance
    int ply_distance = -1;
    bool have_move = false;
    std::pair<std::string, int> best_move_score;

    for (const auto &p : scoredMoves) {
      if (p.first == "a0a0") {
        ply_distance = std::stoi(p.second);
        continue;
      }
      int child = std::stoi(p.second);
      int eval = backprop_score_for_scan(child);
      if (!have_move || eval > best_move_score.second) {
        best_move_score = {p.first, eval};
        have_move = true;
      }
    }

    if (!have_move) continue;

    // Filter by threshold and exclude TB draw/stalemate (=0)
    if (best_move_score.second > 250 || best_move_score.second < -250) {
      // Reconstruct original hex from key without leading 'h'
      std::string hex_key = bin2hex(k.ToString().substr(1));
      std::string fen = cbhexfen2fen(hex_key);
      // Filter out positions with 7 or fewer pieces
      if (count_pieces_in_fen_board(fen) <= 7) {
        continue;
      }
      // Enforce classical castling constraints (exclude Chess960-style rights)
      if (!obeys_standard_castling_constraints(fen)) {
        continue;
      }
      // Output position and best move with eval
      ++matched_positions;
      print_match(fen, best_move_score, matched_positions, total_positions);
    }
  }

  if (!it->status().ok()) {
    std::cerr << it->status().ToString() << std::endl;
  }

  delete db;
  return 0;
}


