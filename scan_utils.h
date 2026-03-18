#pragma once

#include <cctype>
#include <cstring>
#include <string>

// Backprop: convert a child score (from CDB value) to a parent eval via negamax.
// Special values: -30001 = TB draw/stalemate, >=15000 = TB/mate win, <=-15000 = TB/mate loss.
inline int backprop_score(int child_score) {
  if (child_score == -30001)
    return 0;
  if (child_score >= 15000)
    return -child_score + 1;
  if (child_score <= -15000)
    return -child_score - 1;
  return -child_score;
}

inline int count_pieces_in_fen_board(const std::string &fen) {
  size_t space_pos = fen.find(' ');
  const std::string board =
      fen.substr(0, space_pos == std::string::npos ? fen.size() : space_pos);
  int count = 0;
  for (char c : board) {
    if (std::isalpha(static_cast<unsigned char>(c)))
      ++count;
  }
  return count;
}

// Returns true if all castling flags in the FEN are consistent with standard
// chess (king on e-file, rooks on a/h files).  Rejects Chess960/FRC positions.
inline bool obeys_standard_castling_constraints(const std::string &fen) {
  size_t p1 = fen.find(' ');
  if (p1 == std::string::npos)
    return false;
  size_t p2 = fen.find(' ', p1 + 1);
  if (p2 == std::string::npos)
    return false;
  size_t p3 = fen.find(' ', p2 + 1);
  if (p3 == std::string::npos)
    return false;
  const std::string board = fen.substr(0, p1);
  const std::string castling = fen.substr(p2 + 1, p3 - (p2 + 1));

  char squares[64];
  std::memset(squares, ' ', sizeof(squares));
  int idx = 0;
  for (char c : board) {
    if (c == '/')
      continue;
    if (c >= '1' && c <= '8') {
      idx += c - '0';
    } else if (std::isalpha(static_cast<unsigned char>(c))) {
      if (idx >= 0 && idx < 64)
        squares[idx++] = c;
    }
  }

  auto piece_at = [&](char file, int rank) -> char {
    int col = file - 'a';
    int row = 8 - rank;
    int i = row * 8 + col;
    if (i < 0 || i >= 64)
      return ' ';
    return squares[i];
  };

  if (castling.find('K') != std::string::npos) {
    if (piece_at('e', 1) != 'K' || piece_at('h', 1) != 'R')
      return false;
  }
  if (castling.find('Q') != std::string::npos) {
    if (piece_at('e', 1) != 'K' || piece_at('a', 1) != 'R')
      return false;
  }
  if (castling.find('k') != std::string::npos) {
    if (piece_at('e', 8) != 'k' || piece_at('h', 8) != 'r')
      return false;
  }
  if (castling.find('q') != std::string::npos) {
    if (piece_at('e', 8) != 'k' || piece_at('a', 8) != 'r')
      return false;
  }
  return true;
}
