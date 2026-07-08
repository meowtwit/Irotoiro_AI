#include "irotoiro/engine.hpp"

#include <cassert>
#include <cmath>

namespace irotoiro {
namespace {

std::array<Window, 48> makeWindows() {
  std::array<Window, 48> result{};
  int n = 0;
  auto idx = [](int r, int c) { return static_cast<uint8_t>(r * 5 + c); };

  for (int r = 0; r < 5; ++r) {
    for (int c = 0; c < 3; ++c) {
      result[n++].cells = {idx(r, c), idx(r, c + 1), idx(r, c + 2)};
    }
  }
  for (int c = 0; c < 5; ++c) {
    for (int r = 0; r < 3; ++r) {
      result[n++].cells = {idx(r, c), idx(r + 1, c), idx(r + 2, c)};
    }
  }
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      result[n++].cells = {idx(r, c), idx(r + 1, c + 1), idx(r + 2, c + 2)};
    }
  }
  for (int r = 0; r < 3; ++r) {
    for (int c = 2; c < 5; ++c) {
      result[n++].cells = {idx(r, c), idx(r + 1, c - 1), idx(r + 2, c - 2)};
    }
  }

  assert(n == 48);
  return result;
}

std::array<std::vector<uint8_t>, 25> makeWindowsThrough() {
  std::array<std::vector<uint8_t>, 25> result{};
  const auto& ws = windows();
  for (uint8_t wi = 0; wi < ws.size(); ++wi) {
    for (uint8_t cell : ws[wi].cells) {
      result[cell].push_back(wi);
    }
  }
  return result;
}

bool isAllKind(const GameState& state, const Window& window, CellKind kind) {
  for (uint8_t cell : window.cells) {
    if (state.board[cell].kind != kind) {
      return false;
    }
  }
  return true;
}

}  // namespace

double Rng::next() {
  a_ = a_ + 0x6D2B79F5u;
  uint32_t t = (a_ ^ (a_ >> 15)) * (1u | a_);
  t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
  return static_cast<double>(t ^ (t >> 14)) / 4294967296.0;
}

int mixTile(int a, int b) {
  const int s = a + b;
  if (s == 1) {
    return P;
  }
  if (s == 3) {
    return G;
  }
  return O;
}

const std::array<Window, 48>& windows() {
  static const std::array<Window, 48> kWindows = makeWindows();
  return kWindows;
}

const std::array<std::vector<uint8_t>, 25>& windowsThroughCell() {
  static const std::array<std::vector<uint8_t>, 25> kWindowsThrough = makeWindowsThrough();
  return kWindowsThrough;
}

int handCount(const GameState& state, int player) {
  return state.hand[player][0] + state.hand[player][1] + state.hand[player][2];
}

int bagCount(const GameState& state) {
  return state.bag[0] + state.bag[1] + state.bag[2];
}

int stockCount(const GameState& state, int player) {
  return state.stock[player][0] + state.stock[player][1] + state.stock[player][2];
}

int occupiedCount(const GameState& state) {
  int count = 0;
  for (const Cell& cell : state.board) {
    if (cell.kind != Empty) {
      ++count;
    }
  }
  return count;
}

bool isTerminal(const GameState& state) {
  return stockCount(state, 0) == 0 && stockCount(state, 1) == 0;
}

int drawOneToHand(GameState& state, int player, Rng& rng) {
  if (handCount(state, player) >= 6) {
    return -1;
  }
  const int total = bagCount(state);
  if (total == 0) {
    return -1;
  }

  int r = static_cast<int>(std::floor(rng.next() * static_cast<double>(total)));
  for (int t = 0; t < 3; ++t) {
    if (r < state.bag[t]) {
      --state.bag[t];
      ++state.hand[player][t];
      return t;
    }
    r -= state.bag[t];
  }
  return -1;
}

GameState initialState(Rng& rng) {
  GameState state;
  state.board.fill(Cell{});
  state.stock = {{{4, 4, 4}, {4, 4, 4}}};
  state.hand = {{{0, 0, 0}, {0, 0, 0}}};
  state.bag = {8, 8, 8};
  state.score = {0, 0};
  state.toMove = 0;

  for (int player = 0; player < 2; ++player) {
    for (int n = 0; n < 2; ++n) {
      drawOneToHand(state, player, rng);
    }
  }
  return state;
}

std::vector<Move> legalPlacements(const GameState& state) {
  std::vector<Move> moves;
  const int player = state.toMove;
  for (uint8_t cell = 0; cell < 25; ++cell) {
    if (state.board[cell].kind != Empty) {
      continue;
    }
    for (uint8_t color = 0; color < 3; ++color) {
      if (state.stock[player][color] > 0) {
        moves.push_back(Move{color, cell});
      }
    }
  }
  return moves;
}

Move randomMove(const GameState& state, Rng& rng) {
  const std::vector<Move> moves = legalPlacements(state);
  assert(!moves.empty());
  const int idx = static_cast<int>(std::floor(rng.next() * static_cast<double>(moves.size())));
  return moves[idx];
}

