# 移植オラクル（C++ ⇄ JS 参照実装の一致検証）

C++ エンジンが JS 参照実装（`web/irotoiro.html` 内の `Irotoiro`、2万局検証済み）と**ビット単位で同一の挙動**になることを、コードで検証するための仕様とゴールデン値。
ルールの正は `SPEC.md`、実装方針は `ENGINE.md`。本書は「乱数と着手順を固定した決定的リプレイ」で一致を担保する。

これらの値に一致すれば、C++ エンジンは参照実装と等価とみなす。

---

## 1. 乱数（mulberry32）— 完全一致が必須

JS 参照と同一の PRNG・同一の呼び出し順であること。

```js
function makeRng(seed){ let a = seed >>> 0; return function(){
  a |= 0; a = (a + 0x6D2B79F5) | 0;
  let t = Math.imul(a ^ (a >>> 15), 1 | a);
  t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
}; }
```

C++ 実装（uint32 の折り返し演算・`Math.imul` は 32bit 乗算下位32bit）:

```cpp
struct Rng { uint32_t a;
  explicit Rng(uint32_t seed):a(seed){}
  double next(){
    a = a + 0x6D2B79F5u;
    uint32_t t = (a ^ (a >> 15)) * (1u | a);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (double)(t ^ (t >> 14)) / 4294967296.0;   // [0,1)
  }
};
```

**乱数の消費順（決定的リプレイの要）**
1. `initialState`: プレイヤー0へ2枚 → プレイヤー1へ2枚、の順に `drawOneToHand` を4回。
2. 以降ループ: `randomBotMove`（着手1回につき `next()` を1回）→ `applyTurn`（AAA/ABC のドローで `drawOneToHand` を消費）。

`drawOneToHand(state,p)`（袋から1枚, 上限6・袋空を尊重）:
```
if handCount(p) >= 6: return             // 乱数を消費しない
total = bag[P]+bag[G]+bag[O]; if total==0: return   // 乱数を消費しない
r = floor(next() * total)                // 乱数を1回消費
色を t=P,G,O の順に累積比較: r<bag[t] なら bag[t]--, hand[p][t]++
```
※ 上限到達・袋空のときは `next()` を**呼ばない**こと（呼ぶとストリームがずれる）。

---

## 2. 合法手の順序（randomBotMove の index 選択に影響）

`legalPlacements(state)` は次の順で列挙する（この順序で index が決まるため一致必須）:
```
for cell in 0..24:            // セル昇順
  if board[cell] != empty: continue
  for color in R(0),B(1),Y(2):   // 色昇順
    if stock[toMove][color] > 0: push({color, cell})
```
`randomBotMove`: `idx = floor(next() * moves.length)`、`moves[idx]` を返す。

---

## 3. 盤面シグネチャ

25文字。各セル: 空=`.`、おはじき=`r`/`b`/`y`、タイル=`P`/`G`/`O`（index 0..24 = row*5+col）。

---

## 4. 正準ドライバ（両者ランダム・同一ストリーム）

```
rng = makeRng(seed)
s = initialState(rng)
while not terminal(s):
    mv = randomBotMove(s, rng)
    applyTurn(s, mv, rng)
```

---

## 5. ゴールデン値（per-seed 最終状態）— 完全一致必須

各行: `seed / plies / score[2] / hand[2][P,G,O] / bag[P,G,O] / board(25) / exch / steal / draw`

```
seed=1    plies=24 score=[1,6]  hand=[[1,1,4],[4,1,1]] bag=[0,3,2] board="rrPbPrGbbGPGbryr.yOyybbyr" exch=7  steal=1 draw=5
seed=7    plies=24 score=[4,4]  hand=[[2,2,0],[0,0,4]] bag=[2,4,2] board="yGybyrbPby.yPrObPGrPyryOr" exch=8  steal=1 draw=5
seed=42   plies=24 score=[1,7]  hand=[[1,2,3],[3,2,0]] bag=[2,1,2] board="rrObPyyOrryGbGrrGyOPbyrb." exch=8  steal=1 draw=5
seed=100  plies=24 score=[0,12] hand=[[1,0,2],[2,1,3]] bag=[2,1,0] board="PbGGGyyGGbO.GyPOrPyrrybOr" exch=12 steal=5 draw=7
seed=2024 plies=24 score=[6,6]  hand=[[0,3,1],[3,0,3]] bag=[1,0,1] board="rrPbbrOPOGrPOPGGGbyr.byGy" exch=12 steal=9 draw=6
```

## 6. ゴールデン値（集計パリティ, seeds 1..1000）— 完全一致必須

正準ドライバを seed=1..1000 で実行した集計:
```
games            = 1000
totalPlies       = 24000     // 全局きっかり24手
totalExchanges   = 9229
totalSteals      = 2583      // steal-point + steal-tile の合計
totalDrawEvents  = 6254
scoreHash        = 2950933045
nonTerminal      = 0
boards24occupied = 1000      // 終局盤面は必ず24マス占有・1マス空き
```
`scoreHash` の算出（seed 昇順に反映, uint32 折り返し, 初期値0）:
```
h = 0
for seed in 1..1000: h = (uint32)( (uint64)h*31 + score0*7 + score1 )
```

---

## 7. 決定的ユニット（乱数を使わない）不変条件

任意の合法手列の各手番後で常に成立（`ENGINE.md §8` / `SPEC.md §10`）:
- 盤上おはじき数 + 両stock合計 + 両score合計 = 24
- 盤上タイル数 + 両hand合計 + 袋合計 = 24
- 各 hand 合計 ≤ 6, score ≥ 0
