Home・画面操作

1. Home を小さくすると上が切れるので、window のサイズに追従するようにすること。最近の project が多い場合は一覧を scroll できるようにし、下に空の交互色の行を並べない。他の画面も、小さい window や文字量が多い状態で操作できる layout にする。

2. Home の左側に Tsugumi の鳥と key visual の色を取り入れ、背景・sidebar と統一感を出すこと。アプリアイコンの枠にする必要はなく、鳥そのものを使ってよい。

3. demo 一覧には、何ができるか分かる thumbnail と tag を付けること。今後 demo が増えても探しやすく、見た目から触ってみたくなる一覧にする。

4. Canvas のマウスホイールは、pointer の位置を中心とした拡大縮小にすること。マウスでは中ボタン drag または Space + drag で移動する。trackpad は二本指 scroll で移動、pinch で拡大縮小とし、通常の一覧や editor の scroll はそのまま使えるようにする。

5. よく使う操作に shortcut を用意すること。保存は ⌘S、Undo / Redo は ⌘Z / ⇧⌘Z、検索は ⌘F、設定は ⌘,、実行は ⌘Return、実行停止は ⇧⌘Return、選択解除・操作取消は Esc を基本にする。Canvas の全体表示・100% 表示・zoom にも割り当てる。editor や chat に focus があるときは文字入力を優先し、menu と tooltip に shortcut を表示する。

6. 下部の「実行: 成功」「保存済み」などは、押せる項目と単なる状態表示を見分けられるようにすること。押せるものには button と分かる背景・hover 表示を付ける。ログ・timeline は各 icon で開き、同じ icon を再度押すと閉じる。「閉じる」を独立した選択肢にしない。

7. timeline は drag した時間範囲の拡大、時間方向の移動、全体表示への復帰ができるようにすること。短い処理が大量に並んでも、対象の Card・実行回数・phase を追えるようにする。

8. UI 内に terminal を用意すること。現在の project の実行環境内で使え、対象 project と作業ディレクトリが分かるようにする。初期化ログを見る console と、コマンドを入力する terminal は区別する。

実行環境・速度

9. 実行環境は 1 app 1 VM とし、複数 project で同じ /nix/store を共有すること。project ごとに VM と store を増やさない。作業領域・履歴・成果物・実行権限は project ごとに分け、ある project の停止で別の project の実行を止めない。

10. Virtualization.framework を直接使い、初回起動時に共通 VM の image と保存領域を初期化すること。準備中は Tsugumi の鳥を使った loading と現在の段階を表示し、image の構築・展開・起動のログを console で追えるようにする。何をしているか確認できるログを残し、失敗時も原因を読めるようにする。

11. Card の実行が UI の更新待ちで遅くなっていないか調べること。core は描画完了を待たずに進み、UI は通知をまとめて反映する。shared library を読み込んだ worker の再利用を基本にし、毎回 process 起動・SDK 準備・build を繰り返さない。新しい worker が必要な場合も、起動を軽くする。遅さの原因はまだ断定しない。

Card 設定・editor

12. Card 設定の window を拡大縮小できるようにすること。広げた分だけ editor や一覧も広がり、小さくした場合は必要な部分が scroll できるようにする。

13. output.fbs などを選んだときに落ちる問題を優先して直すこと。提供されたログでは CodeEditor.updateNSView → NSTextView.setSelectedRanges → NSSelectionArray で例外が発生している。本文を差し替えた後、旧選択範囲を filter した結果が空になり、そのまま selectedRanges へ設定される処理が原因候補。新しい本文に収まる UTF-16 の範囲へ補正し、有効な範囲がなければ長さ 0 の cursor 位置を設定する。ファイルごとの cursor 状態も混同しない。FBS の構文エラーで落ちたと決めつけない。

14. ソースを保存すると editor からコードが消え、開き直すと Card 設定が混ざって表示が壊れる問題を直すこと。保存の前後で選択中のファイル・本文・編集中の状態を維持し、保存結果と再読込の更新で別の画面や空の本文へ置き換わらないようにする。

15. Card の preview は「一般」のときだけ表示すること。ソース・Nix 定義・入出力・履歴では作業領域を広く使う。一般の preview は中央に配置し、window のサイズが変わっても自然に収める。前回の実操作では左に寄っていた。

16. Nix 定義とソースの editor は、default.nix などのファイル名・現在開いている場所を分かりやすい header に表示すること。Tree-sitter を UI 側の依存として含め、Nix・C++・Python・FBS などの syntax highlight に使う。Card ごとの SDK や実行環境の build に混ぜない。

17. editor 左側の行番号は本文と別の落ち着いた色にし、番号欄とコードを区別できるようにすること。

18. ソース tree は VS Code のように、folder の階層・展開状態・選択中のファイルが分かる表示にすること。editor のファイル名表示も揃える。複数ファイルを tab で開ける程度の機能は欲しいが、大きな IDE を作り込む必要はない。

SDK・Nix・入出力

19. C++ の利用者に _inputs_generated.h / _outputs_generated.h を直接 include させないこと。公開する入口は #include <card.hh> とし、auto run(Card::Input& input) -> Card::Output; と書ける API にする。複数入力でも codegen により input.<ポート名> で参照でき、出力も名前付きの型として扱えるようにする。実行 Context が必要な処理は別途整理し、通常の Card に内部の受け渡し形式を意識させない。