TurnStats applyTurn(GameState& state, Move move, Rng& rng) {
  const int player = state.toMove;
  const int opponent = 1 - player;
  TurnStats stats;

  state.board[move.cell] = Cell{Ohajiki, move.color};
  --state.stock[player][move.color];

  struct AabWindow {
    uint8_t minorityCell = 0;
    uint8_t mix = 0;
  };

  std::vector<uint8_t> drawWindowIndices;
  std::vector<AabWindow> aabWindows;

  for (uint8_t wi : windowsThroughCell()[move.cell]) {
    const Window& window = windows()[wi];
    if (!isAllKind(state, window, Ohajiki)) {
      continue;
    }

    std::array<int, 3> count = {0, 0, 0};
    for (uint8_t cell : window.cells) {
      ++count[state.board[cell].color];
    }

    int distinct = 0;
    for (int n : count) {
      if (n > 0) {
        ++distinct;
      }
    }

    if (distinct == 1 || distinct == 3) {
      drawWindowIndices.push_back(wi);
      continue;
    }

    int minority = -1;
    int majority = -1;
    for (int color = 0; color < 3; ++color) {
      if (count[color] == 1) {
        minority = color;
      } else if (count[color] == 2) {
        majority = color;
      }
    }

    uint8_t minorityCell = 0;
    for (uint8_t cell : window.cells) {
      if (state.board[cell].color == minority) {
        minorityCell = cell;
        break;
      }
    }
    aabWindows.push_back(AabWindow{minorityCell, static_cast<uint8_t>(mixTile(majority, minority))});
  }

  std::array<bool, 25> consumed{};
  consumed.fill(false);
  std::vector<uint8_t> placedTileCells;

  for (const AabWindow& window : aabWindows) {
    if (consumed[window.minorityCell]) {
      continue;
    }
    if (state.hand[player][window.mix] == 0) {
      continue;
    }

    --state.hand[player][window.mix];
    ++state.score[player];
    state.board[window.minorityCell] = Cell{Tile, window.mix};
    consumed[window.minorityCell] = true;
    placedTileCells.push_back(window.minorityCell);
    ++stats.exchanges;
  }

  std::array<bool, 48> seenTileWindows{};
  seenTileWindows.fill(false);
  for (uint8_t placedCell : placedTileCells) {
    for (uint8_t wi : windowsThroughCell()[placedCell]) {
      if (seenTileWindows[wi]) {
        continue;
      }
      const Window& window = windows()[wi];
      if (!isAllKind(state, window, Tile)) {
        continue;
      }

      seenTileWindows[wi] = true;
      if (state.score[opponent] > 0) {
        --state.score[opponent];
        ++state.score[player];
        ++stats.steals;
      } else if (handCount(state, player) < 6 && handCount(state, opponent) > 0) {
        int tile = -1;
        int best = -1;
        for (int k = 0; k < 3; ++k) {
          if (state.hand[opponent][k] > best) {
            best = state.hand[opponent][k];
            tile = k;
          }
        }
        if (tile >= 0 && state.hand[opponent][tile] > 0) {
          --state.hand[opponent][tile];
          ++state.hand[player][tile];
          ++stats.steals;
        }
      }
    }
  }

  for (size_t k = 0; k < drawWindowIndices.size(); ++k) {
    bool gotAny = false;
    for (int n = 0; n < 3; ++n) {
      const int tile = drawOneToHand(state, player, rng);
      if (tile < 0) {
        break;
      }
      gotAny = true;
    }
    if (gotAny) {
      ++stats.drawEvents;
    }
  }

  state.toMove = static_cast<uint8_t>(opponent);
  return stats;
}

std::string boardSignature(const GameState& state) {
  std::string result;
  result.reserve(25);
  for (const Cell& cell : state.board) {
    if (cell.kind == Empty) {
      result.push_back('.');
    } else if (cell.kind == Ohajiki) {
      static constexpr char kOha[] = {'r', 'b', 'y'};
      result.push_back(kOha[cell.color]);
    } else {
      static constexpr char kTile[] = {'P', 'G', 'O'};
      result.push_back(kTile[cell.color]);
    }
  }
  return result;
}

bool checkInvariants(const GameState& state, std::string* error) {
  int boardOhajiki = 0;
  int boardTiles = 0;
  for (const Cell& cell : state.board) {
    if (cell.kind == Ohajiki) {
      ++boardOhajiki;
    } else if (cell.kind == Tile) {
      ++boardTiles;
    }
  }

  int totalStock = 0;
  int totalHand = 0;
  for (int p = 0; p < 2; ++p) {
    for (int c = 0; c < 3; ++c) {
      totalStock += state.stock[p][c];
      totalHand += state.hand[p][c];
    }
  }

  const int totalScore = state.score[0] + state.score[1];
  const int totalBag = bagCount(state);
  if (boardOhajiki + totalStock + totalScore != 24) {
    if (error != nullptr) {
      *error = "ohajiki total invariant failed";
    }
    return false;
  }
  if (boardTiles + totalHand + totalBag != 24) {
    if (error != nullptr) {
      *error = "tile total invariant failed";
    }
    return false;
  }
  for (int p = 0; p < 2; ++p) {
    if (handCount(state, p) > 6) {
      if (error != nullptr) {
        *error = "hand cap invariant failed";
      }
      return false;
    }
  }
  return true;
}

}  // namespace irotoiro
