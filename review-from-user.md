1. UI 全体として、 visualization や log の card に markdown mode として markdown の renderer を載せたい
   chat の返信の markdown として render して tex で描画できる数式を入れられるようにすること
   collatz 予想の demo では 数式と漸化式を用い説明を書くこと
2. 付箋は font size を変更できるような 設定画面を実装すること
3. 実行ボタンのある bar が長すぎる。 もっと小さくても問題はない
4. window の一番上の部分は window を動かせる領域にしよう。
5. external プロジェクト .... という表示が card in card で出てきて読み取り専用と書かれていますが。
   external プロジェクトとしては取り込む段階にデフォルトで copy です、つまり読み取り専用である必要はないです。
   なぜ、そうするか? それは project 自体がソースの塊でしかなく、 copy を踏んでもそんなに project が肥大化しないからです。
   (特に ソースが全く同じものに対して nix は賢く cache が取られるので build が嵩むこともありません)
   でその変更に合わせて実行ボタンとかの UI は上に
6. wire を 丸 とかから出すことができない、ドラッグすれば出てくるようにするべき
7. wire を選択することができるようにして 切断する時 は delete でできるようにしよう
8. chat の画面はもうちょっと広くていい
9. draft があっても、 card が動かせる状態であれば実行ボタンを押せるようにして実行できるようにしよう
   draft を書きつつ一歩一歩作りたいケースがあるから
10. collatz の demo の compare Run の card の文字が拡大するとぼやける
11. chat で指示して wire とかを繋いでもらっても、それが UI に反映しないので agent が変更すると通知を UI 側に送る仕組みが必要
12. Workflow には反映しましたが保存できませんでした: 別のウインドウまたは Agent が revision 3 を保存しました。変更は保持しています。コピーしてから再読込してください。
    とかなるから chat での変更と UI の変更が同期するようにせよ
13. collatz の demo でも file とかで成果物を保持するようにしてみてください
14. よく既定 って書かれているけど default のほうが日本人にも分かりやすい
15. workflow 設定と tsugumi の設定がやっぱり分からなくなりそう
    統合しちゃったほうがいいかなぁ
16. UI の import のボタンが効かない、 external なプロジェクトを取り込む時の ボタンだよね?
17. チャットのボタンで icon チャット って書いてあるけど icon だけでいい
18. 付箋の文字も拡大するとぼやける
19. 拡大した状態から、 home 状態に戻すと input の card の input の文字が input box からずれて描画される
20. Card を追加では tsumugi が提供している、入力値とか Draft とか 開始とか 分岐とかの card を並べる必要はない。
21. Card を追加で collatz 予想なんですが collatz-cpp とかの card も見れるようになっていると良い (で import 時に自分の definitions に追加される)
    external の中の definitions は card in card で中に入っている時に編集可能になる)

## Draft について
1. 名前 概要 内容 の 3 つの input だけでいい (仮の入出力はもちろん必要だが)
   その変わり内容について、入力をどうするか 出力をどうするか とか user が書きやすいような template が出てくるようにするといい
2. text input に scroll bar がデフォルトであるのは変、 もっと角が丸い text input に入れることができて、内容が長くなってはじめて scroll bar が出てくるようにすること
3. text input の文字が枠に見切れている mergin の設定を調整するべき
4. 仮の入力 仮の出力 の追加をした時にどれが text input かわからないので背景色を他 text input のとそろえる (他のところで揃ってないところがあれば統一感のためにそろえる)
5. 仮の入力 出力 用途説明とか name とかは 薄い文字で placeholder で入れておくといいよね
