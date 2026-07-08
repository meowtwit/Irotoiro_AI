#include "irotoiro/search.hpp"

#include "irotoiro/bot.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace irotoiro {
namespace {

using Clock = std::chrono::steady_clock;

SearchStats g_lastStats;

bool comesBefore(Move a, Move b) {
  if (a.cell != b.cell) {
    return a.cell < b.cell;
  }
  return a.color < b.color;
}

int scoreDiff(const GameState& state, int player) {
  return static_cast<int>(state.score[player]) - static_cast<int>(state.score[1 - player]);
}

double terminalValue(const GameState& state, int maximizingPlayer) {
  const int diff = scoreDiff(state, maximizingPlayer);
  if (diff > 0) {
    return 1000000.0 + static_cast<double>(diff) * 1000.0;
  }
  if (diff < 0) {
    return -1000000.0 + static_cast<double>(diff) * 1000.0;
  }
  return 0.0;
}

std::string stateKey(const GameState& state, int depth, int maximizingPlayer) {
  std::string key;
  key.reserve(25 * 2 + 2 * 3 + 2 * 3 + 3 + 2 + 4);
  key.push_back(static_cast<char>(depth));
  key.push_back(static_cast<char>(maximizingPlayer));
  for (const Cell& cell : state.board) {
    key.push_back(static_cast<char>(cell.kind));
    key.push_back(static_cast<char>(cell.color));
  }
  for (const auto& stock : state.stock) {
    for (uint8_t n : stock) {
      key.push_back(static_cast<char>(n));
    }
  }
  for (const auto& hand : state.hand) {
    for (uint8_t n : hand) {
      key.push_back(static_cast<char>(n));
    }
  }
  for (uint8_t n : state.bag) {
    key.push_back(static_cast<char>(n));
  }
  for (uint8_t n : state.score) {
    key.push_back(static_cast<char>(n));
  }
  key.push_back(static_cast<char>(state.toMove));
  return key;
}

struct OrderedMove {
  Move move;
  int immediateScore = 0;
};

std::vector<OrderedMove> orderedMoves(const GameState& state, bool maximizingNode) {
  const std::vector<Move> legal = legalPlacements(state);
  std::vector<OrderedMove> ordered;
  ordered.reserve(legal.size());
  const int player = state.toMove;
  const int before = scoreDiff(state, player);
  for (Move move : legal) {
    const DeterministicTurn resolved = resolveTurnDeterministic(state, move);
    ordered.push_back(OrderedMove{move, scoreDiff(resolved.state, player) - before});
  }
  std::stable_sort(ordered.begin(), ordered.end(), [&](const OrderedMove& a, const OrderedMove& b) {
    if (a.immediateScore != b.immediateScore) {
      return maximizingNode ? a.immediateScore > b.immediateScore
                            : a.immediateScore < b.immediateScore;
    }
    return comesBefore(a.move, b.move);
  });
  return ordered;
}

struct SearchContext {
  int maximizingPlayer = 0;
  bool useDeadline = false;
  Clock::time_point deadline{};
  bool aborted = false;
  SearchStats stats;
  std::unordered_map<std::string, double> tt;

