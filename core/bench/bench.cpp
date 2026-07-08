#include "irotoiro/bot.hpp"
#include "irotoiro/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace irotoiro;
using Clock = std::chrono::steady_clock;

struct PerftResult {
  int depth = 0;
  uint64_t leaves = 0;
  uint64_t applyCalls = 0;
  uint64_t checksum = 0;
  double ms = 0.0;
};

struct PlayoutResult {
  int games = 0;
  int plies = 0;
  uint64_t checksum = 0;
  double ms = 0.0;
};

struct MatchResult {
  int games = 0;
  int greedyWins = 0;
  int otherWins = 0;
  int draws = 0;
  int plies = 0;
  int greedyMoves = 0;
  double totalMs = 0.0;
  double greedyMs = 0.0;
  uint64_t checksum = 0;
};

enum class PlayerKind { Random, Greedy };

double elapsedMs(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

std::string compilerString() {
#if defined(__clang__)
  return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("GCC ") + __VERSION__;
#elif defined(_MSC_VER)
  return std::string("MSVC ") + std::to_string(_MSC_VER);
#else
  return "unknown";
#endif
}

std::string buildString() {
#ifdef NDEBUG
  return "NDEBUG on, C++20, irotoiro_bench target adds -O3";
#else
  return "NDEBUG off, C++20, irotoiro_bench target adds -O3";
#endif
}

void mixChecksum(uint64_t& checksum, const GameState& state) {
  checksum = checksum * 1315423911ull + static_cast<uint64_t>(state.score[0]) * 17ull +
             static_cast<uint64_t>(state.score[1]) * 31ull +
             static_cast<uint64_t>(occupiedCount(state)) * 43ull +
             static_cast<uint64_t>(handCount(state, 0)) * 59ull +
             static_cast<uint64_t>(handCount(state, 1)) * 71ull;
}

uint64_t perftExpectedOne(GameState state, int depth, Rng& rng, uint64_t& applyCalls,
                          uint64_t& checksum) {
  if (depth == 0 || isTerminal(state)) {
    mixChecksum(checksum, state);
    return 1;
  }

  uint64_t leaves = 0;
  const std::vector<Move> moves = legalPlacements(state);
  for (Move move : moves) {
    GameState next = state;
    applyTurn(next, move, rng);
    ++applyCalls;
    leaves += perftExpectedOne(next, depth - 1, rng, applyCalls, checksum);
  }
  return leaves;
}

PerftResult runPerft(int depth) {
  Rng rng(0x1A2B3C00u + static_cast<uint32_t>(depth));
  GameState state = initialState(rng);
  PerftResult result;
  result.depth = depth;

  const auto start = Clock::now();
  result.leaves = perftExpectedOne(state, depth, rng, result.applyCalls, result.checksum);
  result.ms = elapsedMs(start, Clock::now());
  return result;
}

PerftResult runRandomLines(int depth, int lines) {
  Rng rng(0x51515151u);
  PerftResult result;
  result.depth = depth;

  const auto start = Clock::now();
  for (int line = 0; line < lines; ++line) {
    GameState state = initialState(rng);
    int ply = 0;
    while (ply < depth && !isTerminal(state)) {
      Move move = randomMove(state, rng);
      applyTurn(state, move, rng);
      ++result.applyCalls;
      ++ply;
    }
    ++result.leaves;
    mixChecksum(result.checksum, state);
  }
  result.ms = elapsedMs(start, Clock::now());
  return result;
}

PlayoutResult runRandomPlayouts(int games) {
  Rng rng(0xC0FFEEu);
  PlayoutResult result;
  result.games = games;

  const auto start = Clock::now();
  for (int game = 0; game < games; ++game) {
    GameState state = initialState(rng);
    while (!isTerminal(state)) {
      Move move = randomMove(state, rng);
      applyTurn(state, move, rng);
      ++result.plies;
    }
    mixChecksum(result.checksum, state);
  }
  result.ms = elapsedMs(start, Clock::now());
  return result;
}

void addWin(MatchResult& result, const GameState& state, int greedyPlayer) {
  const int otherPlayer = 1 - greedyPlayer;
  if (state.score[greedyPlayer] > state.score[otherPlayer]) {
    ++result.greedyWins;
  } else if (state.score[greedyPlayer] < state.score[otherPlayer]) {
    ++result.otherWins;
  } else {
    ++result.draws;
  }
}

GameState playMatch(uint32_t seed, PlayerKind p0, PlayerKind p1, MatchResult& result,
                    int greedyWinPerspective) {
  Rng rng(seed);
  GameState state = initialState(rng);
  while (!isTerminal(state)) {
    Move move;
    const PlayerKind kind = state.toMove == 0 ? p0 : p1;
    if (kind == PlayerKind::Greedy) {
      const auto greedyStart = Clock::now();
      move = greedyMove(state, rng);
      result.greedyMs += elapsedMs(greedyStart, Clock::now());
      ++result.greedyMoves;
    } else {
      move = randomMove(state, rng);
    }
    applyTurn(state, move, rng);
    ++result.plies;
  }

  addWin(result, state, greedyWinPerspective);
  mixChecksum(result.checksum, state);
  return state;
}

MatchResult runGreedyVsRandom(int pairedGames) {
  MatchResult result;
  result.games = pairedGames * 2;
  const auto start = Clock::now();
  for (int game = 0; game < pairedGames; ++game) {
    playMatch(0xA0000000u + static_cast<uint32_t>(game), PlayerKind::Greedy, PlayerKind::Random,
              result, 0);
    playMatch(0xB0000000u + static_cast<uint32_t>(game), PlayerKind::Random, PlayerKind::Greedy,
              result, 1);
  }
  result.totalMs = elapsedMs(start, Clock::now());
  return result;
}

MatchResult runGreedyVsGreedy(int games) {
  MatchResult result;
  result.games = games;
  const auto start = Clock::now();
  for (int game = 0; game < games; ++game) {
    Rng rng(0xD0000000u + static_cast<uint32_t>(game));
    GameState state = initialState(rng);
    while (!isTerminal(state)) {
      const auto greedyStart = Clock::now();
      Move move = greedyMove(state, rng);
      result.greedyMs += elapsedMs(greedyStart, Clock::now());
      ++result.greedyMoves;
      applyTurn(state, move, rng);
      ++result.plies;
    }

    if (state.score[0] > state.score[1]) {
      ++result.greedyWins;
    } else if (state.score[0] < state.score[1]) {
      ++result.otherWins;
    } else {
      ++result.draws;
    }
    mixChecksum(result.checksum, state);
  }
  result.totalMs = elapsedMs(start, Clock::now());
  return result;
}

double mnodesPerSec(uint64_t applyCalls, double ms) {
  if (ms <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(applyCalls) / ms / 1000.0;
}

double perSec(int count, double ms) {
  if (ms <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(count) * 1000.0 / ms;
}

void printPerftTable(const std::vector<PerftResult>& rows) {
  std::cout << "perft EXPECTED-1 (all placements, draw chance sampled by one fixed Rng stream)\n";
  std::cout << "depth        leaves       apply calls        ms    Mnodes/s\n";
  for (const PerftResult& r : rows) {
    std::cout << std::setw(5) << r.depth << std::setw(14) << r.leaves << std::setw(18)
              << r.applyCalls << std::setw(10) << std::fixed << std::setprecision(2) << r.ms
              << std::setw(12) << std::setprecision(3) << mnodesPerSec(r.applyCalls, r.ms)
              << "\n";
  }
}

void printRandomLine(const PerftResult& r) {
  std::cout << "\nrandom single-line depth=" << r.depth << " lines=" << r.leaves << "\n";
  std::cout << "apply calls=" << r.applyCalls << "  ms=" << std::fixed << std::setprecision(2)
            << r.ms << "  Mnodes/s=" << std::setprecision(3)
            << mnodesPerSec(r.applyCalls, r.ms) << "\n";
}

void printPlayouts(const PlayoutResult& r) {
  std::cout << "\nrandom playouts\n";
  std::cout << "games=" << r.games << "  plies=" << r.plies << "  ms=" << std::fixed
            << std::setprecision(2) << r.ms << "  games/s=" << std::setprecision(1)
            << perSec(r.games, r.ms) << "  plies/s=" << std::setprecision(1)
            << perSec(r.plies, r.ms) << "\n";
}

void printMatch(const std::string& name, const MatchResult& r, const std::string& winLabel,
                const std::string& lossLabel) {
  const double greedyWinRate = r.games == 0 ? 0.0 : 100.0 * r.greedyWins / r.games;
  std::cout << "\n" << name << "\n";
  std::cout << "games=" << r.games << "  " << winLabel << "=" << r.greedyWins << "  "
            << lossLabel << "=" << r.otherWins << "  draws=" << r.draws << "  win%="
            << std::fixed << std::setprecision(1) << greedyWinRate << "  total ms="
            << std::setprecision(2) << r.totalMs << "\n";
  std::cout << "greedy moves=" << r.greedyMoves << "  greedy select ms=" << std::setprecision(2)
            << r.greedyMs << "  greedy moves/s=" << std::setprecision(1)
            << perSec(r.greedyMoves, r.greedyMs) << "\n";
}

}  // namespace

int main() {
  std::cout << "Irotoiro core benchmark\n";
  std::cout << "compiler: " << compilerString() << "\n";
  std::cout << "flags:    " << buildString() << "\n";
  std::cout << "seeds:    deterministic fixed constants; no wall-clock game seeds\n\n";

  std::vector<PerftResult> perftRows;
  for (int depth = 1; depth <= 4; ++depth) {
    perftRows.push_back(runPerft(depth));
  }
  printPerftTable(perftRows);

  const PerftResult randomLine = runRandomLines(24, 100000);
  printRandomLine(randomLine);

  const PlayoutResult playouts = runRandomPlayouts(100000);
  printPlayouts(playouts);

  const MatchResult greedyRandom = runGreedyVsRandom(1000);
  printMatch("greedy vs random (paired seats)", greedyRandom, "greedy wins", "random wins");

  const MatchResult greedyGreedy = runGreedyVsGreedy(1000);
  printMatch("greedy vs greedy", greedyGreedy, "player0 wins", "player1 wins");

  std::cout << "\nexpectimax: not implemented in this Stage 2 build (TODO)\n";

  double bestPerft = 0.0;
  for (const PerftResult& row : perftRows) {
    bestPerft = std::max(bestPerft, mnodesPerSec(row.applyCalls, row.ms));
  }
  std::cout << "\nsummary: best perft " << std::fixed << std::setprecision(3) << bestPerft
            << " Mnodes/s; random " << std::setprecision(1) << perSec(playouts.games, playouts.ms)
            << " games/s; greedy-v-random win " << std::setprecision(1)
            << (100.0 * greedyRandom.greedyWins / greedyRandom.games) << "%\n";

  return 0;
}
