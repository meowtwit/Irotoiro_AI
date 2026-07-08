#include "irotoiro/bot.hpp"
#include "irotoiro/engine.hpp"
#include "irotoiro/search.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace irotoiro;
using Clock = std::chrono::steady_clock;

struct CliOptions {
  bool color = true;
  uint32_t seed = 1;
};

struct PlayOptions : CliOptions {
  int human = 0;
  bool fixedDepth = false;
  int aiDepth = 0;
  int aiMs = 300;
};

enum class AgentKind { Random, Greedy, ExpectimaxDepth, ExpectimaxTimed };

struct AgentSpec {
  AgentKind kind = AgentKind::Greedy;
  int budget = 0;
  std::string name;
};

struct MatchOptions : CliOptions {
  AgentSpec a;
  AgentSpec b;
  int games = 0;
};

struct MoveTiming {
  int moves = 0;
  double ms = 0.0;
};

struct MatchStats {
  int games = 0;
  int aWins = 0;
  int bWins = 0;
  int draws = 0;
  int plies = 0;
  uint64_t checksum = 0;
  double totalMs = 0.0;
  double aScore = 0.0;
  double bScore = 0.0;
  MoveTiming agent[2];
};

struct NarratedTurn {
  TurnStats stats;
  std::vector<int> drawnTiles;
};

