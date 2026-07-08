#pragma once

#include "irotoiro/engine.hpp"

#include <cstdint>

namespace irotoiro {

struct SearchStats {
  uint64_t nodes = 0;
  uint64_t chanceOutcomes = 0;
  int completedDepth = 0;
  double elapsedMs = 0.0;
  bool timedOut = false;
};

struct ExpectimaxResult {
  Move move;
  double value = 0.0;
  SearchStats stats;
  bool completed = false;
};

ExpectimaxResult expectimaxMoveExact(const GameState& state, int maxDepth);
ExpectimaxResult expectimaxMovePruned(const GameState& state, int maxDepth);
Move expectimaxMove(const GameState& state, int maxDepth);
Move expectimaxMoveTimed(const GameState& state, int timeBudgetMs);
SearchStats lastExpectimaxStats();

}  // namespace irotoiro
