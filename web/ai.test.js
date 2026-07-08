'use strict';

const assert = require('assert');
const I = require('./engine.js');
const AI = require('./ai.js');

const GOLDEN = {
  1: {plies:24, score:[1,6], hand:[[1,1,4],[4,1,1]], bag:[0,3,2], board:'rrPbPrGbbGPGbryr.yOyybbyr', exch:7, steal:1, draw:5},
  7: {plies:24, score:[4,4], hand:[[2,2,0],[0,0,4]], bag:[2,4,2], board:'yGybyrbPby.yPrObPGrPyryOr', exch:8, steal:1, draw:5},
  42: {plies:24, score:[1,7], hand:[[1,2,3],[3,2,0]], bag:[2,1,2], board:'rrObPyyOrryGbGrrGyOPbyrb.', exch:8, steal:1, draw:5},
  100: {plies:24, score:[0,12], hand:[[1,0,2],[2,1,3]], bag:[2,1,0], board:'PbGGGyyGGbO.GyPOrPyrrybOr', exch:12, steal:5, draw:7},
  2024: {plies:24, score:[6,6], hand:[[0,3,1],[3,0,3]], bag:[1,0,1], board:'rrPbbrOPOGrPOPGGGbyr.byGy', exch:12, steal:9, draw:6}
};

function deepEqualState(a, b) {
  assert.deepStrictEqual(a, b);
}

function replay(seed) {
  const rng = I.makeRng(seed);
  const s = I.initialState(rng);
  let plies = 0, exch = 0, steal = 0, draw = 0;
  while (!I.isTerminal(s)) {
    const mv = I.randomBotMove(s, rng);
    const ev = I.applyTurn(s, mv, rng);
    plies++;
    for (const e of ev) {
      if (e.type === 'exchange') exch++;
      else if (e.type === 'steal-point' || e.type === 'steal-tile') steal++;
      else if (e.type === 'draw') draw++;
    }
  }
  return {s, plies, exch, steal, draw};
}

function assertGoldenParity() {
  for (const seed of Object.keys(GOLDEN).map(Number)) {
    const got = replay(seed);
    const exp = GOLDEN[seed];
    assert.strictEqual(got.plies, exp.plies, `plies seed=${seed}`);
    assert.deepStrictEqual(got.s.score, exp.score, `score seed=${seed}`);
    assert.deepStrictEqual(got.s.hand, exp.hand, `hand seed=${seed}`);
    assert.deepStrictEqual(got.s.bag, exp.bag, `bag seed=${seed}`);
    assert.strictEqual(I.boardSignature(got.s), exp.board, `board seed=${seed}`);
    assert.strictEqual(got.exch, exp.exch, `exch seed=${seed}`);
    assert.strictEqual(got.steal, exp.steal, `steal seed=${seed}`);
    assert.strictEqual(got.draw, exp.draw, `draw seed=${seed}`);
  }
  console.log('golden parity PASS seeds=1,7,42,100,2024');
}

function tileTotal(s) {
  let boardTiles = 0, hand = 0, bag = 0;
  for (const c of s.board) if (c && c.kind === 'tile') boardTiles++;
  for (let p=0;p<2;p++) for (let t=0;t<3;t++) hand += s.hand[p][t];
  for (let t=0;t<3;t++) bag += s.bag[t];
  return boardTiles + hand + bag;
}

function assertChanceSum() {
  const s = {
    board:new Array(25).fill(null),
    stock:[[4,3,4],[4,4,4]],
    hand:[[1,1,0],[0,0,0]],
    bag:[2,2,1],
    score:[0,0],
    toMove:0
  };
  s.board[0] = {kind:'oha', color:0};
  s.board[1] = {kind:'oha', color:0};
  const resolved = AI.resolveTurnDeterministic(s, {color:0, cell:2});
  assert.strictEqual(resolved.drawBatches, 1);
  const outcomes = AI.enumerateDrawOutcomes(resolved.state, resolved.drawBatches);
  const sum = outcomes.reduce((acc, o) => acc + o.prob, 0);
  assert(Math.abs(sum - 1) <= 1e-9, `prob sum=${sum}`);
  const totalBefore = tileTotal(s);
  for (const o of outcomes) {
    assert.strictEqual(o.state.toMove, 1);
    assert.strictEqual(tileTotal(o.state), totalBefore);
  }
  console.log(`chance sum PASS outcomes=${outcomes.length} prob=${sum.toFixed(12)}`);
}