std::string lower(std::string s) {
  for (char& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

std::string ansi(const std::string& code, bool enabled) {
  return enabled ? "\033[" + code + "m" : "";
}

const char* ohaShort(int color) {
  static constexpr const char* kNames[] = {"R", "B", "Y"};
  return kNames[color];
}

const char* ohaName(int color) {
  static constexpr const char* kNames[] = {"red/R/赤", "blue/B/青", "yellow/Y/黄"};
  return kNames[color];
}

const char* tileShort(int tile) {
  static constexpr const char* kNames[] = {"P", "G", "O"};
  return kNames[tile];
}

const char* tileName(int tile) {
  static constexpr const char* kNames[] = {"purple/P/紫", "green/G/緑", "orange/O/橙"};
  return kNames[tile];
}

std::string colorizeOha(int color, bool enabled) {
  static constexpr const char* kCodes[] = {"31;1", "34;1", "33;1"};
  return ansi(kCodes[color], enabled) + ohaShort(color) + ansi("0", enabled);
}

std::string colorizeTile(int tile, bool enabled) {
  static constexpr const char* kCodes[] = {"35;1", "32;1", "38;5;208;1"};
  return ansi(kCodes[tile], enabled) + tileShort(tile) + ansi("0", enabled);
}

std::string cellText(const Cell& cell, bool color) {
  if (cell.kind == Empty) {
    return ".";
  }
  if (cell.kind == Ohajiki) {
    return colorizeOha(cell.color, color);
  }
  return colorizeTile(cell.color, color);
}

void printCounts(const std::array<uint8_t, 3>& values, const char* a, const char* b,
                 const char* c) {
  std::cout << a << "=" << static_cast<int>(values[0]) << " " << b << "="
            << static_cast<int>(values[1]) << " " << c << "=" << static_cast<int>(values[2]);
}

void renderBoard(const GameState& state, bool color) {
  std::cout << "\nBoard (cell:piece)\n";
  for (int r = 0; r < 5; ++r) {
    for (int c = 0; c < 5; ++c) {
      const int idx = r * 5 + c;
      std::cout << std::setw(2) << std::setfill('0') << idx << ":" << cellText(state.board[idx], color)
                << std::setfill(' ') << "  ";
    }
    std::cout << "\n";
  }
  std::cout << "Bag: ";
  printCounts(state.bag, "P", "G", "O");
  std::cout << "\n";
  for (int p = 0; p < 2; ++p) {
    std::cout << "Player " << p << " score=" << static_cast<int>(state.score[p])
              << "  stock ";
    printCounts(state.stock[p], "R", "B", "Y");
    std::cout << "  hand ";
    printCounts(state.hand[p], "P", "G", "O");
    if (state.toMove == p) {
      std::cout << "  <- to move";
    }
    std::cout << "\n";
  }
}

void printHelp() {
  std::cout << "Input format: <color> <cell>, for example: r 12\n"
            << "  color: r=red/赤, b=blue/青, y=yellow/黄\n"
            << "  cell: 0..24, shown on the board as row-major indices\n"
            << "Commands: help, board, quit\n"
            << "Rules summary: place one ohajiki on an empty cell. A newly formed 3-ohajiki line\n"
            << "draws tiles for AAA/ABC, or may exchange AAB using the matching mixed tile\n"
            << "(P=R+B, G=B+Y, O=R+Y) for +1 point. Three tiles in a line steal a point\n"
            << "or, if the opponent has no score, one tile when hand capacity allows.\n";
}

std::string cellsString(const Window& window) {
  std::ostringstream out;
  out << static_cast<int>(window.cells[0]) << "-" << static_cast<int>(window.cells[1]) << "-"
      << static_cast<int>(window.cells[2]);
  return out.str();
}

std::string windowColors(const GameState& state, const Window& window) {
  std::ostringstream out;
  for (int i = 0; i < 3; ++i) {
    if (i > 0) {
      out << ",";
    }
    out << ohaShort(state.board[window.cells[i]].color);
  }
  return out.str();
}

bool allKind(const GameState& state, const Window& window, CellKind kind) {
  for (uint8_t cell : window.cells) {
    if (state.board[cell].kind != kind) {
      return false;
    }
  }
  return true;
}

void narrateLineCandidates(const GameState& placed, Move move) {
  bool any = false;
  for (uint8_t wi : windowsThroughCell()[move.cell]) {
    const Window& window = windows()[wi];
    if (!allKind(placed, window, Ohajiki)) {
      continue;
    }

    std::array<int, 3> count = {0, 0, 0};
    for (uint8_t cell : window.cells) {
      ++count[placed.board[cell].color];
    }
    int distinct = 0;
    for (int n : count) {
      if (n > 0) {
        ++distinct;
      }
    }

    any = true;
    std::cout << "  ohajiki line " << cellsString(window) << " (" << windowColors(placed, window)
              << "): ";
    if (distinct == 1) {
      std::cout << "AAA, tile draw event\n";
    } else if (distinct == 3) {
      std::cout << "ABC, tile draw event\n";
    } else {
      int majority = -1;
      int minority = -1;
      for (int color = 0; color < 3; ++color) {
        if (count[color] == 2) {
          majority = color;
        } else if (count[color] == 1) {
          minority = color;
        }
      }
      std::cout << "AAB, exchange candidate using " << tileName(mixTile(majority, minority))
                << "\n";
    }
  }
  if (!any) {
    std::cout << "  no ohajiki 3-line formed by the placed piece\n";
  }
}

std::vector<uint8_t> newTileCells(const GameState& before, const GameState& after) {
  std::vector<uint8_t> cells;
  for (uint8_t cell = 0; cell < 25; ++cell) {
    if (before.board[cell].kind != Tile && after.board[cell].kind == Tile) {
      cells.push_back(cell);
    }
  }
  return cells;
}

void narrateTileLines(const GameState& beforeDeterministic, const GameState& after,
                      const std::vector<uint8_t>& placedTiles) {
  std::array<bool, 48> seen{};
  bool any = false;
  for (uint8_t cell : placedTiles) {
    for (uint8_t wi : windowsThroughCell()[cell]) {
      if (seen[wi]) {
        continue;
      }
      const Window& window = windows()[wi];
      if (!allKind(after, window, Tile)) {
        continue;
      }
      seen[wi] = true;
      any = true;
      std::cout << "  tile line " << cellsString(window) << " triggered a steal";
      const int player = beforeDeterministic.toMove;
      const int opponent = 1 - player;
      if (after.score[opponent] < beforeDeterministic.score[opponent]) {
        std::cout << " (stole 1 score)";
      } else if (after.hand != beforeDeterministic.hand) {
        std::cout << " (stole 1 tile)";
      } else {
        std::cout << " (no effect)";
      }
      std::cout << "\n";
    }
  }
  if (!any && !placedTiles.empty()) {
    std::cout << "  no tile 3-line formed by exchanged tile(s)\n";
  }
}

NarratedTurn applyTurnNarrated(GameState& state, Move move, Rng& rng, bool verbose) {
  const GameState before = state;
  GameState placed = state;
  placed.board[move.cell] = Cell{Ohajiki, move.color};
  --placed.stock[state.toMove][move.color];

  if (verbose) {
    std::cout << "Player " << static_cast<int>(state.toMove) << " placed " << ohaName(move.color)
              << " at cell " << static_cast<int>(move.cell) << ".\n";
    narrateLineCandidates(placed, move);
  }

  DeterministicTurn resolved = resolveTurnDeterministic(state, move);
  const std::vector<uint8_t> tiles = newTileCells(placed, resolved.state);
  if (verbose) {
    for (uint8_t cell : tiles) {
      std::cout << "  exchange: cell " << static_cast<int>(cell) << " became "
                << tileName(resolved.state.board[cell].color) << ", +1 score\n";
    }
    if (resolved.stats.exchanges == 0) {
      std::cout << "  no exchange was resolved\n";
    }
    narrateTileLines(placed, resolved.state, tiles);
  }

  state = resolved.state;
  NarratedTurn narrated;
  narrated.stats = resolved.stats;
  const int player = state.toMove;
  for (int batch = 0; batch < resolved.drawBatches; ++batch) {
    std::vector<int> batchTiles;
    for (int n = 0; n < 3; ++n) {
      const int tile = drawOneToHand(state, player, rng);
      if (tile < 0) {
        break;
      }
      batchTiles.push_back(tile);
      narrated.drawnTiles.push_back(tile);
    }
    if (!batchTiles.empty()) {
      ++narrated.stats.drawEvents;
    }
    if (verbose) {
      std::cout << "  draw batch " << (batch + 1) << ": ";
      if (batchTiles.empty()) {
        std::cout << "no tile drawn";
      } else {
        for (std::size_t i = 0; i < batchTiles.size(); ++i) {
          if (i > 0) {
            std::cout << ", ";
          }
          std::cout << tileName(batchTiles[i]);
        }
      }
      std::cout << "\n";
    }
  }
  if (verbose && resolved.drawBatches == 0) {
    std::cout << "  no tile draw event\n";
  }

  state.toMove = static_cast<uint8_t>(1 - state.toMove);
  (void)before;
  return narrated;
}

std::optional<int> parseInt(const std::string& text) {
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || value < std::numeric_limits<int>::min() ||
      value > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

std::optional<Move> parseHumanMove(const std::string& line, const GameState& state,
                                   std::string& error) {
  std::istringstream in(line);
  std::string colorToken;
  std::string cellToken;
  if (!(in >> colorToken >> cellToken)) {
    error = "Expected '<color> <cell>', for example: r 12.";
    return std::nullopt;
  }
  std::string extra;
  if (in >> extra) {
    error = "Too many tokens. Expected '<color> <cell>'.";
    return std::nullopt;
  }

  colorToken = lower(colorToken);
  int color = -1;
  if (colorToken == "r" || colorToken == "red") {
    color = R;
  } else if (colorToken == "b" || colorToken == "blue") {
    color = B;
  } else if (colorToken == "y" || colorToken == "yellow") {
    color = Y;
  } else {
    error = "Unknown color. Use r, b, or y.";
    return std::nullopt;
  }

  const std::optional<int> cell = parseInt(cellToken);
  if (!cell || *cell < 0 || *cell >= 25) {
    error = "Cell must be an integer from 0 to 24.";
    return std::nullopt;
  }

  const int player = state.toMove;
  if (state.stock[player][color] == 0) {
    error = "You have no remaining stock of that color.";
    return std::nullopt;
  }
  if (state.board[*cell].kind != Empty) {
    error = "That cell is not empty.";
    return std::nullopt;
  }
  return Move{static_cast<uint8_t>(color), static_cast<uint8_t>(*cell)};
}

Move firstLegalMove(const GameState& state) {
  const std::vector<Move> legal = legalPlacements(state);
  return legal.front();
}

Move selectAgentMove(const AgentSpec& agent, const GameState& state, Rng& rng) {
  switch (agent.kind) {
    case AgentKind::Random:
      return randomMove(state, rng);
    case AgentKind::Greedy:
      return greedyMove(state, rng);
    case AgentKind::ExpectimaxDepth:
      return expectimaxMove(state, agent.budget);
    case AgentKind::ExpectimaxTimed:
      return expectimaxMoveTimed(state, agent.budget);
  }
  return firstLegalMove(state);
}

Move selectAiMove(const PlayOptions& options, const GameState& state) {
  if (options.fixedDepth) {
    return expectimaxMove(state, options.aiDepth);
  }
  return expectimaxMoveTimed(state, options.aiMs);
}

void printUsage() {
  std::cout << "Usage:\n"
            << "  irotoiro_cli play [--human 0|1] [--ai-depth D | --ai-ms MS] [--seed S] [--no-color]\n"
            << "  irotoiro_cli match --a AGENT --b AGENT --games N [--seed S] [--no-color]\n"
            << "\nAGENT: random, greedy, expectimax:D, expectimax-ms:MS\n";
}

bool parseSeed(const std::string& value, uint32_t& seed) {
  const std::optional<int> parsed = parseInt(value);
  if (!parsed || *parsed < 0) {
    return false;
  }
  seed = static_cast<uint32_t>(*parsed);
  return true;
}

bool parsePlayArgs(int argc, char** argv, PlayOptions& options) {
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    auto needValue = [&](const char* name) -> std::optional<std::string> {
      if (i + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        return std::nullopt;
      }
      return std::string(argv[++i]);
    };

    if (arg == "--human") {
      const auto value = needValue("--human");
      if (!value) {
        return false;
      }
      const auto parsed = parseInt(*value);
      if (!parsed || (*parsed != 0 && *parsed != 1)) {
        std::cerr << "--human must be 0 or 1\n";
        return false;
      }
      options.human = *parsed;
    } else if (arg == "--ai-depth") {
      const auto value = needValue("--ai-depth");
      if (!value) {
        return false;
      }
      const auto parsed = parseInt(*value);
      if (!parsed || *parsed <= 0) {
        std::cerr << "--ai-depth must be positive\n";
        return false;
      }
      options.fixedDepth = true;
      options.aiDepth = *parsed;
    } else if (arg == "--ai-ms") {
      const auto value = needValue("--ai-ms");
      if (!value) {
        return false;
      }
      const auto parsed = parseInt(*value);
      if (!parsed || *parsed <= 0) {
        std::cerr << "--ai-ms must be positive\n";
        return false;
      }
      options.fixedDepth = false;
      options.aiMs = *parsed;
    } else if (arg == "--seed") {
      const auto value = needValue("--seed");
      if (!value || !parseSeed(*value, options.seed)) {
        std::cerr << "--seed must be a non-negative integer\n";
        return false;
      }
    } else if (arg == "--no-color") {
      options.color = false;
    } else {
      std::cerr << "Unknown play option: " << arg << "\n";
      return false;
    }
  }
  return true;
}

std::optional<AgentSpec> parseAgent(const std::string& text) {
  if (text == "random") {
    return AgentSpec{AgentKind::Random, 0, text};
  }
  if (text == "greedy") {
    return AgentSpec{AgentKind::Greedy, 0, text};
  }
  const std::string depthPrefix = "expectimax:";
  if (text.rfind(depthPrefix, 0) == 0) {
    const auto parsed = parseInt(text.substr(depthPrefix.size()));
    if (parsed && *parsed > 0) {
      return AgentSpec{AgentKind::ExpectimaxDepth, *parsed, text};
    }
    return std::nullopt;
  }
  const std::string timedPrefix = "expectimax-ms:";
  if (text.rfind(timedPrefix, 0) == 0) {
    const auto parsed = parseInt(text.substr(timedPrefix.size()));
    if (parsed && *parsed > 0) {
      return AgentSpec{AgentKind::ExpectimaxTimed, *parsed, text};
    }
    return std::nullopt;
  }
  return std::nullopt;
}

bool parseMatchArgs(int argc, char** argv, MatchOptions& options) {
  bool haveA = false;
  bool haveB = false;
  bool haveGames = false;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    auto needValue = [&](const char* name) -> std::optional<std::string> {
      if (i + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        return std::nullopt;
      }
      return std::string(argv[++i]);
    };

    if (arg == "--a") {
      const auto value = needValue("--a");
      const auto agent = value ? parseAgent(*value) : std::nullopt;
      if (!agent) {
        std::cerr << "--a must be random, greedy, expectimax:D, or expectimax-ms:MS\n";
        return false;
      }
      options.a = *agent;
      haveA = true;
    } else if (arg == "--b") {
      const auto value = needValue("--b");
      const auto agent = value ? parseAgent(*value) : std::nullopt;
      if (!agent) {
        std::cerr << "--b must be random, greedy, expectimax:D, or expectimax-ms:MS\n";
        return false;
      }
      options.b = *agent;
      haveB = true;
    } else if (arg == "--games") {
      const auto value = needValue("--games");
      const auto parsed = value ? parseInt(*value) : std::nullopt;
      if (!parsed || *parsed <= 0) {
        std::cerr << "--games must be positive\n";
        return false;
      }
      options.games = *parsed;
      haveGames = true;
    } else if (arg == "--seed") {
      const auto value = needValue("--seed");
      if (!value || !parseSeed(*value, options.seed)) {
        std::cerr << "--seed must be a non-negative integer\n";
        return false;
      }
    } else if (arg == "--no-color") {
      options.color = false;
    } else {
      std::cerr << "Unknown match option: " << arg << "\n";
      return false;
    }
  }

  if (!haveA || !haveB || !haveGames) {
    std::cerr << "match requires --a, --b, and --games\n";
    return false;
  }
  return true;
}

int runPlay(const PlayOptions& options) {
  Rng rng(options.seed);
  GameState state = initialState(rng);
  const int ai = 1 - options.human;

  std::cout << "Irotoiro play. Human is player " << options.human << ", AI is player " << ai
            << ". Seed=" << options.seed << "\n";
  if (options.fixedDepth) {
    std::cout << "AI: expectimax depth " << options.aiDepth << "\n";
  } else {
    std::cout << "AI: timed expectimax " << options.aiMs << " ms\n";
  }
  printHelp();

  bool stdinEnded = false;
  while (!isTerminal(state)) {
    renderBoard(state, options.color);
    Move move;
    if (state.toMove == options.human && !stdinEnded) {
      while (true) {
        std::cout << "player " << options.human << " move> " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) {
          stdinEnded = true;
          std::cout << "\nInput ended; auto-playing remaining human turns with greedy legal moves.\n";
          Rng autoRng(options.seed ^ 0xBADC0DEu);
          move = greedyMove(state, autoRng);
          break;
        }
        const std::string command = lower(line);
        if (command == "help") {
          printHelp();
          continue;
        }
        if (command == "board") {
          renderBoard(state, options.color);
          continue;
        }
        if (command == "quit") {
          std::cout << "Quit.\n";
          return EXIT_SUCCESS;
        }
        std::string error;
        const auto parsed = parseHumanMove(line, state, error);
        if (!parsed) {
          std::cout << "Illegal input: " << error << "\n";
          continue;
        }
        move = *parsed;
        break;
      }
    } else if (state.toMove == options.human) {
      Rng autoRng(options.seed ^ static_cast<uint32_t>(occupiedCount(state) * 977u + 17u));
      move = greedyMove(state, autoRng);
      std::cout << "Auto-human move: " << ohaShort(move.color) << " "
                << static_cast<int>(move.cell) << "\n";
    } else {
      const auto start = Clock::now();
      move = selectAiMove(options, state);
      const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      std::cout << "AI move: " << ohaShort(move.color) << " " << static_cast<int>(move.cell)
                << " (" << std::fixed << std::setprecision(2) << ms << " ms)\n";
    }
    applyTurnNarrated(state, move, rng, true);
  }

  renderBoard(state, options.color);
  std::cout << "Final score: player 0 = " << static_cast<int>(state.score[0])
            << ", player 1 = " << static_cast<int>(state.score[1]) << "\n";
  if (state.score[0] > state.score[1]) {
    std::cout << "Winner: player 0\n";
  } else if (state.score[1] > state.score[0]) {
    std::cout << "Winner: player 1\n";
  } else {
    std::cout << "Result: draw\n";
  }
  return EXIT_SUCCESS;
}

