#include "irotoiro/bot.hpp"

#include <algorithm>
#include <cassert>

namespace irotoiro {
namespace {

int other(int player) {
  return 1 - player;
}

bool isAllKindOrEmpty(const GameState& state, const Window& window, CellKind kind, int& kindCount,
                      int& emptyCount) {
  kindCount = 0;
  emptyCount = 0;
  for (uint8_t cell : window.cells) {
    const Cell& c = state.board[cell];
    if (c.kind == kind) {
      ++kindCount;
    } else if (c.kind == Empty) {
      ++emptyCount;
    } else {
      return false;
    }
  }
  return true;
}

int potentialAabBonus(const GameState& state, int player) {
  int bonus = 0;
  for (const Window& window : windows()) {
    int ohaCount = 0;
    int emptyCount = 0;
    if (!isAllKindOrEmpty(state, window, Ohajiki, ohaCount, emptyCount) || ohaCount != 2 ||
        emptyCount != 1) {
      continue;
    }

    std::array<int, 3> colors = {0, 0, 0};
    int emptyCell = -1;
    for (uint8_t cell : window.cells) {
      if (state.board[cell].kind == Ohajiki) {
        ++colors[state.board[cell].color];
      } else {
        emptyCell = cell;
      }
    }

    for (int placedColor = 0; placedColor < 3; ++placedColor) {
      if (state.stock[player][placedColor] == 0) {
        continue;
      }
      std::array<int, 3> after = colors;
      ++after[placedColor];

      int distinct = 0;
      int majority = -1;
      int minority = -1;
      for (int c = 0; c < 3; ++c) {
        if (after[c] > 0) {
          ++distinct;
        }
        if (after[c] == 2) {
          majority = c;
        } else if (after[c] == 1) {
          minority = c;
        }
      }
      if (distinct == 2 && majority >= 0 && minority >= 0) {
        const int tile = mixTile(majority, minority);
        if (state.hand[player][tile] > 0 && emptyCell >= 0) {
          bonus += 8;
          if (placedColor == minority) {
            bonus += 2;
          }
        }
      }
    }
  }
  return bonus;
}

int tileReachBonus(const GameState& state, int player) {
  int bonus = 0;
  const int opponent = other(player);
  for (const Window& window : windows()) {
    int tileCount = 0;
    int emptyCount = 0;
    if (!isAllKindOrEmpty(state, window, Tile, tileCount, emptyCount) || tileCount != 2 ||
        emptyCount != 1) {
      continue;
    }

    if (state.score[opponent] > 0) {
      bonus += 6;
    } else if (handCount(state, opponent) > 0 && handCount(state, player) < 6) {
      bonus += 3;
    } else {
      bonus += 1;
    }
  }
  return bonus;
}

int handUtility(const GameState& state, int player) {
  int value = 2 * handCount(state, player);
  for (int tile = 0; tile < 3; ++tile) {
    if (state.hand[player][tile] > 0) {
      value += 1;
    }
  }
  return value;
}

bool comesBefore(Move a, Move b) {
  if (a.cell != b.cell) {
    return a.cell < b.cell;
  }
  return a.color < b.color;
}

}  // namespace

int evaluate(const GameState& state, int player) {
  const int opponent = other(player);
  int score = 100 * (static_cast<int>(state.score[player]) - static_cast<int>(state.score[opponent]));
  score += potentialAabBonus(state, player);
  score -= potentialAabBonus(state, opponent) / 2;
  score += tileReachBonus(state, player);
  score -= tileReachBonus(state, opponent) / 2;
  score += handUtility(state, player);
  score -= handUtility(state, opponent) / 2;
  return score;
}

Move greedyMove(const GameState& state, Rng& rng) {
  const std::vector<Move> moves = legalPlacements(state);
  assert(!moves.empty());

  const int player = state.toMove;
  Move best = moves.front();
  int bestScore = -1000000000;

  for (Move move : moves) {
    GameState next = state;
    Rng candidateRng = rng;
    applyTurn(next, move, candidateRng);
    const int score = evaluate(next, player);
    if (score > bestScore || (score == bestScore && comesBefore(move, best))) {
      bestScore = score;
      best = move;
    }
  }

  return best;
}

}  // namespace irotoiro