20. Nix では inputs.tsugumi.sdk { inputs = ...; ... } のように入出力契約を渡して SDK を作り、その SDK を cppCard などへ渡す構成にすること。visualization は tsugumi::visualization から呼べるようにする。ここに書いた API 名は目指す利用形であり、すでに存在する API の説明ではない。

21. visualization や runtime など Card 固有の codegen に依存しない部分は共通の shared library にし、SDK ごとに作り直さないこと。Card 固有の型生成・薄い adapter と共通部分を分け、SDK の build 時間を抑える。

22. SDK を作るときに対象言語を明示できるようにすること。C++ だけ必要なら C++ だけを生成・準備し、将来 Rust / Go などを増やしても、使わない言語まで build しない。

23. Nix 定義で実行する shared object のパスを明示すること。cppCard は SDK を使って shared object を含む package を作る役割に絞り、runtime が指定された .so を読み込む。例えば ${package}/lib/main.so のように、package と実行対象の関係が分かる定義にする。

24. preRun / postRun を用意し、実行前の一時ファイル準備や bash 処理、実行後の処理を設定できるようにすること。各 invocation の作業領域・入出力と結び付け、worker を再利用しても必要な前後処理が抜けないようにする。失敗時・停止時に postRun を実行するかも明確にする。

25. 入出力設定の入力 port にある「省略可」の選択肢はなくすこと。name は編集可能な text field と分かる背景・枠・focus 表示にする。UI の項目削除と、core が既存の任意入力やループを扱う契約の変更は区別する。

26. Card 定義の import は基本的に copy とすること。コピー後はその project の Card として編集でき、「定義に従う」などの継承状態を選ばせない。コピー時点の設定値を具体的な値として持たせ、どの値が使われているか分かるようにする。

27. 「成果物を保持」では、保持できる出力 port を 1 つ選べるようにすること。対象となる File / Directory の port を名前付きで表示し、選べる出力がなければその理由を示す。

28. 分岐 Card は value と condition を明確に区別し、どちらが boolean の入力か見た目で分かるようにすること。condition に bool の表示を付け、出力も true / false と a / b の対応が分かるようにする。

実行履歴・chat

29. 実行履歴・成果物が空のときは、壊れた設定画面に見えないようにすること。「まだ実行していません」「保持された成果物はありません」など、空である理由を短く表示する。読み込み中・取得失敗とも区別する。

30. 実行ログは stdout / stderr / journal を別々に選ばせず、GitHub Actions のように phase ごとに折りたためる表示にすること。SDK 準備・評価・build・実行・保存のどこで何が起きたか、状態とログをまとめて追えるようにする。元の stream 情報が必要なら詳細に残す。

31. chat 左下の $ は削除すること。scroll bar は本文が領域を超えたときだけ表示し、空の会話や短い入力に常時出さない。

32. API key と認証情報の保存方法を確認し、安全に保管すること。秘密情報は Keychain 等へ保存し、project ファイル・通常設定・ログに平文で残さない。Anthropic に加え、OpenAI API key と ChatGPT アカウントによる外部アプリ認証を選べる構成にする。アカウント認証は公式に利用可能な認証方式・利用条件を確認して実装し、API key 接続と混同させない。現時点で保存が安全だと確認済みではない。

前回の実操作で確認した点

33. 120% 表示で wire とポートの位置がずれ、100% に戻すと一致した。Card 本体、ポート、wire、hit test の座標変換を統一すること。小さい文字も拡大縮小でぼやけて見えるので、描画倍率と配置を合わせて修正する。

34. Collatz のループの戻り線が途中の Card を横切っていた。戻り線は左右の外周へ逃がし、Card・文字・表示枠を避けること。100% でも発生するため、端点のずれとは別に直す必要がある。

35. Card 設定で「ソース」「Nix 定義」を押しても一般設定のまま、アプリ設定でも「外観」から「Skills」へ切り替わらなかった。sidebar の選択と内容表示を連動させること。Workflow 設定の「一般 → データベース接続先」は切り替わったため、画面ごとに切り分ける。

36. 完了した段階が全部緑の check に置き換わり、何の段階か分からなかった。各 phase の icon を残して状態を色で表し、現在の phase は文字でも出す。実行回数は #112 などで表示されていたが、1 回目も省略せず、どの実行の結果か分かるようにする。

37. SDK 準備中で、まだ出力がない Card の先の wire まで動いていた。Card が running / succeeded というだけで全 outgoing wire を光らせず、実際に値が流れた port・接続に対応させること。

38. 「全体表示」の初期状態でも visualization の枠が右側に切れていた。Card だけでなく表示枠・付箋・戻り線の範囲も含め、sidebar・chat・下部 panel を除いた領域に収めること。表示枠と元 Card の関係も専用色の線で示す。

39. cpp / python など複数ポートの名前と丸の位置がずれていた。名前をポートの位置に揃え、多い場合は省略し、hover で完全な名前と型を読めるようにすること。

40. 子 Workflow の Canvas だけでは、どのポートが親の input / output につながるか分かりにくかった。上端に公開入力、下端に公開出力を置き、内部の接続先まで追えるようにすること。

41. 実行成功後も完了 bar が 18 / 20 のまま残った。この bar は削除し、状態・phase・経過時間で状況を示すこと。また実行中の「1 分 39 秒」が成功後に「26.3 秒」になり、timeline は「48.9 秒」だった。時計の基準と計測区間を揃え、時間の表示が巻き戻らないようにする。
