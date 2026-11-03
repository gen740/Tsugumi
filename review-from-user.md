1. Home の Tsugumi は動かせない設定とかできるようにするといいかも。(mac の アニメーションを減らす設定と既定値は同期させる)
2. Home の最近のプロジェクトは表示から消せるようにする (check box があって全選択から削除も可能にしておくといい)
3. chat の動きをもう少しスムーズに追従するようにしたい (フェードインしながら文字が表われるかんじ)
4. 出力中に text input に文字を打とうとすると薄い文字(メッセージ (Return で送信)) と IME の途中が被って表示されてしまう
5. メッセージの claude-sonnet-5.5 という 選択のところ select box 感が出すぎているので chat の UI に統合して自然な形で text の box の下に claude と似せてやっちゃえばいいよ
6. 会話一覧でも 会話を圧縮とかの指示が選べてしまうのを修正
7. メッセージを要約で (全文の履歴は残っています) という表示は要らない
8. 実行中の 実行ボタン (canvas の上中央のやつ) ががたがたちらつくからそれを修正高速の実行時にもわかりやすくしたい
9. 実行の横の V の表示押せるように見えるけど押せなくなっている。そもそもそういう記号を無くす必要がある
10. 会話の中に folder を添付できる (VM から bind してそれだけ見れるようにする) 機能を作りなさい、その folder を up する時は注意を出すようにしなさい
11. メインのUI ですが window の title bar を目立たせない作り (上まで canvas が広がっており、実行ボタンと同じような感じの UI に chat ボタンとか title の表示がある、現状の < Collatz (C++/python) > Collatz in C++ 
    とかと title を統合すると良いと思う
12. card の表面の sdk / nix / build / run / validate / store ... みたいに読めるアイコンが量が多すぎる
    build -> run -> validate の 3 step でいいんじゃない?表示は で →とかで step のことを言っているんだよって分かるように
13. 設定に パス とか 名前 とか 定義 とかあるけど ようは ID なので name とかだけで良くない?
14. アイコンを選ぶから pick しようとすると、picker が見切れてしまっている
15. 「定義の既定値」というのが残っている 1 card ごとに copy されているはずだからそういうものはないはずだよ?
16. card 設定 > 一般 で 入力の揃え方 というより trigger という名前で設定できると良い
17. 表示の出力表示を Canvas に出す。は無くてよくて canvas の sdk でコントロール可能にするほうが良い
18. canvas で折り疊むというもの 一般の設定に無くていい
19. /private/var/piagent/work/screenshot/Screenshot\ 2026-11-03\ 13.29.32.png nix 定義のtitel bar 付近、
    number column の線が被ってしまっているのとここももっとシームレスな UI にしてしまおう、カード定義として
20. card in card とかだけど
definitions とかにまとめず
my-project/
  tsugumi.json
  definitions/
    plot/                  ← 自分の定義
    collatz-step/          ← externals の定義へのリンク
      _link.json           {"external": "collatz", "definition": "step"}
  externals/
    collatz/               ← 単体でも開ける Tsugumi プロジェクト
      tsugumi.json
      definitions/...
      externals/...        ← 中にさらに置いてもよい
 のような構造の中の definitions とかも再帰的に見れるようにしておいて
 card の definitions の中にはそれ用の link みたいなものにしよう。 demos の collatz もそういう風にしてほしい
21. ソース tab の editor にも nix 定義と同じく number columnの線が window の上部まで行っている
22. ソースの Readonly のファイルを見ると (編集しません) って書いてあるけど RO とか x とかの簡単な記号でいい
23. 入出力の設定だけど、 _output.fbs で作った型を pulldown で見れるようにするといいね
    (core の側じゃなく UI の側で fbs を bfbs にする指令とかだけを core に送って、 core から来た bfbs を parse して取るとか)
    そうして File とか Directory とかの型を選べるようにしよう
24. 入力ポートの真ん中の textbox は何を使うの..?
25. 記録と成果物の「ログを保存する DB 接続」 はなくていいかなぁ、ただ worker ごとに副作用を持たせるかどうかを選べるようにしたいから。
    一般の設定であるといい。
    experimental という section を作ってそこに、 network のアクセスを許可する。
    host への folder への アクセスを許可する (folder の pickup) の UI を作って実装して
26. 実行履歴だけで履歴の一覧で動いた履歴を押すと右の表示に反映されるようにして
27. visualization の canvas をドラッグ する時すごく遅い、移動を速くするようにしないと
28. visualization とか output の card への線の付けかただけど線と card とか visualization の◯ の port の位置が対応していない。
    最短の位置から 線を出すようにして port はその線の位置に従って card の端に表示されるようにしよう
39. card を hover した時に出てくる説明の位置がもっと cursor に近いところに出ていい
30. Tsugumi 設定の外観として title bar の書きかたを他の UI と合わせてシームレスに
31. 一般設定で言語の説明とか 並列実行数とかの説明とかあるけど hover で説明が表示されるようにしよう
32. 流れの向きが設定できるのはすごく良い。ただ、 layout が崩れます という注意書きがあっても良い
33. エディター設定 number column が設定画面にはみだしている



## ショートカット
以下のようなショートカットを定義しよう Command + enter で実行はあんまり直感的ではない
**アプリ・プロジェクト**

| 操作 | キー |
|---|---|
| 新しいプロジェクト | `⌘N` |
| プロジェクトを開く | `⌘O` |
| Homeへ戻る | `⌘⇧H` |
| 保存 | `⌘S` |
| プロジェクトを複製 | `⌘⇧S` |
| 取り消す | `⌘Z` |
| やり直す | `⌘⇧Z` |
| テキスト検索 | `⌘F` |
| ワークフローを実行 | `⌘R` |
| 実行を停止 | `⌘.` |
| アプリ設定 | `⌘,` |

キャンバスにフォーカスがあるとき：

| 操作 | キー |
|---|---|
| Card設定を開く | `S` |
| 成果物を開く | `A` |
| 実行ログを開く | `L` |
| 折りたたみを切り替える | `F` |
| Cardを複製 | `C` |
| 選択対象を削除 | `⌫` / `Forward Delete` |
| 全体表示 | `0` |
| 100%で表示 | `1` |
| 拡大 | `⇧=` |
| 縮小 | `-` |
| 整列 | `⇧A` |
| グループ操作 | `G` |
| Draft Cardを追加 | `E` |
| DraftからCardを生成 | `⇧D` |

チャット・ソース編集：

| 操作 | キー |
|---|---|
| チャット送信 | `⌘Enter`（標準設定では `Enter` も可） |
| チャットで改行 | `⇧Enter` |
| Skill候補を移動 | `↑` / `↓` |
| Skill候補を確定 | `Enter` / `Tab` |
| Skill候補を閉じる | `Esc` |
| ソースコードを整形 | `⌥⇧F` |
