#pragma once

#include "irotoiro/engine.hpp"

namespace irotoiro {

// Lightweight static evaluation from the perspective of player.
int evaluate(const GameState& state, int player);

// One-ply greedy Lv1 bot. Candidate draws are evaluated on cloned RNG streams,
// so choosing a move does not advance rng unless future tie-breaks use it.
Move greedyMove(const GameState& state, Rng& rng);

}  // namespace irotoiro
