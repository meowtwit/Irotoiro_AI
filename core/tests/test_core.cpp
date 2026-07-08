#include "irotoiro/engine.hpp"
#include "irotoiro/bot.hpp"
#include "irotoiro/search.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace irotoiro;

struct TestContext {
  int passed = 0;
  int failed = 0;
};

#define CHECK(ctx, expr)                                                                       \
  do {                                                                                         \
    if (expr) {                                                                                \
      ++(ctx).passed;                                                                          \
    } else {                                                                                   \
      ++(ctx).failed;                                                                          \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " << #expr << "\n";      \
    }                                                                                          \
  } while (false)

#define CHECK_EQ(ctx, actual, expected)                                                        \
  do {                                                                                         \
    const auto actual_value = (actual);                                                        \
    const auto expected_value = (expected);                                                    \
    if (actual_value == expected_value) {                                                      \
      ++(ctx).passed;                                                                          \
    } else {                                                                                   \
      ++(ctx).failed;                                                                          \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " << #actual           \
                << " == " << #expected << " (actual=" << actual_value                        \
                << ", expected=" << expected_value << ")\n";                                 \
    }                                                                                          \
  } while (false)

struct GameResult {
  int plies = 0;
  int exchanges = 0;
  int steals = 0;
  int drawEvents = 0;
  GameState state;
};

GameResult playRandomGame(uint32_t seed, bool checkEveryTurn, TestContext& ctx) {
  Rng rng(seed);
  GameState state = initialState(rng);
  GameResult result;
  std::string invariantError;
  CHECK(ctx, checkInvariants(state, &invariantError));

  while (!isTerminal(state)) {
    Move move = randomMove(state, rng);
    TurnStats stats = applyTurn(state, move, rng);
    ++result.plies;
    result.exchanges += stats.exchanges;
    result.steals += stats.steals;
    result.drawEvents += stats.drawEvents;
    if (checkEveryTurn) {
      invariantError.clear();
      if (!checkInvariants(state, &invariantError)) {
        ++ctx.failed;
        std::cerr << "Invariant failed after seed=" << seed << " ply=" << result.plies << ": "
                  << invariantError << "\n";
      } else {
        ++ctx.passed;
      }
    }
  }

  result.state = state;
  return result;
}

GameState edgeState() {
  GameState s;
  s.board.fill(Cell{});
  s.stock = {{{4, 4, 4}, {4, 4, 4}}};
  s.hand = {{{0, 0, 0}, {0, 0, 0}}};
  s.bag = {0, 0, 0};
  s.score = {0, 0};
  s.toMove = 0;
  return s;
}

void putOha(GameState& s, int cell, int color) {
  s.board[cell] = Cell{Ohajiki, static_cast<uint8_t>(color)};
}

void putTile(GameState& s, int cell, int color) {
  s.board[cell] = Cell{Tile, static_cast<uint8_t>(color)};
}

void testGoldenSeeds(TestContext& ctx) {
  struct Golden {
    uint32_t seed;
    int plies;
    int score0;
    int score1;
    std::array<std::array<int, 3>, 2> hand;
    std::array<int, 3> bag;
    const char* board;
    int exchanges;
    int steals;
    int drawEvents;
  };

  const std::vector<Golden> goldens = {
      {1, 24, 1, 6, {{{1, 1, 4}, {4, 1, 1}}}, {0, 3, 2}, "rrPbPrGbbGPGbryr.yOyybbyr", 7, 1, 5},
      {7, 24, 4, 4, {{{2, 2, 0}, {0, 0, 4}}}, {2, 4, 2}, "yGybyrbPby.yPrObPGrPyryOr", 8, 1, 5},
      {42, 24, 1, 7, {{{1, 2, 3}, {3, 2, 0}}}, {2, 1, 2}, "rrObPyyOrryGbGrrGyOPbyrb.", 8, 1, 5},
      {100, 24, 0, 12, {{{1, 0, 2}, {2, 1, 3}}}, {2, 1, 0}, "PbGGGyyGGbO.GyPOrPyrrybOr", 12, 5, 7},
      {2024, 24, 6, 6, {{{0, 3, 1}, {3, 0, 3}}}, {1, 0, 1}, "rrPbbrOPOGrPOPGGGbyr.byGy", 12, 9, 6},
  };

  for (const Golden& g : goldens) {
    GameResult r = playRandomGame(g.seed, false, ctx);
    CHECK_EQ(ctx, r.plies, g.plies);
    CHECK_EQ(ctx, static_cast<int>(r.state.score[0]), g.score0);
    CHECK_EQ(ctx, static_cast<int>(r.state.score[1]), g.score1);
    for (int p = 0; p < 2; ++p) {
      for (int t = 0; t < 3; ++t) {
        CHECK_EQ(ctx, static_cast<int>(r.state.hand[p][t]), g.hand[p][t]);
      }
    }
    for (int t = 0; t < 3; ++t) {
      CHECK_EQ(ctx, static_cast<int>(r.state.bag[t]), g.bag[t]);
    }
    CHECK_EQ(ctx, boardSignature(r.state), std::string(g.board));
    CHECK_EQ(ctx, r.exchanges, g.exchanges);
    CHECK_EQ(ctx, r.steals, g.steals);
    CHECK_EQ(ctx, r.drawEvents, g.drawEvents);
  }
}

void testAggregateParityAndInvariants(TestContext& ctx) {
  int games = 0;
  int totalPlies = 0;
  int totalExchanges = 0;
  int totalSteals = 0;
  int totalDrawEvents = 0;
  uint32_t scoreHash = 0;
  int nonTerminal = 0;
  int boards24Occupied = 0;

  for (uint32_t seed = 1; seed <= 1000; ++seed) {
    GameResult r = playRandomGame(seed, true, ctx);
    ++games;
    totalPlies += r.plies;
    totalExchanges += r.exchanges;
    totalSteals += r.steals;
    totalDrawEvents += r.drawEvents;
    scoreHash = static_cast<uint32_t>(static_cast<uint64_t>(scoreHash) * 31u +
                                      static_cast<uint32_t>(r.state.score[0]) * 7u +
                                      static_cast<uint32_t>(r.state.score[1]));
    if (!isTerminal(r.state)) {
      ++nonTerminal;
    }
    if (occupiedCount(r.state) == 24) {
      ++boards24Occupied;
    }
  }

  CHECK_EQ(ctx, games, 1000);
  CHECK_EQ(ctx, totalPlies, 24000);
  CHECK_EQ(ctx, totalExchanges, 9229);
  CHECK_EQ(ctx, totalSteals, 2583);
  CHECK_EQ(ctx, totalDrawEvents, 6254);
  CHECK_EQ(ctx, scoreHash, 2950933045u);
  CHECK_EQ(ctx, nonTerminal, 0);
  CHECK_EQ(ctx, boards24Occupied, 1000);
}

void testAabExchange(TestContext& ctx) {
  GameState s = edgeState();
  putOha(s, 0, R);
  putOha(s, 1, R);
  s.hand[0][P] = 1;
  Rng rng(1);

  TurnStats stats = applyTurn(s, Move{B, 2}, rng);

  CHECK_EQ(ctx, stats.exchanges, 1);
  CHECK_EQ(ctx, static_cast<int>(s.score[0]), 1);
  CHECK_EQ(ctx, static_cast<int>(s.hand[0][P]), 0);
  CHECK_EQ(ctx, s.board[2].kind, Tile);
  CHECK_EQ(ctx, static_cast<int>(s.board[2].color), P);
}

void testTilePointSteal(TestContext& ctx) {
  GameState s = edgeState();
  putOha(s, 0, R);
  putOha(s, 2, R);
  putTile(s, 6, G);
  putTile(s, 11, O);
  s.hand[0][P] = 1;
  s.score[1] = 2;
  Rng rng(1);

  TurnStats stats = applyTurn(s, Move{B, 1}, rng);

  CHECK_EQ(ctx, stats.exchanges, 1);
  CHECK_EQ(ctx, stats.steals, 1);
  CHECK_EQ(ctx, static_cast<int>(s.score[0]), 2);
  CHECK_EQ(ctx, static_cast<int>(s.score[1]), 1);
  CHECK_EQ(ctx, s.board[1].kind, Tile);
  CHECK_EQ(ctx, static_cast<int>(s.board[1].color), P);
}

void testTileStealAndCapBlock(TestContext& ctx) {
  {
    GameState s = edgeState();
    putOha(s, 0, R);
    putOha(s, 2, R);
    putTile(s, 6, G);
    putTile(s, 11, O);
    s.hand[0][P] = 1;
    s.hand[1] = {1, 3, 2};
    Rng rng(1);

    TurnStats stats = applyTurn(s, Move{B, 1}, rng);

    CHECK_EQ(ctx, stats.steals, 1);
    CHECK_EQ(ctx, handCount(s, 0), 1);
    CHECK_EQ(ctx, static_cast<int>(s.hand[0][G]), 1);
    CHECK_EQ(ctx, handCount(s, 1), 5);
    CHECK_EQ(ctx, static_cast<int>(s.hand[1][G]), 2);
  }

  {
    GameState s = edgeState();
    putOha(s, 0, R);
    putOha(s, 2, R);
    putTile(s, 6, G);
    putTile(s, 11, O);
    putTile(s, 7, P);
    putTile(s, 13, G);
    s.hand[0] = {1, 3, 2};
    s.hand[1] = {2, 2, 2};
    Rng rng(1);

    TurnStats stats = applyTurn(s, Move{B, 1}, rng);

    CHECK_EQ(ctx, stats.exchanges, 1);
    CHECK_EQ(ctx, stats.steals, 1);
    CHECK_EQ(ctx, handCount(s, 0), 6);
    CHECK_EQ(ctx, handCount(s, 1), 5);
  }
}

void testSharedMinorityOnlyOneExchange(TestContext& ctx) {
  GameState s = edgeState();
  putOha(s, 6, R);
  putOha(s, 7, R);
  putOha(s, 11, R);
  putOha(s, 15, R);
  s.hand[0][P] = 2;
  Rng rng(1);

  TurnStats stats = applyTurn(s, Move{B, 5}, rng);

  CHECK_EQ(ctx, stats.exchanges, 1);
  CHECK_EQ(ctx, static_cast<int>(s.score[0]), 1);
  CHECK_EQ(ctx, static_cast<int>(s.hand[0][P]), 1);
  CHECK_EQ(ctx, s.board[5].kind, Tile);
  CHECK_EQ(ctx, static_cast<int>(s.board[5].color), P);
}

void testSameMixOnlyOneAvailable(TestContext& ctx) {
  GameState s = edgeState();
  putOha(s, 0, R);
  putOha(s, 2, R);
  putOha(s, 6, B);
  putOha(s, 11, R);
  s.hand[0][P] = 1;
  Rng rng(1);

  TurnStats stats = applyTurn(s, Move{B, 1}, rng);

  CHECK_EQ(ctx, stats.exchanges, 1);
  CHECK_EQ(ctx, static_cast<int>(s.score[0]), 1);
  CHECK_EQ(ctx, static_cast<int>(s.hand[0][P]), 0);
  CHECK_EQ(ctx, s.board[1].kind, Tile);
  CHECK_EQ(ctx, s.board[11].kind, Ohajiki);
}

void testEdgeCases(TestContext& ctx) {
  testAabExchange(ctx);
  testTilePointSteal(ctx);
  testTileStealAndCapBlock(ctx);
  testSharedMinorityOnlyOneExchange(ctx);
  testSameMixOnlyOneAvailable(ctx);
}

void testGreedyTakesImmediateExchange(TestContext& ctx) {
  GameState s = edgeState();
  putOha(s, 0, R);
  putOha(s, 1, R);
  s.stock = {{{0, 1, 0}, {0, 0, 0}}};
  s.hand[0][P] = 1;
  Rng rng(123);

  Move move = greedyMove(s, rng);

  CHECK_EQ(ctx, static_cast<int>(move.cell), 2);
  CHECK_EQ(ctx, static_cast<int>(move.color), B);
}

void testGreedyReturnsLegalMoves(TestContext& ctx) {
  for (uint32_t seed = 10; seed < 30; ++seed) {
    Rng rng(seed);
    GameState s = initialState(rng);
    while (!isTerminal(s)) {
      const std::vector<Move> legal = legalPlacements(s);
      Move move = greedyMove(s, rng);
      const bool found = std::any_of(legal.begin(), legal.end(), [&](Move legalMove) {
        return legalMove.cell == move.cell && legalMove.color == move.color;
      });
      CHECK(ctx, found);
      applyTurn(s, move, rng);
    }
  }
}

void testChanceOutcomesSumAndConserveTiles(TestContext& ctx) {
  GameState s = edgeState();
  putOha(s, 0, R);
  putOha(s, 1, R);
  s.bag = {2, 1, 0};
  s.stock = {{{1, 0, 0}, {0, 0, 0}}};

  const DeterministicTurn resolved = resolveTurnDeterministic(s, Move{R, 2});
  CHECK_EQ(ctx, resolved.drawBatches, 1);

  const int preHand = handCount(resolved.state, 0) + handCount(resolved.state, 1);
  const int preBag = bagCount(resolved.state);
  const std::vector<ChanceOutcome> outcomes =
      enumerateDrawOutcomes(resolved.state, resolved.drawBatches);

  double probabilitySum = 0.0;
  double expectedHand = 0.0;
  double expectedBag = 0.0;
  for (const ChanceOutcome& outcome : outcomes) {
    probabilitySum += outcome.probability;
    const int hand = handCount(outcome.state, 0) + handCount(outcome.state, 1);
    const int bag = bagCount(outcome.state);
    expectedHand += outcome.probability * static_cast<double>(hand);
    expectedBag += outcome.probability * static_cast<double>(bag);
    CHECK_EQ(ctx, hand + bag, preHand + preBag);
    CHECK_EQ(ctx, outcome.state.toMove, 1);
  }

  CHECK(ctx, std::abs(probabilitySum - 1.0) <= 1e-9);
  CHECK(ctx, std::abs(expectedHand - static_cast<double>(preHand + 3)) <= 1e-9);
  CHECK(ctx, std::abs(expectedBag - static_cast<double>(preBag - 3)) <= 1e-9);
}

void testExpectimaxTakesImmediateWinningExchange(TestContext& ctx) {
  GameState s = edgeState();
  putOha(s, 0, R);
  putOha(s, 1, R);
  s.stock = {{{0, 1, 0}, {0, 0, 0}}};
  s.hand[0][P] = 1;

  const Move first = expectimaxMove(s, 1);
  const Move second = expectimaxMove(s, 2);

  CHECK_EQ(ctx, static_cast<int>(first.cell), 2);
  CHECK_EQ(ctx, static_cast<int>(first.color), B);
  CHECK_EQ(ctx, static_cast<int>(second.cell), 2);
  CHECK_EQ(ctx, static_cast<int>(second.color), B);

  const std::vector<Move> legal = legalPlacements(s);
  const bool found = std::any_of(legal.begin(), legal.end(), [&](Move legalMove) {
    return legalMove.cell == second.cell && legalMove.color == second.color;
  });
  CHECK(ctx, found);
}

}  // namespace

int main() {
  TestContext ctx;

  testGoldenSeeds(ctx);
  testAggregateParityAndInvariants(ctx);
  testEdgeCases(ctx);
  testGreedyTakesImmediateExchange(ctx);
  testGreedyReturnsLegalMoves(ctx);
  testChanceOutcomesSumAndConserveTiles(ctx);
  testExpectimaxTakesImmediateWinningExchange(ctx);

  const int total = ctx.passed + ctx.failed;
  std::cout << "Irotoiro core tests\n";
  std::cout << "  assertions: " << total << "\n";
  std::cout << "  passed:     " << ctx.passed << "\n";
  std::cout << "  failed:     " << ctx.failed << "\n";
  if (ctx.failed == 0) {
    std::cout << "  result:     PASS\n";
    return EXIT_SUCCESS;
  }
  std::cout << "  result:     FAIL\n";
  return EXIT_FAILURE;
}
