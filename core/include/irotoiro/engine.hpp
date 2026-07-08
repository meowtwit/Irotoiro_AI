#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace irotoiro {

enum OhaColor : uint8_t { R = 0, B = 1, Y = 2 };
enum TileColor : uint8_t { P = 0, G = 1, O = 2 };
enum CellKind : uint8_t { Empty = 0, Ohajiki = 1, Tile = 2 };

struct Cell {
  CellKind kind = Empty;
  uint8_t color = 0;
};

struct Move {
  uint8_t color = 0;
  uint8_t cell = 0;
};

struct TurnStats {
  int exchanges = 0;
  int steals = 0;
  int drawEvents = 0;
};

struct GameState {
  std::array<Cell, 25> board{};
  std::array<std::array<uint8_t, 3>, 2> stock{};
  std::array<std::array<uint8_t, 3>, 2> hand{};
  std::array<uint8_t, 3> bag{};
  std::array<uint8_t, 2> score{};
  uint8_t toMove = 0;
};

struct Window {
  std::array<uint8_t, 3> cells{};
};

class Rng {
 public:
  explicit Rng(uint32_t seed) : a_(seed) {}

  double next();

 private:
  uint32_t a_;
};

int mixTile(int a, int b);
const std::array<Window, 48>& windows();
const std::array<std::vector<uint8_t>, 25>& windowsThroughCell();

int handCount(const GameState& state, int player);
int bagCount(const GameState& state);
int stockCount(const GameState& state, int player);
int occupiedCount(const GameState& state);
bool isTerminal(const GameState& state);

int drawOneToHand(GameState& state, int player, Rng& rng);
GameState initialState(Rng& rng);
std::vector<Move> legalPlacements(const GameState& state);
Move randomMove(const GameState& state, Rng& rng);
TurnStats applyTurn(GameState& state, Move move, Rng& rng);

std::string boardSignature(const GameState& state);
bool checkInvariants(const GameState& state, std::string* error = nullptr);

}  // namespace irotoiro
