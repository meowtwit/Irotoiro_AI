# イロトイロ — コアエンジン＋簡単Bot＋速度ベンチ 設計書 v1.0

対象フェーズ：フェーズ1（エンジン）＋フェーズ3の入口（簡単Bot）＋探索速度の実測。
ルールの正は `SPEC.md`。本書はその**実装方針**を定める。言語は **C++（C++20）**、実装は codex に委譲。

---

## 1. スコープ（この設計書で作るもの）

1. **ルールエンジン**：状態表現・合法手生成・イベント解決（`SPEC.md §7`順）・終局判定。CLIで1ゲーム完走できる。
2. **簡単Bot**：貪欲（Lv1）＋任意で深さ制限Expectimax（Lv2）。
3. **ベンチマーク**：`perft`（生成/適用スループット）、ランダム対局スループット、Expectimaxの到達深さ計測。
4. **テスト**：不変条件＋`SPEC.md §10`のエッジケース。

GUI/WASM/学習系はスコープ外（本マイルストーンでは native only）。

---

## 2. リポジトリ構成（最小）

```
/core
  /include/irotoiro/    # 公開ヘッダ（state.hpp, move.hpp, engine.hpp, eval.hpp, bot.hpp）
  /src/                 # 実装
  /bench/               # perft・playout・expectimax ベンチ実行体
  /tests/               # 単体テスト（doctest or GoogleTest）
  /cli/                 # テキスト対戦・自己対戦ドライバ
CMakeLists.txt          # native ターゲット（後で emcmake で WASM 追加）
```

---

## 3. データ構造（ビットボード）

25セル＝`uint32_t`の下位25bit（`idx = row*5 + col`）で表現。内容は色別マスクで持つ：

```cpp
struct Board {
  uint32_t oha[3];   // おはじき: [R,B,Y] それぞれの占有マスク
  uint32_t tile[3];  // タイル:   [P,G,O] それぞれの占有マスク
  // 派生: occupied = oha[0|1|2] | tile[0|1|2] ; empty = MASK25 & ~occupied
};

struct GameState {
  Board board;
  uint8_t stock[2][3];   // 手持ちおはじき [player][R,B,Y]  初期 {4,4,4}
  uint8_t hand[2][3];    // 手札タイル   [player][P,G,O]   合計 ≤ 6
  uint8_t bag[3];        // 袋残 [P,G,O]  初期 = 8*3 − 配布分
  uint8_t score[2];      // スコアボード上のおはじき枚数
  uint8_t toMove;        // 0 or 1
};
```

**並び判定の前計算**：5×5上の「長さ3の連続ウィンドウ」は全 **48個**（横15・縦15・＼9・／9）。各ウィンドウを `{idx0, idx1, idx2}` と 3bitマスクで保持。さらに **セル→そのセルを含むウィンドウ一覧** を前計算（`windowsThroughCell[25]`）。設置セル `k` のイベント判定は `windowsThroughCell[k]` のみ走査（`SPEC.md §3`：置いた駒を含むウィンドウのみ・重複なく各1回）。

色定数：`enum Oha{R,B,Y}; enum Tile{P,G,O};` 混色 `mix(a,b)`：R+B=P, B+Y=G, R+Y=O（テーブルで）。

---

## 4. Move表現と合法手生成

1手番＝「設置」＋その結果の「プレイヤー選択（あれば）」。探索の分岐点を明確化するため、**完全に解決しきった1手番**を1つの `Move` として列挙する。

```cpp
struct Move {
  uint8_t color;         // 設置するおはじき色 (R/B/Y)
  uint8_t cell;          // 設置セル 0..24（空きに限る）
  // 解決時のプレイヤー選択（SPEC §5, §6）。多くの手番で空。
  ResolutionChoices choices; // 実行するAABの選択、同一セル競合の選択、§6で奪うタイルの選択 等
};
```

- **基本列挙**：`(color with stock>0) × (empty cell)`。序盤最大75、以降減少。
- **解決選択**：設置後に生じる選択肢（実行可能AABの部分集合、§5競合の択一、§6の奪取対象）を展開して別Moveにする。ほとんどの局面で選択は0〜2個。
- **袋抽選（CHANCE）は Move に含めない**：`applyMove` が確率分岐で返す（§6）。

`legalMoves(state) -> vector<Move>`。

> 実装メモ：簡単Botや素朴なperftでは、解決選択を「常に得点最大の解決」に固定する簡易版 `legalPlacements()`（=色×空きマスのみ、解決はエンジンが貪欲確定）も用意すると速度計測が素直。探索用の完全版 `legalMoves()` と使い分ける。

---

## 5. イベント解決（`SPEC.md §7` の正準順序を実装）

`applyMove` の内部手順：