async function assertLegalityNoMutation() {
  const rng = I.makeRng(77);
  const s = I.initialState(rng);
  for (let i=0;i<5;i++) I.applyTurn(s, I.randomBotMove(s, rng), rng);
  const before = I.cloneState(s);
  const mv = await AI.bestMove(s, {depth:2});
  const legal = I.legalPlacements(s).some(m => m.cell === mv.cell && m.color === mv.color);
  assert(legal, `illegal move ${JSON.stringify(mv)}`);
  deepEqualState(s, before);
  console.log(`legality/no-mutation PASS move=${JSON.stringify(mv)}`);
}

function chooseRandom(s, rng) {
  return I.randomBotMove(s, rng);
}

async function chooseDepth(s, depth) {
  return AI.bestMove(s, {depth});
}

function finalResult(s, player) {
  return s.score[player] > s.score[1-player] ? 1 : s.score[player] < s.score[1-player] ? -1 : 0;
}

async function playGame(seed, seat, botKind) {
  const rng = I.makeRng(seed);
  const s = I.initialState(rng);
  while (!I.isTerminal(s)) {
    let mv;
    if (s.toMove === seat) mv = await chooseDepth(s, 2);
    else if (botKind === 'random') mv = chooseRandom(s, rng);
    else mv = await chooseDepth(s, 1);
    I.applyTurn(s, mv, rng);
  }
  return finalResult(s, seat);
}

async function strength() {
  const N = 20;
  let randomWins = 0, randomLosses = 0, randomDraws = 0;
  let greedyWins = 0, greedyLosses = 0, greedyDraws = 0;
  for (let i=1;i<=N/2;i++) {
    for (const seat of [0,1]) {
      const r = await playGame(10000 + i, seat, 'random');
      if (r > 0) randomWins++; else if (r < 0) randomLosses++; else randomDraws++;
      const g = await playGame(20000 + i, seat, 'greedy');
      if (g > 0) greedyWins++; else if (g < 0) greedyLosses++; else greedyDraws++;
    }
  }
  const randomRate = randomWins / N;
  assert(randomRate > 0.90, `depth2 vs random win rate too low: ${randomRate}`);
  assert(greedyWins > greedyLosses, `depth2 vs greedy wins=${greedyWins} losses=${greedyLosses}`);
  console.log(`strength PASS depth2-vs-random wins=${randomWins}/${N} losses=${randomLosses} draws=${randomDraws} winRate=${(randomRate*100).toFixed(1)}%`);
  console.log(`strength PASS depth2-vs-greedy wins=${greedyWins}/${N} losses=${greedyLosses} draws=${greedyDraws}`);
}

async function latency(level, samples) {
  const rng = I.makeRng(9000 + level);
  const s = I.initialState(rng);
  for (let i=0;i<8;i++) I.applyTurn(s, I.randomBotMove(s, rng), rng);
  let total = 0;
  for (let i=0;i<samples;i++) {
    const test = I.cloneState(s);
    const t0 = Date.now();
    const mv = await AI.bestMove(test, {level});
    total += Date.now() - t0;
    assert(I.legalPlacements(test).some(m => m.cell === mv.cell && m.color === mv.color));
  }
  console.log(`latency level=${level} avgMoveMs=${(total/samples).toFixed(1)} samples=${samples}`);
}

(async function main() {
  assertGoldenParity();
  assertChanceSum();
  await assertLegalityNoMutation();
  await strength();
  await latency(5, 5);
  await latency(8, 3);
})().catch(err => {
  console.error(err && err.stack || err);
  process.exitCode = 1;
});
