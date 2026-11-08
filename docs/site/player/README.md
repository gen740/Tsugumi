# デモプレイヤー

インストールなしでデモの動きを見せる、文書サイト用のプレイヤーです。
Card・線・幾何は Web UI のコード（`web/src/ui/cardNode.tsx`、`web/src/model/canvasKit.ts`）をそのまま使い、CSS は `web/src/styles/canvas.css` と `web/src/styles.css` の色を `.tp-root` の中に閉じて使います。
ブラウザの中では何も計算しません。実際に動かした実行の記録を再生するだけです。

```js
import { mountPlayer } from './site/js/player.js'
const player = mountPlayer(el, { demo: 'report', data: 'site/player-data/report.json' })
player.destroy()
```

- `data` は JSON への相対 URL で、画像はこの JSON からの相対で読みます。`site/player.css` はページになければ自動で足します。
- `compact: true` はギャラリー用の縮小表示です。枠に合わせて全体を描き、画面に入ったときとホバーで再生します（`autoplay: false` でホバーだけ）。`el` に高さを与えてください。
- 通常の表示は `autoplay: true` で画面に入ったときに一度再生します。既定では ▶ 実行 を押すまで動きません。
- `prefers-reduced-motion` のときは自動で再生しません（`compact` は実行の終わりの状態を静止して見せます）。
- 高さは `--tp-height` で変えられます。

## ファイル

| ファイル | 中身 |
| --- | --- |
| `web/src/ui/demoPlayer.tsx` | プレイヤー本体。Web UI の Home のサムネイルも同じものです |
| `web/src/styles/demoPlayer.css` | プレイヤー独自の CSS |
| `narration.json` | 実行の各段階の説明文（[Demos.md](../../Demos.md) から手で書いたもの） |
| `build.mjs` | `site/js/player.js`、`site/player.css`、`site/player-data/<id>.json`、`index.json` を生成する（`--check` は調べるだけ） |
| `harvest.py` | デモを実際に実行して `site/player-data/assets/` に記録を書く |
| `svg.mjs` | 再生を動く SVG にする（Cas A の大きいもの `docs/assets/casa-demo.svg` と、各デモのタイル `docs/assets/demos/<id>.svg`）。README のように画像しか置けない場所のため |
| `demo.html` | 開発用のページ（全デモのサムネイルと通常表示） |

## 作り直す

Card やデモを変えたら、まず `web/src/gen` を用意して（`web/` で `nix develop --command npm run gen`）、次を実行します。

```sh
node docs/site/player/build.mjs
```

作り直し忘れは `node docs/site/player/build.mjs --check`（何も書かず、`player.js`・`player.css`・`player-data/` が今の入力と違えば失敗します）で分かります。CI の Web のステップが実行します。

実行の記録は別の手順です。`tsugumid` のビルドと Nix が要ります（初回は依存の取得に数分かかります）。

```sh
# 既定は <repo>/build。別の場所のビルドなら TSUGUMI_BUILD_DIR で指定する
# 隔離（Seatbelt）のまま、Draft 以外の全デモを実行する
nix develop --command python3 docs/site/player/harvest.py --ports \
  report collatz live-stream plots casa card-guide \
  gw150914 protein-6lu7 exoplanet-transit seismic-trench \
  mnist-classifiers newton pharmacokinetics max-flow dose-response molecules cell-count \
  survival meta-analysis rna-seq simpson battery gillespie heat-inverse ecology-models
node docs/site/player/build.mjs
```

## 動く SVG

GitHub の README のように、スクリプトが動かず画像だけを置ける場所のために、再生を SVG にします。`player-data/` を作り直してから、Mac で実行します（タイルの画像を `sips` で縮めます）。

```sh
node docs/site/player/svg.mjs            # 記録のある全デモ。ID を並べればそれだけ
```

- `docs/assets/casa-demo.svg`: Cas A を、通常の表示（実行ボタンと経過時間、説明文、メモ）のまま大きく。
- `docs/assets/demos/<id>.svg`: 記録のある各デモを、ギャラリーの縮小表示（`compact`）のまま 440 × 264 のタイルに。小さい Canvas で CSS が隠す文字（`.tp-small` など）と Card の説明の行の中身は省き、画像は長辺 200 px にします。

再生の各段階（`framesOf`）を、プレイヤーの `Canvas`（と `RunControl`）が描く HTML のまま `<foreignObject>` に入れ、`site/player.css` のうち使う規則だけを入れます。同じ描画は 1 回だけ持ち（Card は形・本体・ポートに分けて）、それが出ている段階でだけ CSS のアニメーションで見せます。アイコンは `<defs>` に 1 回だけ置きます。ループし、`prefers-reduced-motion` のときは実行の終わりの状態で止まります。画像は data: URI で埋め込むので、外のファイルもフォントも読みません。

プレイヤーと違う点は、Card を選んで中身を読めないこと、拡大縮小のボタンがないこと、出力の枠のフェードインが最初の 1 周だけなことです。中身を読むには、サイトの `#demo-<id>`（例: `https://gen740.github.io/Tsugumi/#demo-casa`）でそのデモのプレイヤーが開きます。

## 記録の中身

`player-data/assets/<id>.run.json` の `run` に、どの実行の記録か（Run ID、日付、ホスト、隔離、コミット）があります。
再生の順序と時間は `RunInvocationsRequest`、表示は `RunRecordRequest` の値そのものです。時間の重なる Card は同時に動いたものとして 1 つの段階で再生します。再生の速さは実際の時間に比例しません。

手を加えているのは次の点だけです。

- 画像は長辺 640 px に縮め、220 KB を超える PNG は JPEG にしています。
- 表は先頭 24 行、文章は 4000 文字、折れ線と散布図は 1 系列 400 点に間引いています（点数の多いものだけ）。
- Plotly・Altair・Dawn の図は、ライブラリを読み込まず、同じ値を簡単な SVG で描きます（アプリのようなズームやホバーはありません）。パネルが分かれた Vega-Lite の図（concat や facet）は、記録するときに Vega で描いて画像にします（`vega-svg.mjs`）。3D 点群は点の数だけを示します。
- `ports.json` はコアが解決したポートです（実行はしません）。

記録のないデモは、線のつながりから順序を作って再生し、結果を出さず、その旨をページに表示します。Draft は実行できません。

