#include "irotoiro/search.hpp"

#include "irotoiro/bot.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <vector>

namespace irotoiro {
namespace {

using Clock = std::chrono::steady_clock;

SearchStats g_lastStats;
constexpr std::size_t kMaxMoves = 75;
constexpr std::size_t kMaxChanceOutcomes = 64;
// Search values are always root-perspective and finite. The evaluator is
// intentionally small (score, hand, and pattern terms), while terminal wins use
// +/-1,024,000 at the extreme score diff, so this interval safely covers every
// leaf and terminal value used by expectimax.
constexpr double kValueMin = -2000000.0;
constexpr double kValueMax = 2000000.0;

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

uint64_t splitmix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

struct ZobristTables {
  std::array<std::array<uint64_t, 7>, 25> board{};
  std::array<std::array<std::array<uint64_t, 5>, 3>, 2> stock{};
  std::array<std::array<std::array<uint64_t, 7>, 3>, 2> hand{};
  std::array<std::array<uint64_t, 9>, 3> bag{};
  std::array<std::array<uint64_t, 25>, 2> score{};
  std::array<uint64_t, 2> toMove{};
  std::array<uint64_t, 2> maximizingPlayer{};
  std::array<uint64_t, 32> depth{};
};

ZobristTables makeZobristTables() {
  ZobristTables z;
  uint64_t seed = 0xD1B54A32D192ED03ull;
  auto next = [&]() {
    seed = splitmix64(seed);
    return seed;
  };

  for (auto& byCell : z.board) {
    for (uint64_t& value : byCell) {
      value = next();
    }
  }
  for (auto& byPlayer : z.stock) {
    for (auto& byColor : byPlayer) {
      for (uint64_t& value : byColor) {
        value = next();
      }
    }
  }
  for (auto& byPlayer : z.hand) {
    for (auto& byTile : byPlayer) {
      for (uint64_t& value : byTile) {
        value = next();
      }
    }
  }
  for (auto& byTile : z.bag) {
    for (uint64_t& value : byTile) {
      value = next();
    }
  }
  for (auto& byPlayer : z.score) {
    for (uint64_t& value : byPlayer) {
      value = next();
    }
  }
  for (uint64_t& value : z.toMove) {
    value = next();
  }
  for (uint64_t& value : z.maximizingPlayer) {
    value = next();
  }
  for (uint64_t& value : z.depth) {
    value = next();
  }
  return z;
}

const ZobristTables& zobristTables() {
  static const ZobristTables kTables = makeZobristTables();
  return kTables;
}

uint8_t boardCode(const Cell& cell) {
  if (cell.kind == Empty) {
    return 0;
  }
  if (cell.kind == Ohajiki) {
    return static_cast<uint8_t>(1 + cell.color);
  }
  return static_cast<uint8_t>(4 + cell.color);
}

uint64_t stateHash(const GameState& state, int depth, int maximizingPlayer) {
  const ZobristTables& z = zobristTables();
  uint64_t h = z.maximizingPlayer[maximizingPlayer] ^ z.toMove[state.toMove];
  if (depth >= 0 && static_cast<std::size_t>(depth) < z.depth.size()) {
    h ^= z.depth[static_cast<std::size_t>(depth)];
  } else {
    h ^= splitmix64(0xA0761D6478BD642Full ^ static_cast<uint64_t>(depth));
  }
  for (std::size_t cell = 0; cell < state.board.size(); ++cell) {
    h ^= z.board[cell][boardCode(state.board[cell])];
  }
  for (std::size_t player = 0; player < 2; ++player) {
    for (std::size_t color = 0; color < 3; ++color) {
      h ^= z.stock[player][color][state.stock[player][color]];
      h ^= z.hand[player][color][state.hand[player][color]];
    }
    h ^= z.score[player][state.score[player]];
  }
  for (std::size_t tile = 0; tile < 3; ++tile) {
    h ^= z.bag[tile][state.bag[tile]];
  }
  return h;
}

struct OrderedMove {
  Move move;
  DeterministicTurn resolved;
  int immediateScore = 0;
};

struct OrderedMoveList {
  std::array<OrderedMove, kMaxMoves> moves{};
  std::size_t count = 0;
};

OrderedMoveList orderedMoves(const GameState& state, bool maximizingNode) {
  OrderedMoveList ordered;
  const int player = state.toMove;
  const int before = scoreDiff(state, player);
  for (uint8_t cell = 0; cell < 25; ++cell) {
    if (state.board[cell].kind != Empty) {
      continue;
    }
    for (uint8_t color = 0; color < 3; ++color) {
      if (state.stock[player][color] == 0) {
        continue;
      }
      Move move{color, cell};
      DeterministicTurn resolved = resolveTurnDeterministic(state, move);
      ordered.moves[ordered.count++] =
          OrderedMove{move, resolved, scoreDiff(resolved.state, player) - before};
    }
  }
  std::stable_sort(ordered.moves.begin(), ordered.moves.begin() + ordered.count,
                   [&](const OrderedMove& a, const OrderedMove& b) {
                     if (a.immediateScore != b.immediateScore) {
                       return maximizingNode ? a.immediateScore > b.immediateScore
                                             : a.immediateScore < b.immediateScore;
                     }
                     return comesBefore(a.move, b.move);
                   });
  return ordered;
}

struct ChanceOutcomeBuffer {
  std::array<ChanceOutcome, kMaxChanceOutcomes> outcomes{};
  std::size_t count = 0;
};

bool sameState(const GameState& a, const GameState& b) {
  if (a.toMove != b.toMove || a.score != b.score || a.stock != b.stock || a.hand != b.hand ||
      a.bag != b.bag) {
    return false;
  }
  for (std::size_t i = 0; i < a.board.size(); ++i) {
    if (a.board[i].kind != b.board[i].kind || a.board[i].color != b.board[i].color) {
      return false;
    }
  }
  return true;
}

void addChanceOutcome(ChanceOutcomeBuffer& buffer, GameState state, double probability) {
  state.toMove = static_cast<uint8_t>(1 - state.toMove);
  for (std::size_t i = 0; i < buffer.count; ++i) {
    if (sameState(buffer.outcomes[i].state, state)) {
      buffer.outcomes[i].probability += probability;
      return;
    }
  }
  if (buffer.count < buffer.outcomes.size()) {
    buffer.outcomes[buffer.count++] = ChanceOutcome{probability, state};
  }
}

void enumerateDrawsInBatch(const GameState& state, int batch, int drawInBatch, int drawBatches,
                           double probability, ChanceOutcomeBuffer& buffer);

void enumerateDrawBatch(const GameState& state, int batch, int drawBatches, double probability,
                        ChanceOutcomeBuffer& buffer) {
  if (batch >= drawBatches) {
    addChanceOutcome(buffer, state, probability);
    return;
  }
  enumerateDrawsInBatch(state, batch, 0, drawBatches, probability, buffer);
}

void enumerateDrawsInBatch(const GameState& state, int batch, int drawInBatch, int drawBatches,
                           double probability, ChanceOutcomeBuffer& buffer) {
  if (drawInBatch >= 3 || handCount(state, state.toMove) >= 6 || bagCount(state) == 0) {
    enumerateDrawBatch(state, batch + 1, drawBatches, probability, buffer);
    return;
  }

  const int player = state.toMove;
  const int total = bagCount(state);
  for (int tile = 0; tile < 3; ++tile) {
    if (state.bag[tile] == 0) {
      continue;
    }
    GameState next = state;
    const double p = static_cast<double>(next.bag[tile]) / static_cast<double>(total);
    --next.bag[tile];
    ++next.hand[player][tile];
    enumerateDrawsInBatch(next, batch, drawInBatch + 1, drawBatches, probability * p, buffer);
  }
}

ChanceOutcomeBuffer enumerateDrawOutcomesFast(const GameState& preDrawState, int drawBatches) {
  ChanceOutcomeBuffer buffer;
  enumerateDrawBatch(preDrawState, 0, drawBatches, 1.0, buffer);
  return buffer;
}

struct SearchContext {
  int maximizingPlayer = 0;
  bool useDeadline = false;
  Clock::time_point deadline{};
  bool aborted = false;
  SearchStats stats;

