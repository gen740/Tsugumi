# 同梱デモのしくみ

アプリに同梱しているデモが、どんな Card をどうつなぎ、実行すると何が起きるのかをまとめます。
ホーム画面の「デモを見る」から「コピーを開く」を選ぶと、`~/Documents/Tsugumi/<demo>` に編集できるコピーができます（同梱の原本は変わりません）。
画面の操作は[操作ガイド](Usage.md)、Card の種類と実行の規則は[主要機能](features.md#2-card-と-workflow)を参照してください。

| デモ | 題材 | このデモで分かること |
| --- | --- | --- |
| [データからレポートへ](#データからレポートへreport) | 売上表の集計とレポート | seed による再現、失敗と `blocked`、保存入力での単体再実行、成果物、子 Workflow |
| [Collatz (C++ / Python)](#collatz-c--pythoncollatz) | コラッツ数列 | 2 言語での独立実装の照合、分岐・合流・ループ、`maxRuns`、整数あふれの検出 |
| [Live Stream](#live-streamlive-stream) | ランダムウォークの逐次描画 | フィードバックループによるストリーム、即時の停止、Dawn の折れ線 |
| [Plots](#plotsplots) | ランダムウォークの拡散 | ふつうの matplotlib（`plt.show()`・`FuncAnimation`）がそのまま Card に描かれ、Plotly と Altair は対話できるグラフになる |
| [Cas A · Chandra RGB](#cas-a--chandra-rgbcasa) | 超新星残骸の X 線観測 | Nix で固定した観測データ、Card 単位の再実行、版の履歴 |
| [科学デモ](#科学デモ公開データの取得と解析) | 重力波・タンパク質・系外惑星・地震 | 公開データの取得（大きさと SHA-256）と解析・図示 |
| [MNIST · 手書き数字の分類器くらべ](#mnist--手書き数字の分類器くらべmnist-classifiers) | 手書き数字の分類（kNN から CNN まで 7 種） | 1 つの入力から 7 つの分岐が別々の Nix 環境で同時に動いて合流し、1 つの分岐だけを条件を変えて実行し直せる |
| [Newton 法](#newton-法newton) | 複素平面での Newton 法 | 初期値で行き先と収束の速さが変わり、重根や周期軌道では 2 次収束を失う |
| [薬物動態](#薬物動態pharmacokinetics) | 架空の薬物の血中濃度 | 投与間隔で蓄積と山谷が変わり、数値解を閉じた式で確かめる（教材であり服薬の指針ではない） |
| [最大流と最小カット](#最大流と最小カットmax-flow) | 最大流と最小カット | 最大流 = 最小カットを 3 通りで確かめ、ボトルネックだけが流量を変えることを見る |
| [濃度–反応曲線](#濃度反応曲線dose-response) | 濃度–反応曲線（合成データ） | 4 パラメータのロジスティックのあてはめとブートストラップ区間。濃度範囲が EC50 を挟まないと区間が開く |
| [分子の物性](#分子の物性molecules) | 公開された分子の物性（RDKit） | SMILES から物性・構造・類似度を求める。分子を貼り付けて試せて、不正な SMILES は理由を示す |
| [細胞画像の計数](#細胞画像の計数cell-count) | 細胞画像の計数（合成データと実画像） | 正解のある画像で閾値と watershed を採点し、どこで破綻するかを見る |
| [生存時間解析](#生存時間解析survival) | 肺がんの生存時間（survival パッケージの lung） | 対象者と調整変数でハザード比がどう動くかを並べて見る。比例ハザードの仮定も確かめる |
| [メタ解析: BCG ワクチン](#メタ解析-bcg-ワクチンmeta-analysis) | BCG ワクチンのメタ解析（metafor の dat.bcg） | 固定効果と変量効果、異質性、1 試験を除いたときの動き、緯度によるメタ回帰 |
| [RNA-seq: airway とデキサメタゾン](#rna-seq-airway-とデキサメタゾンrna-seq) | RNA-seq（airway とデキサメタゾン） | DESeq2 と edgeR を別の分岐で比べ、細胞株の項を入れるかで遺伝子の数が大きく変わるのを見る |
| [集計と層別で逆転する相関](#集計と層別で逆転する相関simpson) | シンプソンのパラドックス（ペンギンと Berkeley の入学） | 集計と層別で向きが逆転する相関と、どちらを読むかは問い次第であること |
| [Draft 版](#draft-版agent-に実装させる) | 上の科学デモと Cas A の計画だけ | Draft から Agent が Card を生成し、実行して確かめる |
| [力学系ギャラリー](#力学系ギャラリーdynamics-draft) | 6 つの力学系 | 独立した Draft を Agent が helper に手分けして同時に実装する |

図の矢印は Card の線（`出力ポート → 入力ポート`）です。Card の種類は、trigger（実行の起点）、input（Canvas で値を書き換える入力）、fork（分配）、branch（条件分岐）、merge（合流）、subgraph（Card in Card）、コード Card（C++ / Python / bash）です。

## 基本のデモ

### データからレポートへ（`report`）

seed から売上表を作り、地域ごとに集計して Markdown のレポートにし、最後に子 Workflow でまとめて保管します。
Python の Card が中心の、いちばん短い Workflow です。

```text
start → settings ─ share ─┬→ generate ─csv→ aggregate ─summary→ render ─report→ archive（子 Workflow）
                          └──────────────── config ──────────────→ render
```

| Card | 種類 | していること |
| --- | --- | --- |
| `settings` | input | `seed`（42）・`rows`（500）・`heading` を流す |
| `share` | fork | 設定を `generate` と `render` に配る |
| `generate` | Python | `seed` の乱数で地域・商品・数量・単価の CSV を `rows` 行書く。同じ `seed` なら同じ CSV になる |
| `aggregate` | Python | 地域ごとに売上と数量を合計する。`fail_on_purpose` を On にすると、わざと失敗する（`recordIO` で入出力値を記録） |
| `render` | Python | `report.md` と `summary.json` をディレクトリに書き、レポートを表示する（`retainArtifact` で成果物を保持） |
| `archive` | subgraph | 中の bash の Card `copy` が、レポートのディレクトリを複製して `ARCHIVED_AT` を添える |

**試すこと**
- `seed` や `heading` を変えて実行する。同じ `seed` なら集計は同じで、見出しだけが変わる。
- `aggregate` の `fail_on_purpose` を On にして実行する。`aggregate` のログに理由が出て、下流の `render` は `blocked` になる。Off に戻し、`aggregate` だけを実行し直すと、保存された入力で即座に成功する。
- `archive` をダブルクリックして子 Workflow に入り、成果物のディレクトリをプレビュー・書き出す。成果物は実行キャッシュを消しても残る。

### Collatz (C++ / Python)（`collatz`）

同じコラッツの反復を C++ と Python で別々に実装し、両方の数列が一致するかを確かめます。
反復そのものを Card の線で描いているので、分岐・合流・ループの動きを Canvas でたどれます。

```text
start → initial ─ both ─┬→ collatz-cpp（子 Workflow）─┬→ compare ─sequence→ record
                        └→ collatz-py （子 Workflow）─┘
```

外側の Card:

| Card | 種類 | していること |
| --- | --- | --- |
| `initial` | input | 初期値（27）を流す |
| `both` | fork | 同じ値を C++ 側と Python 側に配る |
| `collatz-cpp` / `collatz-py` | subgraph | 中の Workflow で反復し、1 に着くまでの数列を返す |
| `compare` | Python | 2 つの数列が完全に一致することを確かめ、数列を次へ渡す |
| `record` | Python | 数列を `collatz.csv`（`k, a_k, parity`）に書き、成果物として残す |

子 Workflow の中（C++ と Python で同じ形）:

```text
（境界の入力 n）→ loop ─ split ─┬→ check ─value / even→ route ─a（偶数）→ halve ─┐
                  ↑              └→ collect →（境界の出力 sequence）        └b（奇数）→ triple ─┤
                  └──────────────────────────── half / triple ───────────────────────────────┘
```

| Card | 種類 | していること |
| --- | --- | --- |
| `loop` | merge | 最初の値と、`halve`・`triple` から戻る値を合流させる |
| `split` | fork | 値を `check` と `collect` に配る |
| `check` | コード | 値が 1 なら何も出さない（ループが終わる）。そうでなければ値と偶奇を出す。`maxRuns`（200）が反復の上限 |
| `route` | branch | 偶数なら `a`（`halve`）、奇数なら `b`（`triple`）の片側だけを発火させる |
| `halve` / `triple` | コード | `n/2` と `3n+1` を計算する。`triple` は 64 bit 整数を超えると `integer overflow` で失敗する |
| `collect` | コード | 届いた値をためて表示し、1 が届いたら数列を出す。常駐 worker の状態は `onRunStart` で実行ごとに空にする |

**試すこと**
- そのまま実行する。27 は 111 手（112 個の値）で 1 に着き、途中の最大値は 9232。`compare` が一致を報告する。
- `initial` を 2 にすると、奇数側の `triple` は一度も選ばれず `skipped` になる。
- 中の `check` の `maxRuns` を小さくすると、反復上限（`execution_limit`）で止まる。`3n+1` が 64 bit を超える初期値では `integer overflow` を検出する。

### Live Stream（`live-stream`）

100 ms ごとにランダムウォークの値を 1 つ作り、直近の値を折れ線でリアルタイムに描きます。
「次の状態」を自分に戻すフィードバックループで、終わりのある流れを作っています。

```text
start → settings ─state→ loop → generate ─sample→ plot
                          ↑         │
                          └─ next ──┘
```

| Card | 種類 | していること |
| --- | --- | --- |
| `settings` | input | `seed`（7）・`samples`（200）・`interval_ms`（100）と初期状態を流す |
| `loop` | merge | 初期状態と、`generate` が返す次の状態を合流させる |
| `generate` | C++ | `interval_ms` だけ待ってから、`seed` と番号で決まる乱数で 1 歩進め、値と次の状態を出す。`samples` 個で次の状態を出さなくなり、流れが終わる。待つ間も停止を確かめる |
| `plot` | Python | 直近 `window`（60）個の値とその平均を、Dawn（WebGPU / Metal）の折れ線で描く |

**試すこと**
- 実行して、途中で停止（Cancel）する。即座に止まり、その時点までのグラフが残る。
- `interval_ms`・`seed`・`samples` や `plot` の `window` を変えて実行し直す。系列はリセットされ、並列度 1 でも 2 でも、値が欠けたり順序が入れ替わったりしない。

### Plots（`plots`）

ランダムウォークを作り、ほかの場所で書くのと同じ matplotlib・Plotly・Altair のコードで図にします。
Tsugumi 専用の描画呼び出しは使っていません。Python の Card は SDK の matplotlib backend を使うので、`plt.show()` で描いた図がそのまま Card の表示に出ます。Plotly の `fig.show()` と Altair の `chart.show()` は、対話できるグラフとして出ます。

```text
start → settings ─walk→ simulate ─walks→ share ─┬→ summary
                                                ├→ diffusion
                                                ├→ explore
                                                └→ spread
```

| Card | 種類 | していること |
| --- | --- | --- |
| `settings` | input | `seed`（7）・`walkers`（400）・`steps`（200）を流す |
| `simulate` | Python | numpy で `walkers` 本の ±1 のランダムウォークを `steps` 歩ぶん作る |
| `share` | fork | 同じ軌跡を 4 つの図に配る |
| `summary` | Python | `plt.show()` で、40 本の軌跡と ±2√t の帯、最終位置のヒストグラムと正規分布を 1 枚に描く |
| `diffusion` | Python | `FuncAnimation` で、時刻ごとの分布が √t で広がる様子をライブで再生する（`frames` コマ） |
| `explore` | Python | Plotly の `fig.show()` で、各ウォーカーの最終位置と最も遠くまで行った距離を散布図にする（色は 0 より上にいた時間の割合） |
| `spread` | Python | Altair の `chart.show()` で、平均二乗距離が t に比例して増える様子を理論値と並べて描く |

**試すこと**
- 実行すると、`diffusion` の表示がアニメーションとして動き、最後のコマが残る。
- `explore` と `spread` の表示を拡大し、ズーム・ホバー（ウォーカーごとの値）・凡例を操作する。
- `walkers`・`steps`・`seed` や `frames` を変えて実行し直す。

### Cas A · Chandra RGB（`casa`）

超新星残骸カシオペヤ座 A の X 線イベントデータ（Chandra）を Nix で固定して取得し、光子を 3 つのエネルギー帯に分けて RGB 画像にします。
同じ光子のエネルギースペクトルも描き、どの元素の輝線がどの色になったのかを確かめます。

```text
start → load ─events→ bands ─images→ share ─┬→ rgb
                                            └→ spectrum
```

| Card | 種類 | していること |
| --- | --- | --- |
| `load` | bash | `default.nix` で、HEASARC の観測ファイルを hash で固定して取得・展開する（explicit FTPS）。ダウンロードは最初の 1 回だけで、以後は Nix store のファイルを使う。実行中はネットワークを使わない |
| `bands` | Python | 光子を 600–1650 / 1650–2250 / 2250–7500 eV の 3 帯域に分け、Cas A の中心（RA 350.8584°, Dec +58.8113°）に合わせた 360×360 の格子で数える。帯域ごとの FITS と `provenance.json` を書き、光子数を表で示す |
| `share` | fork | 帯域のディレクトリを `rgb` と `spectrum` に配る |
| `rgb` | Python | 3 帯域を R・G・B に割り当て、asinh で伸長した PNG を作る（`percentile` 99.5、`q` 10）。表示条件と PNG の SHA-256 を記録する |
| `spectrum` | Python | 同じ格子の範囲の光子のエネルギーをヒストグラムにして matplotlib で描く。3 帯域を色で塗り、Mg・Si・S・Ar・Ca・Fe の輝線に印を付ける |

**試すこと**
- そのまま実行する。既定の観測は ObsID 25628（2.1 ks、13 MB）。`bands` が帯域ごとの光子数（赤 101,187 / 緑 157,471 / 青 110,852）を表で示す。
- `load` の `obsid`・`file`・`hash` を ObsID 114（50 ks、520 MB）に変えると、深い観測で鮮明な画像になる（値は `load` の説明にある）。最初の実行だけ取得に時間がかかる。
- `bands` の帯域や `rgb` の伸長、`spectrum` の範囲・刻みを変えて、その Card だけを実行し直す。スペクトルを見ると、緑の帯域がちょうど Si の輝線を含み、S から上は青に入ることが分かる。
- 版のツリーを開く。帯域や格子・伸長を試した過程（失敗した枝、採用しなかった枝、採用した版 `rgb-v1`、その上にスペクトルを足した版）が入っている（[版の管理](Versions.md#デモの履歴)）。

## 科学デモ（公開データの取得と解析）

4 つのデモは同じ形をしています。`load` が公開元からデータを取得し、続く 2 つの Card が解析して図や表にします。

```text
start → load ─data→（解析 1）→（解析 2）
```

`load` はネットワークへのアクセスを許可した Python の Card（`network: true`）で、`urls` と `names` のファイルを取得してフォルダとして渡し、それぞれの大きさと SHA-256 を表示します。
Cas A の `load` が Nix で取得を固定するのに対し、こちらは実行のたびに取得します。

| デモ | 取得するデータ | 解析 1 | 解析 2 |
| --- | --- | --- | --- |
| **GW150914 · 重力波**（`gw150914`） | GWOSC の H1・L1 のひずみデータ（4 kHz、32 秒の HDF5） | `whiten`: 4 秒区間の Welch 法の PSD で白色化し、`f_low`〜`f_high`（35〜350 Hz）のバンドパスを掛けて、イベント前後の波形を描く | `chirp`: L1 を ±10 ms ずらして H1 と最もよく重なる時間差を求めて重ね描きし、H1 のスペクトログラムでチャープを見せる。大まかな SNR も求める |
| **6LU7 · タンパク質と結合ポケット**（`protein-6lu7`） | RCSB PDB の 6LU7（SARS-CoV-2 メインプロテアーゼと阻害剤 N3） | `structure`: Cα 原子と阻害剤（鎖 C）の座標を取り出し、3D 点群（Cα は青、阻害剤は赤）で表示する | `pocket`: 阻害剤から `cutoff`（4 Å）以内の残基を表にし、コンタクトマップにポケットの残基を印す。触媒の His41・Cys145 が入ることを確かめられる |
| **Kepler-6 b · 系外惑星トランジット**（`exoplanet-transit`） | MAST の Kepler-6（KIC 10874614）の第 3 四半期のライトカーブ（FITS） | `detrend`: 品質フラグ 0 の点を規格化し、`window_days` 日の移動中央値でトレンドを除いて、Box Least Squares で周期を探す | `fold`: 周期で位相を折り畳み、減光の深さ δ から惑星半径比 Rp/R* = √δ を求める（周期約 3.23 日、深さ約 1%） |
| **日本海溝 · 震源断面と b 値**（`seismic-trench`） | USGS の 2000〜2024 年、北緯 34〜42 度・東経 138〜146 度、M4 以上の震源（CSV） | `section`: `lat_min`〜`lat_max` の緯度帯を切り出し、震央の地図と、沈み込む太平洋プレートが見える経度–深さの断面を描く | `b_value`: 累積頻度 log10 N(≥M) を描き、`mc` 以上について最尤法（Aki–宇津）で Gutenberg–Richter 則の b 値を求める |

**試すこと**: そのまま実行してから、解析の Card のパラメータ（通過帯域、`cutoff`、`window_days`、緯度帯、`mc` など）を変えて、その Card だけを実行し直す。取得し直さずに、保存された入力で解析だけをやり直せます。

## 手元で完結する計算のデモ

実行中にネットワークを使わないデモです。データは合成するか、パッケージに同梱のものを使うか、Nix で固定します（取得はビルドのときに 1 回だけ）。実行は `TSUGUMI_TEST_DEMO_RUNS=1` のテストで確かめています。

### MNIST · 手書き数字の分類器くらべ（`mnist-classifiers`）

```
start → load → prepare → share ─┬→ knn ──────┐
                                ├→ logistic ─┤
                                ├→ svm ──────┤
                                ├→ forest ───┤
                                ├→ boosting ─┼→ compare → views ─┬→ tradeoff
                                ├→ mlp ──────┤                   ├→ recall
                                ├→ cnn ──────┤                   └→ hard
                                └────────────┘（テスト画像とラベル）
```

| Card | 中身 | すること |
| --- | --- | --- |
| `start` | trigger | 実行の起点 |
| `load` | Python（numpy, matplotlib） | MNIST の IDX 4 ファイルを `default.nix` の `fetchurl` と hash で固定する。ヘッダーと枚数を確かめ、数字ごとの見本を並べて出典とともに表示する。実行中にネットワークは使わない |
| `prepare` | Python（numpy） | `seed`・`train_size`（既定 10000）・`test_size`（既定 2000）で層化抽出し、画素を 0〜1 に揃えて npz（File ポート、成果物として保持）で渡す。数字ごとの枚数を表示する |
| `share` | fork | 7 つの分類器と `compare` に同じ部分データを渡す |
| `knn` | scikit-learn | k 近傍法（`k`）。間違えた数字とその近傍を表示する |
| `logistic` | scikit-learn | ロジスティック回帰（`c`, `max_iter`）。数字ごとの重みの画像を表示する |
| `svm` | scikit-learn | RBF カーネルの SVM（`c`, `gamma`）。数字ごとのサポートベクター数を表示する |
| `forest` | scikit-learn | ランダムフォレスト（`trees`, `max_depth`, `seed`）。効いた画素の画像を表示する |
| `boosting` | xgboost | 勾配ブースティング（`rounds`, `max_depth`, `eta`, `seed`）。回数ごとのテスト誤り率を表示する |
| `mlp` | torch（CPU） | 隠れ層 2 つの全結合ネット（`hidden`, `epochs`, `seed`）。損失の推移を表示する |
| `cnn` | torch（CPU） | 畳み込み 2 層の小さなネット（`channels`, `epochs`, `seed`）。1 層目がテスト画像から取り出したものを表示する |
| `compare` | Python（numpy） | 全分岐を待つ。同じテスト画像か（digest）を確かめ、正解率を予測から計算し直して照合し、順位表と、1 位と 2 位の差に意味があるかを文章で表示する |
| `views` | fork | 比較結果を 3 つの図に渡す |
| `tradeoff` | Plotly | 正解率と学習時間・予測時間の散布図（対数軸、パレート最適を線で結ぶ、ホバーで詳細） |
| `recall` | matplotlib | モデル × 数字の再現率の表 |
| `hard` | matplotlib | 間違えたモデルが多いテスト画像 32 枚と、多かった誤答 |

分類器の Card は共通して、正解率・1 スレッドの CPU 時間（学習と予測）・間違いの内訳を表示し、型付きのメッセージ（`Result`）でテスト全枚の予測を `compare` に渡します。

**試すこと**

- そのまま実行する。7 つの分岐が同時に動きます（同時に動く数は設定の「並列実行数」、既定 4）。
- `forest` の `max_depth` を 3 にして、その Card だけを実行する。他の分岐は動かず、`forest` の正解率だけが 0.9475 から 0.7570 に下がります。
- 比べ直すときは `forest` から先を実行する（「ここから下流を実行」）。動くのは `forest` と `compare` から先だけで、`compare` は新しい `forest` の結果と、他の分岐・`share` の保存された入力で比べます。変えていない分岐は動かないので、その正解率は同じです。
- `knn` の `k`、`svm` の `gamma`、`cnn` の `epochs` を変える。
- `prepare` の `train_size` を増やして、どの分類器が枚数の恩恵を受け、どれの時間が伸びるかを見る（`svm` と `knn` の予測時間が伸びます）。

データは LeCun・Cortes・Burges の MNIST で、PyTorch のミラー `https://ossci-datasets.s3.amazonaws.com/mnist/` から取得します。

### Newton 法（`newton`）

$z^3 - 1 = 0$ を複素平面で解き、初期値が行き先と収束の速さをどう決めるかを見ます。2 次収束の見積もりと収束先の地図に加えて、2 次収束を失う場合と収束しない場合を並べます。

```text
start → settings → roots ─ share ─┬→ orbits ─errors→ order
                                  ├→ basins
                                  └→ trouble
```

| Card | 種類 | していること |
| --- | --- | --- |
| `settings` | input | 多項式の実数係数 `coeffs`（高次から、既定は `[1, 0, 0, -1]`）、減衰係数 `damping`、`tolerance`、`max_iter` を流す |
| `roots` | Python | Newton 法を使わず、コンパニオン行列の固有値（`numpy.roots`）で根と重複度、臨界点を求める。根と係数の関係で確かめ、$z^n = c$ の形なら閉じた式とも比べる |
| `share` | fork | 問題を `orbits`・`basins`・`trouble` に配る |
| `orbits` | Python | `start_re`・`start_im` の初期値ごとに、誤差を反復に対して対数軸で描く |
| `order` | Python | 誤差の列から収束の次数 $q$ と定数を見積もり、重複度と $\lambda$ から決まる理論値と並べて表にする |
| `basins` | Python | 格子の全点を初期値にして、どの根に着くかと反復の回数を描く。`center_re`・`center_im`・`half_width` で拡大できる |
| `trouble` | Python | 基準の場合、臨界点の近く（`offset`）、重根、$\lambda = 2$ で 2 次に戻した重根、周期 2 の閉路を並べる（多項式は固定） |

**試すこと**
- `basins` の `center_re` を -0.8、`half_width` を 0.2 にして、その Card だけを実行し直す。境界のどこを拡大しても 3 色が集まっている。
- `trouble` の `offset` を 1e-5 にする。最初の一歩が $1/(3 z_0^2)$ まで飛ばされ、遠くでは 1 回に 2/3 ずつしか戻らないので、反復が 37 回から 60 回に増える（既定の `max_iter` ちょうど。さらに小さくすると `max_iter` を増やす必要がある）。
- `settings` の `damping` を 0.5 にして全体を実行する。どの初期値でも次数が 1、比が 0.5 になり、地図の境界の形も変わる。
- `settings` の `coeffs` を `[1, 0, -3, 2]`（$(z-1)^2(z+2)$）にする。重複度 2 の根で次数が 1、比が 0.5 に落ち、誤差はおよそ $10^{-8}$ で止まる。
- `settings` の `coeffs` を `[1, 0, -2, 2]`（$z^3 - 2z + 2$）にする。0 の周りに黒い領域（周期 2 の閉路に吸い込まれて収束しない初期値）が現れる。

### 薬物動態（`pharmacokinetics`）

架空の薬物「ツグミシン」の 1 コンパートメント経口モデルで、投与間隔を変えると濃度がどう動くかを比べます。**教育用の架空のモデルです。実在の薬の値ではなく、医療上の助言や投与の指針でもありません。** 有効域も作り物です。

```text
start → settings → simulate ─curves→ share ─┬→ check
                                            ├→ window
                                            └→ missed
```

| Card | 種類 | していること |
| --- | --- | --- |
| `settings` | input | 比べる投与計画（`doses_mg`・`intervals_h`）、`half_life_h`、`ka_per_h`、`volume_l`、`bioavailability`、作り物の有効域（`low_mg_l`・`high_mg_l`）、`days` を流す |
| `simulate` | Python | 微分方程式を投与ごとに区切って `solve_ivp` で解き、投与計画を並べて濃度の推移を Plotly で描く |
| `share` | fork | 結果を `check`・`window`・`missed` に配る |
| `check` | Python | 閉じた式の重ね合わせ、定常状態の蓄積比 $R$、平均濃度を数値解と比べて表にする。差が大きければ失敗する |
| `window` | Python | 最後の投与間隔の濃度を重ね、山、谷、有効域の中にいる時間の割合を比べる |
| `missed` | Python | `missed_at_h` 以降の投与を `missed_count` 回抜いたときの濃度を、閉じた式で投与計画ごとに描く |

**試すこと**
- そのまま実行して `window` を見る。400 mg を 24 時間ごとと 200 mg を 12 時間ごとは平均濃度が同じ（`check` の表）で、間隔を詰めると山と谷の差が縮む。
- `missed` の `missed_count` を 2 にして、その Card だけを実行し直す。1 日量が同じなら、間隔が長い計画ほど有効域の下にいる時間が大きく増える。
- `settings` の `half_life_h` を 16 にして全体を実行する。蓄積比が大きくなり、定常に着くまでの時間も延びる。
- `settings` の `days` を短くすると、定常に着かないことが `check` の「定常への到達」に出る。

### 最大流と最小カット（`max-flow`）

seed から作った架空の層状ネットワークで、最大流と最小カットが等しいことを確かめます。どの辺の容量を増やせば最大流が増え、どの辺では増えないのかを、比べて見られます。

```text
start → settings → network → flow → cut ─ share ─┬→ verify
                                                 ├→ whatif
                                                 └→ sweep
```

| Card | 種類 | していること |
| --- | --- | --- |
| `settings` | input | `seed`（53）、`layers`、`width`、`edge_probability`、`max_capacity` を流す |
| `network` | Python | seed から層状の容量つき有向グラフを作る（架空のデータ） |
| `flow` | Python | networkx で最大流を求め、各辺の流量/容量を図に描く。容量と保存則を確かめる |
| `cut` | Python | networkx で最小カットを求め、$s$ 側、$t$ 側、カットの辺を図に描く。最大流 = 最小カットでなければ失敗する |
| `share` | fork | 結果を `verify`・`whatif`・`sweep` に配る |
| `verify` | Python | scipy の `maximum_flow` と全カットの総当たりで同じ値が出ることを確かめ、最小カットの数を数える |
| `whatif` | Python | カット上の辺とカット外の辺の容量を `raise_by` だけ増やし、最大流と新しいカットを表で比べる。`edge`（例: `a1->b1`）で辺を選べる |
| `sweep` | Python | すべての辺について、容量を `raise_by` だけ増やしたときの最大流の増分を棒で並べる |

**試すこと**
- そのまま実行して `sweep` を見る。カットの 3 本のうち増えるのは 2 本だけで、`a2->b2` は別の最小カットが残っているので増えない（`verify` が最小カット 2 個と数える）。
- `whatif` の `edge` を `s->a2` にして、`raise_by` を 1000 にする。容量いっぱいに流れている辺でも、カット外なら最大流は増えない。
- `whatif` の `edge` を `b3->c3` にする。10 増やしても 8 しか増えない。次のカットが詰まるからで、`raise_by` を増やしても増分はそこで止まる。
- `settings` の `seed` を変えて全体を実行する。別のネットワークでも `verify` の値はすべて一致する。

### 濃度–反応曲線（`dose-response`）

架空の化合物 A・B のプレートを乱数で作り（合成データ）、4 パラメータロジスティック（Hill 曲線）を当てはめて、EC50 と Hill 勾配に信頼区間を付けます。
真の値を知っているので、回復できたかをその場で確かめられます。実在の薬ではなく、医療上の助言でもありません。

```text
start → generate ─plate→ share ┬→ fit ─fit─────────→ bootstrap ─intervals→ compare
                               ├──────── plate ────→ bootstrap
                               └→ range
```

| Card | 種類 | していること |
| --- | --- | --- |
| `generate` | Python | `seed` の乱数で、真の曲線に正規ノイズを乗せた反応を作る。濃度は `top_um` から `dilution` 倍ずつ薄めた `concentrations` 点、各濃度 `replicates` 反復 |
| `share` | fork | プレートを `fit`・`bootstrap`・`range` に配る |
| `fit` | Python | scipy の `curve_fit` で bottom・top・log10 EC50・Hill 勾配を求め、データと曲線を描く |
| `bootstrap` | Python | 残差ブートストラップ（`n_boot` 回）で EC50 と Hill 勾配の 95% 区間を求め、真の値が入るかを表にする。区間が測定範囲を出ると、理由を添えて警告する |
| `compare` | Python | EC50 の比（相対力価）と区間、Hill 勾配の差を示す。区間が 1 を含めば「差は言えない」と表示する |
| `range` | Python | 最高濃度だけを変えた合成プレートを作り直し、EC50 の区間の幅を最高濃度に対して描く |

**試すこと**
- `generate` の `noise_sd` を 12 にして実行する。区間が広がる（B の EC50 で 1.2 倍から 1.7 倍）。
- `generate` の `top_um` を 1 にして実行する。B の曲線は上がりきらず、区間の幅が 1000 倍を超えて `bootstrap` が「濃度範囲が EC50 を挟んでいません」と警告し、`compare` は「力価の差は言えない」と表示する。
- `concentrations` や `replicates` を減らす。`bootstrap` の `n_boot` を変えて、その Card だけを実行し直す。

### 分子の物性（`molecules`）

分子を文字列で表す SMILES から、RDKit で物性を計算し、構造式、Lipinski の Rule of Five、分子量と logP の地図、構造の似かたを並べます。
Canvas の `paste` に SMILES を貼れば、その分子が全部の Card に加わります。

```text
start → paste ─pasted→ properties ─molecules→ share ┬→ structures
                                                    ├→ lipinski ─checked→ chemspace
                                                    └→ similarity
```

| Card | 種類 | していること |
| --- | --- | --- |
| `paste` | input | 試したい分子の `name` と `smiles` を流す（既定は dopamine。`smiles` を空にすると一覧だけになる） |
| `properties` | Python | `molecules`（「名前: SMILES」の一覧）と貼られた分子を読み、分子量・logP（Crippen）・TPSA・HBD・HBA・回転可能結合を表にする。読めない SMILES は理由を示して飛ばす |
| `share` | fork | 物性を 3 つの Card に配る |
| `structures` | Python | 構造式を名前と分子式を添えて格子状に描く |
| `lipinski` | Python | 分子量 ≤ 500、logP ≤ 5、NH+OH ≤ 5、N+O ≤ 10 を確かめ、違反が `max_violations` 以下なら通過とする |
| `chemspace` | Python（Plotly） | 分子量と logP の散布図。ホバーで名前・分子式・TPSA・違反した規則が分かる |
| `similarity` | Python | Morgan フィンガープリント（半径 `radius`）の Tanimoto 係数をヒートマップにする |

**試すこと**
- `paste` の `smiles` に別の SMILES を貼って実行する。
- `paste` の `smiles` を `C1CC(` や `C(C)(C)(C)(C)C` にして実行する。落ちずに、`properties` の表の上に読めない理由が出て、ほかの分子は計算される。
- `lipinski` の `max_violations` を 0 にして、その Card だけを実行し直す。cholesterol が不通過になる。
- `similarity` の `radius` を 0 にして、その Card だけを実行し直す。見分けが粗くなり、似かたの平均が上がる。

### 細胞画像の計数（`cell-count`）

中心が分かっている蛍光風の合成画像（synthetic）を作り、閾値と watershed で細胞を数えて採点します。
接した細胞を切り分けると何が得られるか、どこから数えられなくなるかが分かります。下の列では同じ手順を実際の核の画像にかけます（正解がないので採点はしません）。

```text
start → begin ┬→ generate ─scene→ share ┬→ preprocess ─cleaned→ segment ─labels→ score
              │                         ├──────────────── scene ───────────────→ score
              │                         └→ sweep
              └→ mitosis ─image→ real
```

| Card | 種類 | していること |
| --- | --- | --- |
| `begin` | fork | 実行の起点を合成画像と実画像の両方に配る |
| `generate` | Python | `seed` の乱数で `cells` 個の細胞を置く（`touching_pairs` 組は接する）。照明のむら `unevenness` とノイズ `noise` を乗せる |
| `share` | fork | 画像と真の中心を `preprocess`・`score`・`sweep` に配る |
| `preprocess` | Python | グレースケール開放で背景を見積もって引き、ガウスぼかしをかける |
| `segment` | Python | 大津の閾値で前景を求めて領域に番号を振り（閾値のみ）、距離変換の極大を種に watershed で切り分ける |
| `score` | Python | 重心を真の中心と 1 対 1 に対応づけ、個数の誤差・適合率・再現率・F1 を出す。見つけたもの・余分なもの・見逃したものを両方の方法で重ね描きする |
| `sweep` | Python | 細胞の数とノイズを変えた画像を作り直し、同じ手順の F1 を表にする |
| `mitosis` | bash | ヒト培養細胞の核の蛍光画像を Nix のハッシュで固定して渡す（Moffat et al., Cell 2006、CC0。scikit-image のデータ集から。取得は Nix のビルド時に 1 回だけで、実行中はネットワークを使わない） |
| `real` | Python | 同じ手順を実画像にかけ、個数と輪郭を示す |

**試すこと**
- `generate` の `cells` を 320 にして実行する。閾値のみの再現率は 0.24 に落ち、watershed は 0.79 を保つ。
- `generate` の `noise` を上げる。`sweep` の表の左上のとおり、細胞が少ないほど閾値が雑音に落ちて数えすぎる。
- `segment` の `min_distance` を変えて、その Card だけを実行し直す。大きくすると切り分けが減る。

### 生存時間解析（`survival`）

survival パッケージ同梱の `lung`（NCCTG 進行肺がん試験の実データ、228 人。Loprinzi et al., J Clin Oncol 1994）で、Kaplan–Meier 曲線、log-rank 検定、Cox モデルを求めます。
同じハザード比が「誰を含めるか」「何で調整するか」でどれだけ動くかを、Canvas の 1 枚の図で並べます。古い試験データの中の関連で、原因を示すものでも助言でもありません。

```text
start → load ─lung→ share ─┬→ cohort ─cohort→ split ─┬→ km
                           │                         └→ cox ─fit→ zph
                           └→ specs
```

| Card | 種類 | していること |
| --- | --- | --- |
| `load` | R | `lung` に `event`・`female` の列を足して CSV にし、変数の中央値・範囲・欠損を表で示す |
| `share` | fork | データを `cohort` と `specs` に配る |
| `cohort` | R | `age_min`・`age_max`・`ecog_max` で対象者を選ぶ。人数と死亡数を全体と並べる |
| `split` | fork | 選んだ人を `km` と `cox` に配る |
| `km` | R | `group`（sex / ecog / age）で分けた Kaplan–Meier 曲線、中央値、log-rank 検定。曲線の値を `curves.csv` に残す |
| `cox` | R | `covariates` の変数で Cox モデルを当てはめ、ハザード比と 95% 信頼区間を表で示す |
| `zph` | R | `cox.zph` のスケール化 Schoenfeld 残差を描き、変数ごとと全体の p 値を示す |
| `specs` | R | `exposure` のハザード比を `subsets` × `adjustments` のすべての組み合わせで求め、森林図のように並べる |

**試すこと**
- `specs` の図を読む。女性のハザード比は全員で 0.59 ですが、65 歳未満で ECOG まで調整すると 0.76（信頼区間は 1 をまたぐ）、ECOG 2 以上で 4 変数を調整すると 0.39 になります。後者は体重減少の欠損で 51 人が 46 人に減っています。
- `km` の `group` を `ecog` にして実行する。log-rank の χ² が 10.33 から 18.98 に変わる。
- `cox` の `covariates` に `ph.karno` を入れ、`cox` から先を実行する。`zph` の全体の p 値が 0.216 から 0.016 に下がり、比例ハザードの仮定が怪しくなる（単体の実行では `zph` の入力は更新されないので、`cox` からつなげて実行する）。
- `cohort` の `age_min` を 65 にして実行し、`km` と `cox` の変化を見る。

### メタ解析: BCG ワクチン（`meta-analysis`）

metafor 同梱の `dat.bcg`（結核に対する BCG ワクチンの 13 試験、Colditz et al., JAMA 1994）で、固定効果と変量効果の併合、異質性、緯度に対するメタ回帰を比べます。
1 試験を除く、推定量を変えるといった選択で、結論がどれだけ動くかを Canvas で見られます。統計手法の例で、接種についての助言ではありません。

```text
start → trials ─trials→ share ─┬→ pool
                               ├→ funnel
                               ├→ moderator
                               └→ sensitivity
```

| Card | 種類 | していること |
| --- | --- | --- |
| `trials` | R | 人数から各試験の log リスク比と分散（`escalc`）を求め、表と CSV にする |
| `share` | fork | 同じ効果量を 4 つの解析に配る |
| `pool` | R | 固定効果と変量効果（`method`）で併合し、両方の菱形を載せた森林図と I²・τ²・Q を示す。`exclude` に試験番号を書くと除く |
| `funnel` | R | ファンネルプロットに回帰検定と trim and fill の結果を添える |
| `moderator` | R | `moderator`（既定は緯度の絶対値）でメタ回帰を行い、傾き・残差 τ²・説明された割合を図に示す |
| `sensitivity` | R | 1 試験ずつ除いたとき（固定効果と DL）と、τ² の推定量を変えたときの併合リスク比を並べる |

**試すこと**
- `pool` の森林図で、固定効果 0.65 と変量効果 0.49 の菱形を比べる。I² は 92.1%。
- `pool` の `exclude` を `[8]`（TPT Madras、最も大きい試験）にして `pool` だけを実行する。固定効果は 0.65 から 0.48 に動く。
- `pool` の `method` を `REML` にする。τ² は 0.309 から 0.313 に変わるが、併合リスク比はほとんど動かない。
- `moderator` で、緯度が τ² を 0.313 から 0.076 に減らすのを見る。`moderator` を `year` にして比べる。

### RNA-seq: airway とデキサメタゾン（`rna-seq`）

Bioconductor の airway（気道平滑筋細胞の 4 細胞株をデキサメタゾンで処理した実データ。Himes et al., PLoS One 2014、GEO GSE52778）で、発現が動く遺伝子を DESeq2 と edgeR で求めます。
「発現変動遺伝子の数」は閾値とデザインでどれだけ変わるのかを、それぞれの Card で確かめられます。細胞培養の実験で、治療の効果を示すものでも医学的な助言でもありません。

```text
start → load ─airway→ filter ─counts→ share ─┬→ normalize ─normalized→ pca
                                             ├→ deseq ─results→ fan ─┬→ volcano
                                             │                       ├→ thresholds
                                             │                       ├→ top
                                             │                       ├→ compare ←─results─ edger
                                             │                       └→ design
                                             ├→ edger
                                             └→ design
```

| Card | 種類 | していること |
| --- | --- | --- |
| `load` | R | airway のカウント行列（63,677 遺伝子 × 8 サンプル）と遺伝子名を CSV にし、サンプルの一覧を表で示す |
| `filter` | R | 合計リード数が `min_total` 未満の遺伝子を除き、分布と基準を図で示す |
| `share` | fork | カウントを 4 つの Card に配る |
| `normalize` | R | 中央値の比でサイズ因子を求め、正規化の前後の分布を比べる。PCA のために vst を渡す |
| `pca` | R | 分散の大きい `top_genes` 個の遺伝子で主成分分析を行い、処理と細胞株で塗り分ける |
| `deseq` | R | `paired` で `~ cell + dex` か `~ dex` を選び、DESeq2 の結果を CSV にして MA プロットを描く |
| `fan` | fork | DESeq2 の結果を配る |
| `volcano` | R | `fdr`・`lfc` の閾値で数え、`known` の遺伝子に名前を付けて火山プロットを描く |
| `thresholds` | R | FDR と log2FC の格子ごとの遺伝子の数を表にする |
| `top` | R | FDR の順に上位 `top` 個を表にし、`known` の既知の応答遺伝子の順位を添える |
| `edger` | R | 同じカウントを edgeR の準尤度 F 検定で検定する |
| `compare` | R | DESeq2 と edgeR の log2FC を重ね、両方と片方だけの遺伝子を数える |
| `design` | R | `~ dex` でも DESeq2 を実行し、`~ cell + dex` と推定値・統計量を並べる |

**試すこと**
- `design` の図を読む。細胞株の項を入れると FDR < 0.05 の遺伝子は 2713 から 4030 に増える。log2FC はほぼ同じで、統計量だけが大きくなる。
- `volcano` の `lfc` を 1 から 2 に、`fdr` を変えて `volcano` だけを実行する。`thresholds` の表にある数と同じになる。
- `filter` の `min_total` を 100 にして、`filter` から先を実行する。
- `top` で FKBP5、DUSP1、TSC22D3 の順位を見る。遺伝子ごとの FDR の順位と倍率の大きさは同じ順ではない（FKBP5 は 16.5 倍で 143 位、DUSP1 は 7.7 倍で 3 位）。

### 集計と層別で逆転する相関（`simpson`）

全体で見た関係と層に分けた関係で、符号が逆になることがあります。2 つの実データで確かめます。ペンギンのくちばしの長さと厚さ（palmerpenguins、Gorman et al. 2014）と、1973 年の Berkeley の出願（R の `UCBAdmissions`）です。
どちらの値を読むべきかはデータだけでは決まらず、因果の問いで決まることをノートに書いています。

```text
start → branches ─┬→ penguins ─penguins→ share ─┬→ correlate ─points, correlations→ figure
                  │                             └→ survey
                  └→ admissions ─admissions→ applications
```

| Card | 種類 | していること |
| --- | --- | --- |
| `branches` | fork | 実行を 2 つの枝に配る |
| `penguins` | R | くちばしの長さと厚さがそろう 342 羽を CSV にし、種と島ごとの数を表で示す |
| `share` | fork | データを `correlate` と `survey` に配る |
| `correlate` | R | `stratify`（species / sex / island）で分け、全体と各層の r、信頼区間、傾きを表にする |
| `figure` | R | 同じ点に全体の回帰直線（破線）と層ごとの回帰直線を重ねる |
| `survey` | R | 3 つの層別の r を 1 枚に並べ、どこで逆転するかを示す |
| `admissions` | R | 学科ごとと全体の合格率とオッズ比、Mantel–Haenszel の共通オッズ比を表にする |
| `applications` | R | 合格率を応募者数の大きさの点で描き、全体の合格率を破線で重ねる |

**試すこと**
- `correlate` の `stratify` を `sex` にして、`correlate` から先を実行する。r は全層で負のままで、逆転しない。
- `stratify` を `island` にする。Biscoe では負のまま、Dream と Torgersen では正になる。
- `admissions` の表で、全体のオッズ比 1.84 が学科で調整すると 0.90 になるのを見る。`applications` の図で、応募先の偏りを見る。

## Draft 版（Agent に実装させる）

`casa-draft`・`gw150914-draft`・`protein-6lu7-draft`・`exoplanet-transit-draft`・`seismic-trench-draft` は、上のデモと同じ計画を Draft だけで持つプロジェクトです。
開始 Card と 3 つの Draft があり、各 Draft の説明には、何を受け取り、何をして、何を渡すかが書かれています（実装済みのデモの Card の説明と同じ文です）。

Canvas の右上の「Card を生成」を押すと、Agent が Draft の説明と前後の接続を読み、コード・出力型・Nix の環境定義を作って保存し、実行して確かめます。
途中で失敗したり取り消したりしても、中途半端な変更は保存されず、計画は残ります。
実際の Claude API で `casa-draft` を実装させた記録は [AgentRun.md](AgentRun.md) にあります。

## 力学系ギャラリー（`dynamics-draft`）

互いに依存しない 6 つの Draft を、Agent が helper に手分けさせて同時に実装するデモです。

```text
start → settings ─ share ─┬→ lorenz      ローレンツ・アトラクタ（RK4、x–z 平面の濃淡）
                          ├→ mandelbrot  マンデルブロ集合（滑らかな色付け）
                          ├→ logistic    ロジスティック写像の分岐図
                          ├→ life        ライフゲーム（トーラスの盤で 200 世代）
                          ├→ grayscott   Gray–Scott の反応拡散の模様
                          └→ pendulum    二重振り子のカオス（初期角を 1e−6 ずらした 2 本）
```

`settings` は画像の大きさ（`size` 480）と乱数の種（`seed` 7）を 6 つの Draft に配るだけで、Draft 同士は線でつながっていません。
各 Draft の説明には、計算の手順と、結果が正しいことを確かめる `assert`（ライフゲームのグライダーが 4 世代で斜めに 1 マス動く、二重振り子のエネルギー誤差が小さい、など）が書かれています。

「Card を生成」を押すと、Agent は 6 つの Draft を helper に分けます。helper ごとに色の違うツグミが自分の Card を同時に書き、実行して確かめます。helper の報告がそろうと、Agent がそれぞれの Card を確かめてまとめて報告します（[手分け](features.md#9-agent)）。

## 試せる操作と、確認できる結果

| デモ | 試せる操作 | 確認できる結果 |
| --- | --- | --- |
| **データからレポートへ**（`report`） | そのまま実行する。`settings` の `seed` や `heading` を変えて再実行する。`aggregate` の `fail_on_purpose` を On にして実行し、Off に戻して `aggregate` だけを単体再実行する。`archive` をダブルクリックして子 Workflow に入り、成果物をプレビュー・書き出す | 同じ `seed` なら常に同じ集計結果になり、`heading` の変更がレポート見出しに反映される。意図的に失敗させると `aggregate` のエラーログに理由が出て下流の `render` は `blocked` になる。単体再実行では保存済み入力を使って即座に成功する。生成されたディレクトリ成果物は環境を初期化しても残る |
| **Collatz (C++ / Python)**（`collatz`） | そのまま実行する。`initial` の初期値や、各言語側の `check` の `maxRuns`（反復上限）を変更して実行する | 初期値 `27` は 111 ステップ（初期値を含め 112 個の値）で `1` に到達し、`compare` が C++ と Python の完全一致を報告する。`maxRuns` を小さくすると反復上限（`execution_limit`）で止まり、`3n+1` が 64 bit 整数を超える初期値では `integer overflow` を検出する。初期値 `2` では奇数側（`triple`）が `skipped` になる |
| **Plots**（`plots`） | そのまま実行する。`settings` の `seed`・`walkers`・`steps` や、`diffusion` の `frames`（アニメーションのコマ数）を変えて再実行する。`explore`・`spread` の表示を拡大してズーム・ホバーする | ウィンドウ向けに書いた matplotlib のコードがそのまま Card の表示に描かれる。`summary` は `plt.show()` で 400 本のランダムウォークのうち 40 本の軌跡と、全体の最終位置の分布（正規分布との比較）を示し、`diffusion` は `FuncAnimation` で広がりが √t で増える様子をライブで再生する。`explore`（Plotly）と `spread`（Altair）は対話できるグラフで、ウォーカーごとの値や平均二乗距離を確かめられる |
| **Cas A · Chandra RGB**（`casa`） | そのまま実行する。`bands` の帯域や `rgb` の `percentile`・`q` を変えて、その Card だけを実行し直す。`load` の `obsid`・`file`・`hash` を ObsID 114（50 ks、520 MB の深い観測）に変える。`spectrum` の範囲や刻み（`min_ev`・`max_ev`・`bin_ev`）を変える | 最初の実行で観測（ObsID 25628、13 MB）を Nix が 1 度だけ取得し、以後はストアのファイルを使う。`bands` が帯域ごとの光子数（赤 101,187 / 緑 157,471 / 青 110,852）を表で示し、`rgb` が Cas A を中心にした 360×360 の RGB 画像を表示する。`spectrum` は同じ光子のエネルギースペクトルを matplotlib で描き、3 帯域を色で塗って Mg・Si・S・Ar・Ca・Fe の輝線に印を付ける（緑の帯域が Si の輝線を含むことが分かる） |
| **科学デモ**（`gw150914`・`protein-6lu7`・`exoplanet-transit`・`seismic-trench`） | そのまま実行する（`load` が公開元からデータを取得するため、ネットワークへのアクセスを許可した Card を含む）。途中の Card のパラメータを変えて、その Card だけを実行し直す | 取得したファイルの大きさと SHA-256 が表示され、重力波のチャープ、タンパク質の結合ポケット、トランジットの折りたたみ、震源断面と b 値が図で表示される |
| **MNIST · 手書き数字の分類器くらべ**（`mnist-classifiers`） | そのまま実行する。`forest` の `max_depth` を 3 にしてその Card だけを実行し、`forest` から先を実行して比べ直す。`knn` の `k`、`cnn` の `epochs`、`prepare` の `train_size` を変える | 7 つの分岐が同じテスト 2000 枚で比べられ、既定では `svm` 0.9705、`cnn` 0.9680、`mlp` 0.9625、`knn` 0.9500、`forest` 0.9475、`boosting` 0.9455、`logistic` 0.9210。`svm` と `cnn` の差は揺らぎの 2 倍に届かないと `compare` が述べる。`max_depth` 3 の `forest` は 0.7570 で、他の分岐は動かない |
| **Newton 法**（`newton`） | そのまま実行する。`orbits` の初期値、`basins` の拡大、`trouble` の `offset` を変えてその Card だけを実行し直す。`settings` の `coeffs`（`[1, 0, -3, 2]` で重根、`[1, 0, -2, 2]` で閉路）や `damping` を変えて全体を実行する | `orbits` の誤差が対数軸で 2 次に落ち、`order` の表で推定次数が理論値と並ぶ。`basins` にフラクタルの境界が描かれ、`roots` で閉じた式との差が表示される。`trouble` では重根で次数 1 に落ち、$z^3 - 2z + 2$ の $z_0 = 0$ が収束しない |
| **薬物動態**（`pharmacokinetics`、架空の薬物の教育用モデル） | そのまま実行し、`simulate` の表示を拡大してズーム・ホバーする。`settings` の `doses_mg`・`intervals_h`・`half_life_h` を変えて全体を実行する。`missed` の `missed_at_h`・`missed_count` を変えてその Card だけを実行し直す | 投与計画ごとの濃度が定常状態へ近づく様子を並べて見られる。`check` で数値解と閉じた式の差と蓄積比が表になり、`window` で山、谷、作り物の有効域の中にいる時間の割合、`missed` で飲み忘れの影響が表示される |
| **最大流と最小カット**（`max-flow`） | そのまま実行する。`whatif` の `edge`・`raise_by`、`sweep` の `raise_by` を変えてその Card だけを実行し直す。`settings` の `seed` を変えて全体を実行する | `flow` と `cut` の図に流量とカットの辺が描かれ、最大流と最小カットの値が一致する。`verify` で scipy と全カットの総当たりも同じ値になる。`whatif` と `sweep` で、カット外の辺を太くしても最大流が変わらないことを確かめられる |
| **濃度–反応曲線**（`dose-response`） | そのまま実行する。`generate` の `noise_sd`・`concentrations`・`top_um` を変えて再実行する。`bootstrap` の `n_boot` を変えて、その Card だけを実行し直す | 架空の化合物 A・B の合成データで、EC50 と Hill 勾配の 95% 区間が真の値を含み、相対力価（真の値 15）が 14.4（11.6 – 18.6）と出る。`top_um` を 1 にすると B の区間が 1000 倍を超え、濃度範囲が EC50 を挟んでいないと Card が警告する。`range` は同じことを最高濃度を変えながら 1 枚の図で示す |
| **分子の物性**（`molecules`） | そのまま実行する。`paste` の `smiles` に SMILES を貼って再実行する。`lipinski` の `max_violations` や `similarity` の `radius` を変えて、その Card だけを実行し直す。`chemspace` の表示を拡大してホバーする | 15 分子の物性の表と構造式が出る。Rule of Five は atorvastatin と cyclosporin A だけが不通過、いちばん似ているのは morphine と codeine（Tanimoto 0.76）。読めない SMILES は落ちずに、理由が表の上に表示される |
| **細胞画像の計数**（`cell-count`） | そのまま実行する。`generate` の `cells`・`noise` を変えて再実行する。`segment` の `min_distance` を変えて、その Card だけを実行し直す | 真の個数 80 に対して閾値のみは 60 個（再現率 0.75）、watershed は 74 個（再現率 0.93）。見つけたものと見逃したものが画像に重なる。`sweep` の表で、混みすぎると見逃し、少ない細胞に強いノイズで数えすぎることが分かる。実際の核の画像では閾値のみ 271 個、watershed 311 個（正解がないので輪郭を目で確かめる） |
| **生存時間解析**（`survival`） | そのまま実行する。`cohort` の `age_min`・`ecog_max`、`km` の `group`、`cox` の `covariates`、`specs` の `subsets`・`adjustments` を変えて再実行する | 女性と男性の生存期間の中央値（426 日と 270 日）、log-rank の χ² = 10.33、Cox のハザード比。`specs` が対象者と調整の 24 通りのハザード比を森林図で並べ、`zph` が比例ハザードの確認を示す |
| **メタ解析: BCG ワクチン**（`meta-analysis`） | そのまま実行する。`pool` の `method`（DL・REML など）と `exclude`（除く試験番号）、`moderator` の `moderator`（`ablat`・`year`）を変えて再実行する | 13 試験の森林図に固定効果（RR 0.65）と変量効果（RR 0.49）の菱形が載り、I² = 92.1%、τ² = 0.309 が出る。`moderator` は緯度の傾きと残差 τ²、`sensitivity` は 1 試験を除いたときと推定量を変えたときの併合値を並べる |
| **RNA-seq: airway**（`rna-seq`） | そのまま実行する（初回は DESeq2・edgeR・airway の Nix ビルドに時間がかかる）。`deseq` の `paired`、`volcano` の `fdr`・`lfc`、`filter` の `min_total` を変えて再実行する | 細胞株の項を入れると FDR < 0.05 の遺伝子が 2713 から 4030 に増える。DUSP1・FKBP5・TSC22D3 などの既知の応答遺伝子が上昇側に現れる。`compare` は DESeq2 と edgeR で一致する 3985 遺伝子を示す |
| **集計と層別で逆転する相関**（`simpson`） | そのまま実行する。`correlate` の `stratify`（species・sex・island）を変えて、`correlate` から先を実行し直す | くちばしの長さと厚さの r は全体で −0.24、種ごとでは 0.39・0.65・0.64。性別では逆転せず、島では一部だけ逆転する。Berkeley の出願は全体のオッズ比 1.84 が学科で調整すると 0.90 になる |
| **Draft 版**（`casa-draft` ほか、名前が `-draft` のもの） | 開いて Canvas の「Card を生成」を押し、Agent に実装させる | 開始 Card と 3 つの Draft（何を受け取り何を渡すかの計画）だけの状態から、Agent が Card を実装し、実行して確かめる |
| **力学系ギャラリー**（`dynamics-draft`） | 開いて「Card を生成」を押す | 互いに依存しない 6 つの Draft（ローレンツ・アトラクタ、マンデルブロ集合、ロジスティック写像、ライフゲーム、Gray–Scott、二重振り子）を、Agent が helper に手分けさせる。helper ごとに色の違う鶫が自分の Card を同時に書き、実行して確かめ、Agent が最後にまとめて報告する |
| **Live Stream**（`live-stream`） | 実行し、途中で停止（Cancel）する。`interval_ms` / `seed` / `samples` や `plot` の表示幅（`window`）を変えて再実行する | 100 ms 間隔のランダムウォークが WebGPU（Dawn / Metal）の折れ線グラフで滑らかにリアルタイム描画される（直近 60 点）。停止を押すと即座に中断し、その時点までのグラフが残る。再実行すると系列がリセットされ、並列度 1 でも 2 でも 200 点が欠落・順序逆転なく描画される |