void mixChecksum(uint64_t& checksum, const GameState& state) {
  checksum = checksum * 1315423911ull + static_cast<uint64_t>(state.score[0]) * 17ull +
             static_cast<uint64_t>(state.score[1]) * 31ull +
             static_cast<uint64_t>(occupiedCount(state)) * 43ull +
             static_cast<uint64_t>(handCount(state, 0)) * 59ull +
             static_cast<uint64_t>(handCount(state, 1)) * 71ull;
}

GameState playOneMatchGame(uint32_t seed, const AgentSpec& p0, const AgentSpec& p1,
                           MatchStats& stats, bool aIsP0) {
  Rng rng(seed);
  GameState state = initialState(rng);
  while (!isTerminal(state)) {
    const int player = state.toMove;
    const bool agentA = (player == 0) == aIsP0;
    const AgentSpec& agent = agentA ? (aIsP0 ? p0 : p1) : (aIsP0 ? p1 : p0);
    const auto start = Clock::now();
    const Move move = selectAgentMove(agent, state, rng);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    const int idx = agentA ? 0 : 1;
    stats.agent[idx].ms += ms;
    ++stats.agent[idx].moves;
    applyTurn(state, move, rng);
    ++stats.plies;
  }
  return state;
}

int runMatch(const MatchOptions& options) {
  MatchStats stats;
  stats.games = options.games;
  const auto start = Clock::now();

  int played = 0;
  for (int pair = 0; played < options.games; ++pair) {
    const uint32_t seed = options.seed + static_cast<uint32_t>(pair * 1009u);
    {
      GameState state = playOneMatchGame(seed, options.a, options.b, stats, true);
      if (state.score[0] > state.score[1]) {
        ++stats.aWins;
      } else if (state.score[0] < state.score[1]) {
        ++stats.bWins;
      } else {
        ++stats.draws;
      }
      stats.aScore += state.score[0];
      stats.bScore += state.score[1];
      mixChecksum(stats.checksum, state);
      ++played;
    }
    if (played >= options.games) {
      break;
    }
    {
      GameState state = playOneMatchGame(seed, options.b, options.a, stats, false);
      if (state.score[1] > state.score[0]) {
        ++stats.aWins;
      } else if (state.score[1] < state.score[0]) {
        ++stats.bWins;
      } else {
        ++stats.draws;
      }
      stats.aScore += state.score[1];
      stats.bScore += state.score[0];
      mixChecksum(stats.checksum, state);
      ++played;
    }
  }

  stats.totalMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  const double games = static_cast<double>(stats.games);
  const double aWinPct = games > 0.0 ? 100.0 * static_cast<double>(stats.aWins) / games : 0.0;
  const double bWinPct = games > 0.0 ? 100.0 * static_cast<double>(stats.bWins) / games : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "Irotoiro match\n"
            << "  A: " << options.a.name << "\n"
            << "  B: " << options.b.name << "\n"
            << "  games: " << stats.games << " (" << (stats.games / 2)
            << " paired seed(s)";
  if (stats.games % 2 != 0) {
    std::cout << " + 1 unpaired game";
  }
  std::cout << ")\n"
            << "  seed: " << options.seed << "\n"
            << "  A wins: " << stats.aWins << " (" << aWinPct << "%)\n"
            << "  B wins: " << stats.bWins << " (" << bWinPct << "%)\n"
            << "  draws:  " << stats.draws << "\n"
            << "  avg score A: " << (stats.aScore / games) << "\n"
            << "  avg score B: " << (stats.bScore / games) << "\n";
  for (int idx = 0; idx < 2; ++idx) {
    const MoveTiming& timing = stats.agent[idx];
    const double msPerMove = timing.moves > 0 ? timing.ms / static_cast<double>(timing.moves) : 0.0;
    const double movesPerSec = timing.ms > 0.0 ? 1000.0 * static_cast<double>(timing.moves) / timing.ms
                                               : 0.0;
    std::cout << "  " << (idx == 0 ? "A" : "B") << " moves: " << timing.moves
              << ", ms/move: " << msPerMove << ", moves/s: " << movesPerSec << "\n";
  }
  std::cout << "  total plies: " << stats.plies << "\n"
            << "  total elapsed ms: " << stats.totalMs << "\n"
            << "  checksum: " << stats.checksum << "\n";
  return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printUsage();
    return EXIT_FAILURE;
  }

  const std::string command = argv[1];
  if (command == "play") {
    PlayOptions options;
    if (!parsePlayArgs(argc, argv, options)) {
      printUsage();
      return EXIT_FAILURE;
    }
    return runPlay(options);
  }
  if (command == "match") {
    MatchOptions options;
    if (!parseMatchArgs(argc, argv, options)) {
      printUsage();
      return EXIT_FAILURE;
    }
    return runMatch(options);
  }

  printUsage();
  return EXIT_FAILURE;
}
