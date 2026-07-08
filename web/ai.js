(function (root, factory) {
  'use strict';
  if (typeof module === 'object' && module.exports) module.exports = factory(require('./engine.js'));
  else root.IrotoiroAI = factory(root.Irotoiro);
})(typeof globalThis !== 'undefined' ? globalThis : (typeof window !== 'undefined' ? window : this), function (I) {
  'use strict';
  if (!I) throw new Error('Irotoiro engine is required before IrotoiroAI');

  const VALUE_MIN = -2000000;
  const VALUE_MAX = 2000000;
  const LEVEL_TIME = {3:100,4:180,5:280,6:420,7:600,8:850,9:1200,10:1700};

  const now = () => (typeof performance !== 'undefined' && performance.now ? performance.now() : Date.now());
  const yieldTurn = () => new Promise(resolve => {
    if (typeof setTimeout !== 'undefined') setTimeout(resolve, 0);
    else resolve();
  });
  const other = p => 1 - p;
  const comesBefore = (a,b) => a.cell !== b.cell ? a.cell < b.cell : a.color < b.color;
  const scoreDiff = (s,p) => s.score[p] - s.score[1-p];

  function cellKey(c) {
    if (c === null) return '.';
    return c.kind === 'oha' ? 'rby'[c.color] : 'PGO'[c.color];
  }

  function stateKey(s) {
    let key = '';
    for (let i=0;i<25;i++) key += cellKey(s.board[i]);
    key += '|s' + s.stock[0].join(',') + '/' + s.stock[1].join(',');
    key += '|h' + s.hand[0].join(',') + '/' + s.hand[1].join(',');
    key += '|b' + s.bag.join(',') + '|c' + s.score.join(',') + '|m' + s.toMove;
    return key;
  }

  function isAllKindOrEmpty(s, w, kind) {
    let kindCount = 0, emptyCount = 0;
    for (const cell of w) {
      const c = s.board[cell];
      if (c && c.kind === kind) kindCount++;
      else if (c === null) emptyCount++;
      else return null;
    }
    return {kindCount, emptyCount};
  }

  function potentialAabBonus(s, player) {
    let bonus = 0;
    for (const w of I.WINDOWS) {
      const counts = isAllKindOrEmpty(s, w, 'oha');
      if (!counts || counts.kindCount !== 2 || counts.emptyCount !== 1) continue;
      const colors = [0,0,0];
      let emptyCell = -1;
      for (const cell of w) {
        const c = s.board[cell];
        if (c && c.kind === 'oha') colors[c.color]++;
        else emptyCell = cell;
      }
      for (let placedColor=0; placedColor<3; placedColor++) {
        if (s.stock[player][placedColor] === 0) continue;
        const after = colors.slice();
        after[placedColor]++;
        let distinct = 0, majority = -1, minority = -1;
        for (let c=0;c<3;c++) {
          if (after[c] > 0) distinct++;
          if (after[c] === 2) majority = c;
          else if (after[c] === 1) minority = c;
        }
        if (distinct === 2 && majority >= 0 && minority >= 0) {
          const tile = I.mixTile(majority, minority);
          if (s.hand[player][tile] > 0 && emptyCell >= 0) {
            bonus += 8;
            if (placedColor === minority) bonus += 2;
          }
        }
      }
    }
    return bonus;
  }

  function tileReachBonus(s, player) {
    let bonus = 0;
    const opponent = other(player);
    for (const w of I.WINDOWS) {
      const counts = isAllKindOrEmpty(s, w, 'tile');
      if (!counts || counts.kindCount !== 2 || counts.emptyCount !== 1) continue;
      if (s.score[opponent] > 0) bonus += 6;
      else if (I.handCount(s, opponent) > 0 && I.handCount(s, player) < 6) bonus += 3;
      else bonus += 1;
    }
    return bonus;
  }

  function handUtility(s, player) {
    let value = 2 * I.handCount(s, player);
    for (let t=0;t<3;t++) if (s.hand[player][t] > 0) value++;
    return value;
  }

  function evaluate(s, player) {
    const opponent = other(player);
    let score = 100 * (s.score[player] - s.score[opponent]);
    score += potentialAabBonus(s, player);
    score -= Math.trunc(potentialAabBonus(s, opponent) / 2);
    score += tileReachBonus(s, player);
    score -= Math.trunc(tileReachBonus(s, opponent) / 2);
    score += handUtility(s, player);
    score -= Math.trunc(handUtility(s, opponent) / 2);
    return score;
  }

  function resolveTurnDeterministic(state, move) {
    const next = I.cloneState(state);
    const p = state.toMove, opp = 1 - p;
    const stats = {exchanges:0, steals:0, drawEvents:0};
    next.board[move.cell] = {kind:'oha', color:move.color};
    next.stock[p][move.color]--;

    const aabWindows = [];
    let drawBatches = 0;
    for (const wi of I.WINDOWS_THROUGH[move.cell]) {
      const w = I.WINDOWS[wi];
      const a = next.board[w[0]], b = next.board[w[1]], c = next.board[w[2]];
      if (!a || !b || !c || a.kind !== 'oha' || b.kind !== 'oha' || c.kind !== 'oha') continue;
      const cnt = [0,0,0];
      cnt[a.color]++; cnt[b.color]++; cnt[c.color]++;
      const distinct = cnt.filter(x => x > 0).length;
      if (distinct === 1 || distinct === 3) {
        drawBatches++;
        continue;
      }
      let minority = -1, majority = -1;
      for (let color=0;color<3;color++) {
        if (cnt[color] === 1) minority = color;
        else if (cnt[color] === 2) majority = color;
      }
      const minorityCell = w.find(cell => next.board[cell].color === minority);
      aabWindows.push({minorityCell, mix:I.mixTile(majority, minority)});
    }

    const consumed = new Set();
    const placedTileCells = [];
    for (const ab of aabWindows) {
      if (consumed.has(ab.minorityCell)) continue;
      if (next.hand[p][ab.mix] === 0) continue;
      next.hand[p][ab.mix]--;
      next.score[p]++;
      next.board[ab.minorityCell] = {kind:'tile', color:ab.mix};
      consumed.add(ab.minorityCell);
      placedTileCells.push(ab.minorityCell);
      stats.exchanges++;
    }

    const seen = new Set();
    for (const pc of placedTileCells) {
      for (const wi of I.WINDOWS_THROUGH[pc]) {
        if (seen.has(wi)) continue;
        const w = I.WINDOWS[wi];
        const a = next.board[w[0]], b = next.board[w[1]], c = next.board[w[2]];
        if (!a || !b || !c || a.kind !== 'tile' || b.kind !== 'tile' || c.kind !== 'tile') continue;
        seen.add(wi);
        if (next.score[opp] > 0) {
          next.score[opp]--;
          next.score[p]++;
          stats.steals++;
        } else if (I.handCount(next, p) < 6 && I.handCount(next, opp) > 0) {
          let tile = -1, best = -1;
          for (let k=0;k<3;k++) if (next.hand[opp][k] > best) { best = next.hand[opp][k]; tile = k; }
          if (tile >= 0 && next.hand[opp][tile] > 0) {
            next.hand[opp][tile]--;
            next.hand[p][tile]++;
            stats.steals++;
          }
        }
      }
    }

    return {state:next, drawBatches, stats};
  }

  function enumerateDrawOutcomes(preDrawState, drawBatches) {
    const merged = new Map();
    function addOutcome(s, prob) {
      const done = I.cloneState(s);
      done.toMove = 1 - done.toMove;
      const key = stateKey(done);
      const hit = merged.get(key);
      if (hit) hit.prob += prob;
      else merged.set(key, {prob, state:done});
    }
    function enumerateBatch(s, batch, prob) {
      if (batch >= drawBatches) {
        addOutcome(s, prob);
        return;
      }
      enumerateDrawInBatch(s, batch, 0, prob);
    }
    function enumerateDrawInBatch(s, batch, drawInBatch, prob) {
      if (drawInBatch >= 3 || I.handCount(s, s.toMove) >= 6 || I.bagCount(s) === 0) {
        enumerateBatch(s, batch + 1, prob);
        return;
      }
      const p = s.toMove;
      const total = I.bagCount(s);
      for (let tile=0;tile<3;tile++) {
        if (s.bag[tile] === 0) continue;
        const next = I.cloneState(s);
        const chance = next.bag[tile] / total;
        next.bag[tile]--;
        next.hand[p][tile]++;
        enumerateDrawInBatch(next, batch, drawInBatch + 1, prob * chance);
      }
    }
    enumerateBatch(preDrawState, 0, 1);
    return Array.from(merged.values());
  }

  function terminalValue(s, maximizingPlayer) {
    const diff = scoreDiff(s, maximizingPlayer);
    if (diff > 0) return 1000000 + diff * 1000;
    if (diff < 0) return -1000000 + diff * 1000;
    return 0;
  }

  function orderedMoves(s, maximizingNode) {
    const player = s.toMove;
    const before = scoreDiff(s, player);
    return I.legalPlacements(s).map(move => {
      const resolved = resolveTurnDeterministic(s, move);
      return {move, resolved, immediateScore:scoreDiff(resolved.state, player) - before};
    }).sort((a,b) => {
      if (a.immediateScore !== b.immediateScore) return maximizingNode ? b.immediateScore - a.immediateScore : a.immediateScore - b.immediateScore;
      return comesBefore(a.move,b.move) ? -1 : comesBefore(b.move,a.move) ? 1 : 0;
    });
  }

  class AbortSearch extends Error {}

  function makeContext(maximizingPlayer, deadline) {
    return {
      maximizingPlayer,
      deadline: deadline == null ? null : deadline,
      nodes: 0,
      chanceOutcomes: 0,
      check() {
        this.nodes++;
        if (this.deadline != null && (this.nodes & 255) === 0 && now() >= this.deadline) throw new AbortSearch();
      }
    };
  }

  function chanceValue(resolved, depth, ctx) {
    if (resolved.drawBatches === 0) {
      const next = I.cloneState(resolved.state);
      next.toMove = 1 - next.toMove;
      return expectiminimax(next, depth - 1, VALUE_MIN, VALUE_MAX, ctx);
    }
    const outcomes = enumerateDrawOutcomes(resolved.state, resolved.drawBatches);
    ctx.chanceOutcomes += outcomes.length;
    let value = 0;
    for (const outcome of outcomes) {
      value += outcome.prob * expectiminimax(outcome.state, depth - 1, VALUE_MIN, VALUE_MAX, ctx);
    }
    return value;
  }

  function expectiminimax(s, depth, alpha, beta, ctx) {
    ctx.check();
    if (I.isTerminal(s)) return terminalValue(s, ctx.maximizingPlayer);
    if (depth <= 0) return evaluate(s, ctx.maximizingPlayer);

    const maximizingNode = s.toMove === ctx.maximizingPlayer;
    const moves = orderedMoves(s, maximizingNode);
    if (maximizingNode) {
      let best = VALUE_MIN;
      for (const ordered of moves) {
        const value = chanceValue(ordered.resolved, depth, ctx);
        if (value > best) best = value;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
      }
      return best;
    }
    let best = VALUE_MAX;
    for (const ordered of moves) {
      const value = chanceValue(ordered.resolved, depth, ctx);
      if (value < best) best = value;
      if (best < beta) beta = best;
      if (alpha >= beta) break;
    }
    return best;
  }

  async function searchRootDepth(state, depth, deadline) {
    const ctx = makeContext(state.toMove, deadline);
    let moves = orderedMoves(state, true);
    if (!moves.length) return {move:null, value:0, completed:true, stats:ctx};
    let best = moves[0].move;
    let bestValue = VALUE_MIN;
    let haveBest = false;
    let alpha = VALUE_MIN;
    const beta = VALUE_MAX;
    for (const ordered of moves) {
      if (deadline != null && now() >= deadline) throw new AbortSearch();
      const value = chanceValue(ordered.resolved, depth, ctx);
      if (!haveBest || value > bestValue || (Math.abs(value - bestValue) <= 1e-12 && comesBefore(ordered.move, best))) {
        bestValue = value;
        best = ordered.move;
        haveBest = true;
      }
      if (bestValue > alpha) alpha = bestValue;
      await yieldTurn();
    }
    return {move:best, value:bestValue, completed:haveBest, stats:ctx};
  }

  // position value from a FIXED perspective (root is MAX if perspective to move, else MIN)
  async function rootValueDepth(state, depth, deadline, perspective, shouldAbort) {
    const ctx = makeContext(perspective, deadline);
    const maximizingNode = state.toMove === perspective;
    const moves = orderedMoves(state, maximizingNode);
    if (!moves.length) return {value: terminalValue(state, perspective), completed:true, nodes:0};
    let best = maximizingNode ? VALUE_MIN : VALUE_MAX;
    let alpha = VALUE_MIN, beta = VALUE_MAX, have = false;
    for (const ordered of moves) {
      if ((deadline != null && now() >= deadline) || (shouldAbort && shouldAbort())) throw new AbortSearch();
      const value = chanceValue(ordered.resolved, depth, ctx);
      if (maximizingNode) { if (!have || value > best) best = value; if (best > alpha) alpha = best; }
      else { if (!have || value < best) best = value; if (best < beta) beta = best; }
      have = true;
      if (alpha >= beta) break;
      await yieldTurn();
    }
    return {value: best, completed: have, nodes: ctx.nodes};
  }

  // Evaluate the current position by iterative-deepening search (stable, deep) from `perspective`.
  async function analyze(state, opts) {
    opts = opts || {};
    const s = I.cloneState(state);
    const perspective = opts.perspective == null ? s.toMove : opts.perspective;
    const timeMs = Math.max(1, opts.timeMs == null ? 1000 : opts.timeMs);
    if (I.isTerminal(s)) return {value: terminalValue(s, perspective), depth:0, nodes:0, nps:0, ms:0, perspective};
    const startedAt = now();
    const deadline = startedAt + timeMs;
    let value = evaluate(s, perspective), reached = 0, totalNodes = 0;
    const shouldAbort = opts.shouldAbort || null;
    const maxDepth = Math.max(1, I.stockCount(s,0) + I.stockCount(s,1));
    for (let depth=1; depth<=maxDepth; depth++) {
      if (now() >= deadline || (shouldAbort && shouldAbort())) break;
      try {
        const r = await rootValueDepth(s, depth, deadline, perspective, shouldAbort);
        totalNodes += r.nodes;
        if (r.completed) { value = r.value; reached = depth; }
      } catch (err) {
        if (err instanceof AbortSearch) break;
        throw err;
      }
      await yieldTurn();
    }
    const ms = now() - startedAt;
    return {value, depth:reached, nodes:totalNodes, nps: totalNodes/Math.max(0.001, ms/1000), ms, perspective};
  }

  function randomMove(state, rngOrMathRandom) {
    const rng = rngOrMathRandom || Math.random;
    const moves = I.legalPlacements(state);
    if (!moves.length) return null;
    const r = typeof rng === 'function' ? rng() : rng.random();
    return moves[Math.floor(r * moves.length)];
  }

  function greedyMove(state) {
    const moves = I.legalPlacements(state);
    if (!moves.length) return null;
    const player = state.toMove;
    let best = moves[0], bestScore = -1000000000;
    for (const move of moves) {
      const resolved = resolveTurnDeterministic(state, move);
      let next;
      if (resolved.drawBatches === 0) {
        next = I.cloneState(resolved.state);
        next.toMove = 1 - next.toMove;
      } else {
        const outcomes = enumerateDrawOutcomes(resolved.state, resolved.drawBatches);
        let score = 0;
        for (const outcome of outcomes) score += outcome.prob * evaluate(outcome.state, player);
        if (score > bestScore || (Math.abs(score - bestScore) <= 1e-12 && comesBefore(move, best))) {
          bestScore = score;
          best = move;
        }
        continue;
      }
      const score = evaluate(next, player);
      if (score > bestScore || (score === bestScore && comesBefore(move, best))) {
        bestScore = score;
        best = move;
      }
    }
    return best;
  }

  let lastStats = null;

  async function bestMove(state, opts) {
    const options = opts || {level:4};
    const onProgress = typeof options.onProgress === 'function' ? options.onProgress : null;
    const stateCopy = I.cloneState(state);
    const legal = I.legalPlacements(stateCopy);
    if (!legal.length) { lastStats = null; return null; }
    const startedAt = now();
    const npsOf = (nodes, ms) => nodes / Math.max(0.001, ms/1000);

    if (options.level === 1) {
      const mv = randomMove(stateCopy, Math.random);
      lastStats = {mode:'random', depth:0, nodes:0, nps:0, ms:now()-startedAt, value:evaluate(stateCopy, stateCopy.toMove)};
      if (onProgress) onProgress(lastStats);
      return mv;
    }
    if (options.level === 2) {
      const mv = greedyMove(stateCopy);
      const ms = now()-startedAt;
      lastStats = {mode:'greedy', depth:1, nodes:legal.length, nps:npsOf(legal.length, ms), ms, value:evaluate(stateCopy, stateCopy.toMove)};
      if (onProgress) onProgress(lastStats);
      return mv;
    }
    if (options.depth != null) {
      const d = Math.max(1, options.depth);
      const result = await searchRootDepth(stateCopy, d, null);
      const ms = now()-startedAt;
      lastStats = {mode:'depth', depth:d, nodes:result.stats.nodes, nps:npsOf(result.stats.nodes, ms), ms, value:result.value};
      if (onProgress) onProgress(lastStats);
      return result.move ? {color:result.move.color, cell:result.move.cell} : legal[0];
    }

    let timeMs;
    if (options.timeMs != null) timeMs = Math.max(1, options.timeMs);
    else {
      const level = options.level == null ? 4 : options.level;
      timeMs = LEVEL_TIME[level] || LEVEL_TIME[4];
    }

    const deadline = now() + timeMs;
    let best = greedyMove(stateCopy) || legal[0];
    let bestValue = evaluate(stateCopy, stateCopy.toMove);
    let totalNodes = 0, reachedDepth = 0;
    const maxDepth = Math.max(1, I.stockCount(stateCopy,0) + I.stockCount(stateCopy,1));
    for (let depth=1; depth<=maxDepth; depth++) {
      if (now() >= deadline) break;
      try {
        const result = await searchRootDepth(stateCopy, depth, deadline);
        totalNodes += result.stats.nodes;
        if (result.completed && result.move) { best = result.move; bestValue = result.value; reachedDepth = depth; }
        const ms = now()-startedAt;
        const info = {mode:'expectimax', depth, depthNodes:result.stats.nodes, nodes:totalNodes, nps:npsOf(totalNodes, ms), ms, value:result.value, completed:result.completed};
        lastStats = info;
        if (onProgress && result.completed) onProgress(info);
      } catch (err) {
        if (err instanceof AbortSearch) break;
        throw err;
      }
      await yieldTurn();
    }
    const finalMs = now()-startedAt;
    lastStats = {mode:'expectimax', depth:reachedDepth, nodes:totalNodes, nps:npsOf(totalNodes, finalMs), ms:finalMs, value:bestValue, completed:true};
    return {color:best.color, cell:best.cell};
  }

  function lastSearchStats(){ return lastStats; }

  return {evaluate, resolveTurnDeterministic, enumerateDrawOutcomes, bestMove, randomMove, greedyMove, lastSearchStats, analyze};
});