  enum class BoundFlag : uint8_t { Exact, Lower, Upper };

  struct TTEntry {
    double value = 0.0;
    BoundFlag flag = BoundFlag::Exact;
  };

  struct BoundResult {
    double value = 0.0;
    BoundFlag flag = BoundFlag::Exact;
  };

  std::unordered_map<uint64_t, TTEntry> tt;

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

using BoundFlag = SearchContext::BoundFlag;
using BoundResult = SearchContext::BoundResult;

double expectiminimaxExactValue(const GameState& state, int depth, SearchContext& ctx);
BoundResult expectiminimaxPrunedValue(const GameState& state, int depth, double alpha, double beta,
                                      SearchContext& ctx);

double chanceValueExact(const DeterministicTurn& resolved, int depth, SearchContext& ctx) {
  const ChanceOutcomeBuffer outcomes =
      enumerateDrawOutcomesFast(resolved.state, resolved.drawBatches);
  ctx.stats.chanceOutcomes += outcomes.count;

  double value = 0.0;
  for (std::size_t i = 0; i < outcomes.count; ++i) {
    if (ctx.aborted) {
      return 0.0;
    }
    const ChanceOutcome& outcome = outcomes.outcomes[i];
    value += outcome.probability * expectiminimaxExactValue(outcome.state, depth - 1, ctx);
  }
  return value;
}

double expectiminimaxExactValue(const GameState& state, int depth, SearchContext& ctx) {
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

  const bool maximizingNode = state.toMove == ctx.maximizingPlayer;
  double best = maximizingNode ? -std::numeric_limits<double>::infinity()
                               : std::numeric_limits<double>::infinity();
  const OrderedMoveList moves = orderedMoves(state, maximizingNode);
  for (std::size_t i = 0; i < moves.count; ++i) {
    const OrderedMove& ordered = moves.moves[i];
    const double value = chanceValueExact(ordered.resolved, depth, ctx);
    if (ctx.aborted) {
      return 0.0;
    }
    if (maximizingNode) {
      best = std::max(best, value);
    } else {
      best = std::min(best, value);
    }
  }

  return best;
}

BoundResult chanceValuePruned(const DeterministicTurn& resolved, int depth, double alpha,
                              double beta, SearchContext& ctx) {
  const ChanceOutcomeBuffer outcomes =
      enumerateDrawOutcomesFast(resolved.state, resolved.drawBatches);
  ctx.stats.chanceOutcomes += outcomes.count;

  double value = 0.0;
  double remainingProbability = 0.0;
  for (std::size_t i = 0; i < outcomes.count; ++i) {
    remainingProbability += outcomes.outcomes[i].probability;
  }

  for (std::size_t i = 0; i < outcomes.count; ++i) {
    if (ctx.aborted) {
      return BoundResult{};
    }

    const ChanceOutcome& outcome = outcomes.outcomes[i];
    remainingProbability -= outcome.probability;

    const double p = outcome.probability;
    const double childAlpha = (alpha - value - remainingProbability * kValueMax) / p;
    const double childBeta = (beta - value - remainingProbability * kValueMin) / p;
    const BoundResult child =
        expectiminimaxPrunedValue(outcome.state, depth - 1, childAlpha, childBeta, ctx);
    if (ctx.aborted) {
      return BoundResult{};
    }

    if (child.flag == BoundFlag::Upper) {
      const double upper = value + p * child.value + remainingProbability * kValueMax;
      return BoundResult{upper, BoundFlag::Upper};
    }
    if (child.flag == BoundFlag::Lower) {
      const double lower = value + p * child.value + remainingProbability * kValueMin;
      return BoundResult{lower, BoundFlag::Lower};
    }

    value += p * child.value;

    const double lower = value + remainingProbability * kValueMin;
    const double upper = value + remainingProbability * kValueMax;
    if (upper <= alpha) {
      return BoundResult{upper, BoundFlag::Upper};
    }
    if (lower >= beta) {
      return BoundResult{lower, BoundFlag::Lower};
    }
  }

  return BoundResult{value, BoundFlag::Exact};
}

BoundResult expectiminimaxPrunedValue(const GameState& state, int depth, double alpha, double beta,
                                      SearchContext& ctx) {
  ++ctx.stats.nodes;
  if (ctx.expired()) {
    return BoundResult{};
  }
  if (isTerminal(state)) {
    return BoundResult{terminalValue(state, ctx.maximizingPlayer), BoundFlag::Exact};
  }
  if (depth <= 0) {
    return BoundResult{static_cast<double>(evaluate(state, ctx.maximizingPlayer)),
                       BoundFlag::Exact};
  }

  const double originalAlpha = alpha;
  const double originalBeta = beta;
  const uint64_t key = stateHash(state, depth, ctx.maximizingPlayer);
  const auto hit = ctx.tt.find(key);
  if (hit != ctx.tt.end()) {
    const SearchContext::TTEntry& entry = hit->second;
    if (entry.flag == BoundFlag::Exact ||
        (entry.flag == BoundFlag::Lower && entry.value >= beta) ||
        (entry.flag == BoundFlag::Upper && entry.value <= alpha)) {
      return BoundResult{entry.value, entry.flag};
    }
  }

  const bool maximizingNode = state.toMove == ctx.maximizingPlayer;
  double best = maximizingNode ? kValueMin : kValueMax;
  const OrderedMoveList moves = orderedMoves(state, maximizingNode);
  for (std::size_t i = 0; i < moves.count; ++i) {
    const OrderedMove& ordered = moves.moves[i];
    const BoundResult child = chanceValuePruned(ordered.resolved, depth, alpha, beta, ctx);
    if (ctx.aborted) {
      return BoundResult{};
    }

    if (maximizingNode) {
      if (child.value > best) {
        best = child.value;
      }
      if (best > alpha) {
        alpha = best;
      }
      if (alpha >= beta) {
        break;
      }
    } else {
      if (child.value < best) {
        best = child.value;
      }
      if (best < beta) {
        beta = best;
      }
      if (alpha >= beta) {
        break;
      }
    }
  }

  BoundFlag flag = BoundFlag::Exact;
  if (best <= originalAlpha) {
    flag = BoundFlag::Upper;
  } else if (best >= originalBeta) {
    flag = BoundFlag::Lower;
  }

  if (!ctx.aborted) {
    ctx.tt[key] = SearchContext::TTEntry{best, flag};
  }
  return BoundResult{best, flag};
}

struct RootResult {
  Move move;
  double value = 0.0;
  bool completed = false;
};

RootResult searchRootExact(const GameState& state, int depth, SearchContext& ctx) {
  const bool maximizingNode = state.toMove == ctx.maximizingPlayer;
  const OrderedMoveList moves = orderedMoves(state, maximizingNode);
  if (moves.count == 0) {
    return RootResult{};
  }

  Move best = moves.moves[0].move;
  double bestValue = -std::numeric_limits<double>::infinity();
  bool haveBest = false;

  for (std::size_t i = 0; i < moves.count; ++i) {
    const OrderedMove& ordered = moves.moves[i];
    const double value = chanceValueExact(ordered.resolved, depth, ctx);
    if (ctx.aborted) {
      return RootResult{best, bestValue, false};
    }
    if (!haveBest || value > bestValue ||
        (std::abs(value - bestValue) <= 1e-12 && comesBefore(ordered.move, best))) {
      bestValue = value;
      best = ordered.move;
      haveBest = true;
    }
  }

  return RootResult{best, bestValue, true};
}

RootResult searchRootPruned(const GameState& state, int depth, SearchContext& ctx) {
  OrderedMoveList moves = orderedMoves(state, true);
  if (moves.count == 0) {
    return RootResult{};
  }
  std::stable_sort(moves.moves.begin(), moves.moves.begin() + moves.count,
                   [](const OrderedMove& a, const OrderedMove& b) {
                     return comesBefore(a.move, b.move);
                   });

  Move best = moves.moves[0].move;
  double bestValue = kValueMin;
  double alpha = kValueMin;
  constexpr double beta = kValueMax;
  bool haveBest = false;

  for (std::size_t i = 0; i < moves.count; ++i) {
    const OrderedMove& ordered = moves.moves[i];
    const BoundResult child = chanceValuePruned(ordered.resolved, depth, alpha, beta, ctx);
    if (ctx.aborted) {
      return RootResult{best, bestValue, false};
    }

    if (child.flag == BoundFlag::Exact &&
        (!haveBest || child.value > bestValue ||
         (std::abs(child.value - bestValue) <= 1e-12 && comesBefore(ordered.move, best)))) {
      bestValue = child.value;
      best = ordered.move;
      haveBest = true;
    } else if (!haveBest && child.flag != BoundFlag::Upper) {
      bestValue = child.value;
      best = ordered.move;
      haveBest = true;
    }

    if (haveBest && bestValue > alpha) {
      alpha = bestValue;
    }
  }

  return RootResult{best, bestValue, haveBest};
}

int remainingPlies(const GameState& state) {
  return stockCount(state, 0) + stockCount(state, 1);
}

double elapsedMs(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

ExpectimaxResult expectimaxMoveExact(const GameState& state, int maxDepth) {
  const std::vector<Move> legal = legalPlacements(state);
  if (legal.empty()) {
    return ExpectimaxResult{};
  }

  const int depth = std::max(1, maxDepth);
  SearchContext ctx;
  ctx.maximizingPlayer = state.toMove;
  const auto start = Clock::now();
  const RootResult result = searchRootExact(state, depth, ctx);
  ctx.stats.completedDepth = result.completed ? depth : 0;
  ctx.stats.elapsedMs = elapsedMs(start, Clock::now());
  return ExpectimaxResult{result.completed ? result.move : legal.front(), result.value, ctx.stats,
                          result.completed};
}

ExpectimaxResult expectimaxMovePruned(const GameState& state, int maxDepth) {
  const std::vector<Move> legal = legalPlacements(state);
  if (legal.empty()) {
    return ExpectimaxResult{};
  }

  const int depth = std::max(1, maxDepth);
  SearchContext ctx;
  ctx.maximizingPlayer = state.toMove;
  const auto start = Clock::now();
  const RootResult result = searchRootPruned(state, depth, ctx);
  ctx.stats.completedDepth = result.completed ? depth : 0;
  ctx.stats.elapsedMs = elapsedMs(start, Clock::now());
  return ExpectimaxResult{result.completed ? result.move : legal.front(), result.value, ctx.stats,
                          result.completed};
}

Move expectimaxMove(const GameState& state, int maxDepth) {
  ExpectimaxResult result = expectimaxMovePruned(state, maxDepth);
  g_lastStats = result.stats;
  return result.move;
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
    RootResult result = searchRootPruned(state, depth, ctx);
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
