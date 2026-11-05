# 同梱デモのしくみ

アプリに同梱しているデモが、どんな Card をどうつなぎ、実行すると何が起きるのかをまとめます。
ホーム画面の「デモを見る」から「コピーを開く」を選ぶと、`~/Documents/Tsugumi/<demo>` に編集できるコピーができます（同梱の原本は変わりません）。
画面の操作は[操作ガイド](usage.md)、Card の種類と実行の規則は[主要機能](features.md#2-card-と-workflow)を参照してください。

| デモ | 題材 | このデモで分かること |
| --- | --- | --- |
| [データからレポートへ](#データからレポートへreport) | 売上表の集計とレポート | seed による再現、失敗と `blocked`、保存入力での単体再実行、成果物、子 Workflow |
| [Collatz (C++ / Python)](#collatz-c--pythoncollatz) | コラッツ数列 | 2 言語での独立実装の照合、分岐・合流・ループ、`maxRuns`、整数あふれの検出 |
| [Live Stream](#live-streamlive-stream) | ランダムウォークの逐次描画 | フィードバックループによるストリーム、即時の停止、Dawn の折れ線 |
| [Plots](#plotsplots) | ランダムウォークの拡散 | ふつうの matplotlib（`plt.show()`・`FuncAnimation`）がそのまま Card に描かれ、Plotly と Altair は対話できるグラフになる |
| [Cas A · Chandra RGB](#cas-a--chandra-rgbcasa) | 超新星残骸の X 線観測 | Nix で固定した観測データ、Card 単位の再実行、版の履歴 |
| [科学デモ](#科学デモ公開データの取得と解析) | 重力波・タンパク質・系外惑星・地震 | 公開データの取得（大きさと SHA-256）と解析・図示 |
| [MNIST · 手書き数字の分類器くらべ](#mnist--手書き数字の分類器くらべmnist-classifiers) | 手書き数字の分類（kNN から CNN まで 7 種） | 1 つの入力から 7 つの分岐が別々の Nix 環境で同時に動いて合流し、1 つの分岐だけを条件を変えて実行し直せる |
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
- 版のツリーを開く。帯域や格子・伸長を試した過程（失敗した枝、採用しなかった枝、採用した版 `rgb-v1`、その上にスペクトルを足した版）が入っている（[版の管理](versions.md#デモの履歴)）。

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
- 比べ直すときは `forest` から先を実行する（「ここから実行」）。動くのは `forest` と `compare` から先だけで、`compare` は新しい `forest` の結果と、他の分岐・`share` の保存された入力で比べます。変えていない分岐は動かないので、その正解率は同じです。
- `knn` の `k`、`svm` の `gamma`、`cnn` の `epochs` を変える。
- `prepare` の `train_size` を増やして、どの分類器が枚数の恩恵を受け、どれの時間が伸びるかを見る（`svm` と `knn` の予測時間が伸びます）。

データは LeCun・Cortes・Burges の MNIST で、PyTorch のミラー `https://ossci-datasets.s3.amazonaws.com/mnist/` から取得します。

## Draft 版（Agent に実装させる）

`casa-draft`・`gw150914-draft`・`protein-6lu7-draft`・`exoplanet-transit-draft`・`seismic-trench-draft` は、上のデモと同じ計画を Draft だけで持つプロジェクトです。
開始 Card と 3 つの Draft があり、各 Draft の説明には、何を受け取り、何をして、何を渡すかが書かれています（実装済みのデモの Card の説明と同じ文です）。

Canvas の右上の「Card を生成」を押すと、Agent が Draft の説明と前後の接続を読み、コード・出力型・Nix の環境定義を作って保存し、実行して確かめます。
途中で失敗したり取り消したりしても、中途半端な変更は保存されず、計画は残ります。
実際の Claude API で `casa-draft` を実装させた記録は [AgentRun.md](agentrun.md) にあります。

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
| **Draft 版**（`casa-draft` ほか、名前が `-draft` のもの） | 開いて Canvas の「Card を生成」を押し、Agent に実装させる | 開始 Card と 3 つの Draft（何を受け取り何を渡すかの計画）だけの状態から、Agent が Card を実装し、実行して確かめる |
| **力学系ギャラリー**（`dynamics-draft`） | 開いて「Card を生成」を押す | 互いに依存しない 6 つの Draft（ローレンツ・アトラクタ、マンデルブロ集合、ロジスティック写像、ライフゲーム、Gray–Scott、二重振り子）を、Agent が helper に手分けさせる。helper ごとに色の違う鶫が自分の Card を同時に書き、実行して確かめ、Agent が最後にまとめて報告する |
| **Live Stream**（`live-stream`） | 実行し、途中で停止（Cancel）する。`interval_ms` / `seed` / `samples` や `plot` の表示幅（`window`）を変えて再実行する | 100 ms 間隔のランダムウォークが WebGPU（Dawn / Metal）の折れ線グラフで滑らかにリアルタイム描画される（直近 60 点）。停止を押すと即座に中断し、その時点までのグラフが残る。再実行すると系列がリセットされ、並列度 1 でも 2 でも 200 点が欠落・順序逆転なく描画される |