  bool expired() {
    if (!useDeadline || (stats.nodes & 1023ull) != 0ull) {
      return false;
    }
    if (Clock::now() >= deadline) {
      aborted = true;
      stats.timedOut = true;
      return true;
    }
    return false;
  }
};

double expectiminimax(const GameState& state, int depth, SearchContext& ctx);

double chanceValue(const GameState& state, Move move, int depth, SearchContext& ctx) {
  const DeterministicTurn resolved = resolveTurnDeterministic(state, move);
  const std::vector<ChanceOutcome> outcomes =
      enumerateDrawOutcomes(resolved.state, resolved.drawBatches);
  ctx.stats.chanceOutcomes += outcomes.size();

  double value = 0.0;
  for (const ChanceOutcome& outcome : outcomes) {
    if (ctx.aborted) {
      return 0.0;
    }
    value += outcome.probability * expectiminimax(outcome.state, depth - 1, ctx);
  }
  return value;
}

double expectiminimax(const GameState& state, int depth, SearchContext& ctx) {
  ++ctx.stats.nodes;
  if (ctx.expired()) {
    return 0.0;
  }
  if (isTerminal(state)) {
    return terminalValue(state, ctx.maximizingPlayer);
  }
  if (depth <= 0) {
    return static_cast<double>(evaluate(state, ctx.maximizingPlayer));
  }

  const std::string key = stateKey(state, depth, ctx.maximizingPlayer);
  const auto hit = ctx.tt.find(key);
  if (hit != ctx.tt.end()) {
    return hit->second;
  }

  const bool maximizingNode = state.toMove == ctx.maximizingPlayer;
  double best = maximizingNode ? -std::numeric_limits<double>::infinity()
                               : std::numeric_limits<double>::infinity();
  const std::vector<OrderedMove> moves = orderedMoves(state, maximizingNode);
  for (const OrderedMove& ordered : moves) {
    const double value = chanceValue(state, ordered.move, depth, ctx);
    if (ctx.aborted) {
      return 0.0;
    }
    if (maximizingNode) {
      best = std::max(best, value);
    } else {
      best = std::min(best, value);
    }
  }

  ctx.tt.emplace(key, best);
  return best;
}

struct RootResult {
  Move move;
  bool completed = false;
};

RootResult searchRoot(const GameState& state, int depth, SearchContext& ctx) {
  const std::vector<Move> legal = legalPlacements(state);
  if (legal.empty()) {
    return RootResult{};
  }

  const bool maximizingNode = state.toMove == ctx.maximizingPlayer;
  const std::vector<OrderedMove> moves = orderedMoves(state, maximizingNode);
  Move best = legal.front();
  double bestValue = -std::numeric_limits<double>::infinity();
  bool haveBest = false;

  for (const OrderedMove& ordered : moves) {
    const double value = chanceValue(state, ordered.move, depth, ctx);
    if (ctx.aborted) {
      return RootResult{best, false};
    }
    if (!haveBest || value > bestValue ||
        (std::abs(value - bestValue) <= 1e-12 && comesBefore(ordered.move, best))) {
      bestValue = value;
      best = ordered.move;
      haveBest = true;
    }
  }

  return RootResult{best, true};
}

int remainingPlies(const GameState& state) {
  return stockCount(state, 0) + stockCount(state, 1);
}

double elapsedMs(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

Move expectimaxMove(const GameState& state, int maxDepth) {
  const std::vector<Move> legal = legalPlacements(state);
  if (legal.empty()) {
    g_lastStats = SearchStats{};
    return Move{};
  }

  const int depth = std::max(1, maxDepth);
  SearchContext ctx;
  ctx.maximizingPlayer = state.toMove;
  const auto start = Clock::now();
  const RootResult result = searchRoot(state, depth, ctx);
  ctx.stats.completedDepth = result.completed ? depth : 0;
  ctx.stats.elapsedMs = elapsedMs(start, Clock::now());
  g_lastStats = ctx.stats;
  return result.completed ? result.move : legal.front();
}

Move expectimaxMoveTimed(const GameState& state, int timeBudgetMs) {
  const std::vector<Move> legal = legalPlacements(state);
  if (legal.empty()) {
    g_lastStats = SearchStats{};
    return Move{};
  }

  SearchContext ctx;
  ctx.maximizingPlayer = state.toMove;
  ctx.useDeadline = true;
  const auto start = Clock::now();
  ctx.deadline = start + std::chrono::milliseconds(std::max(1, timeBudgetMs));

  Move best = legal.front();
  const int maxDepth = std::max(1, remainingPlies(state));
  for (int depth = 1; depth <= maxDepth; ++depth) {
    RootResult result = searchRoot(state, depth, ctx);
    if (!result.completed || ctx.aborted) {
      break;
    }
    best = result.move;
    ctx.stats.completedDepth = depth;
    if (Clock::now() >= ctx.deadline) {
      ctx.stats.timedOut = true;
      break;
    }
  }

  ctx.stats.elapsedMs = elapsedMs(start, Clock::now());
  g_lastStats = ctx.stats;
  return best;
}

SearchStats lastExpectimaxStats() {
  return g_lastStats;
}

}  // namespace irotoiro