```
1. 設置：board に color を置き、stock を減らす。
2. windowsThroughCell[cell] を走査し、おはじきウィンドウを分類：
     AAB リスト / (AAA|ABC) リスト。
3. 【得点フェーズ】AAB を choices に従い解決：
     少数色おはじき除去→score[toMove]++、跡地に mix タイル設置（hand から消費）。
     §5競合（同一セル・同一タイル1枚）は choices で択一。
4. 【タイル並びフェーズ】3で新設置したタイルを含むタイルウィンドウを検出し §6 解決：
     相手score>0 → score移動 (−1/+1) ; ==0 → 相手 hand から1枚（choicesで指定, 上限6超過は不可）。
5. 【タイル獲得フェーズ】(AAA|ABC) の袋抽選：
     → ここだけ確率分岐。返り値は vector<pair<double prob, GameState>>。
        袋残<必要数なら引ける分だけ / 上限6で打ち切り。
6. toMove 反転。終局判定。
```

API：

```cpp
struct Outcome { double prob; GameState next; };
std::vector<Outcome> applyMove(const GameState&, const Move&); // 抽選なしなら要素1（prob=1）
bool  isTerminal(const GameState&);   // 両者 stock 全0
int   scoreDiff(const GameState&, int player); // = score[player]-score[other]
```

**副作用なしの純関数**（探索でコピーして使う）。1状態は約 40〜48 byte 目標（コピー安価）。

---

## 6. 簡単Bot

**Lv1：貪欲（既定）**
- 各合法設置を1手だけ適用し、解決は「得点が増える交換は常に実行／§6も常に実行」で確定（見送りは考えない）。
- 抽選は期待値でなく「結果の平均」または無視（=手札増加に小ボーナス）で近似。
- 評価 `eval(state, me)`（軽量）：
  - `+10 * (score[me] - score[opp])`（最優先）
  - `+ 交換直前形×対応タイル所持`（次に点になる形へ小加点）
  - `+ タイル3並びリーチ`（相手の点を奪う布石へ小加点）
  - `+ 手札タイルの有用度・盤面の脅威/妨害`（小）
- 最大の手を選択。同点はタイブレーク（副次ヒューリスティック→固定順）。

**Lv2：深さ制限Expectimax（任意・ストレッチ）**
- MAX/MIN＝手番選択、CHANCE＝袋抽選（`bag`から算出した確率で加重）。
- 反復深化＋置換表（Zobristハッシュ：board＋stock＋hand＋bag＋toMove）。
- 葉で `eval` を呼ぶ。時間 or 深さで打ち切り。終盤は完全読み切り（残り手数が閾値以下）。

`Bot::selectMove(state, budget) -> Move`。budget は「手数深さ」または「ミリ秒」。

---

## 7. ベンチマーク仕様（=「探索速度がどのくらい出せるか」の実測）

`/core/bench` に3本。すべて **native最適化ビルド（-O3 -march=native）** で計測、結果を表で出力。

1. **perft(depth)**：初期局面から `legalPlacements()` で全手を深さ `d` まで展開（CHANCEは全分岐 or 期待値1本に固定の2モード）、葉数と所要時間 → **ノード/秒（生成＋適用スループット）**。move-gen正当性の回帰にも使う。
2. **playout/sec**：ランダム合法手で終局まで対局を繰り返し、**局/秒** を測る（将来のMCTS/自己対戦の目安）。
3. **expectimax-depth**：代表的な中盤局面（サンプル数点）で、`{100ms, 500ms, 1s}` の時間予算内に到達できる**平均深さ**と**探索ノード/秒**を測る。置換表 on/off 比較も出す。

出力例フォーマット：

```
perft d=1..8         : nodes, ms, Mnodes/s
random playouts      : games/s (native)
expectimax @100/500/1000ms : avg depth, Mnodes/s, TT hit%
```

**受け入れ基準（目安・要実測調整）**：native で move-gen ≥ 数Mnodes/s、1s で中盤 expectimax 深さ ≥ 6。数字自体はベンチで確定し、遅ければプロファイル→最適化。

---

## 8. テスト（`SPEC.md §10` 準拠）

- **不変条件**：おはじき総数保存（盤＋両stock＋両score = 24）、タイル総数保存（盤＋両hand＋bag = 24）、`hand合計≤6`、`score≥0`。ランダム自己対戦を多数回し毎手番で検証。
- **エッジケース**：§10の10項目を個別テスト化（4/5連の複数ウィンドウ、同一mixタイル1枚の択一、置いた駒自身が複数方向で少数色→1つ選択、交換で置いたタイルが即3並び、§6相手0点→タイル奪取/上限超過不可、袋不足、上限打ち切り、得点→抽選の順序、最終手のイベント解決後に終局 等）。
- **決定性**：抽選RNGはシード固定で再現可能に。

---

## 9. codexへの依頼単位（タスク分割の目安）

1. データ構造＋ウィンドウ前計算＋`mix`テーブル＋Zobrist。
2. `legalPlacements` / `legalMoves`（解決選択の列挙）。
3. `applyMove`（§5の解決順・確率分岐）＋`isTerminal`。
4. 不変条件テスト＋エッジケーステスト（先にテストを固めるとルール実装が安定）。
5. CLIドライバ（人間入力・ランダム自己対戦）。
6. 簡単Bot Lv1（貪欲＋eval）。
7. ベンチ3種。
8. （任意）Expectimax Lv2＋置換表。

---

## 10. 未確定・後回し
- 評価関数の特徴量の具体的重みは、Lv1稼働後に自己対戦で調整。
- WASM化・GUI連携はフェーズ2で本設計を土台に追加。
