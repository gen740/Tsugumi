# 公開・コードフリーズまでにやること

2026-11-04 時点のコードベースのレビュー（core / macOS・Agent / Web）から。上から順に進め、各修正には回帰テストを 1 つ添える。

## 判断（2026-11-04 に決定）

- 2・6: Agent が Card の権限（`network`・`sandbox`・`hostFolders`）を広げる保存は、利用者に確認する。確認せずに許す範囲（Card の既定の権限）は利用者が設定でき、確認の画面からも変えられる。Card には持っている権限をアイコンで示す。ホスト上で動かす構成の `sandbox: false` も同じく利用者が承知の上で許可する（tsugumid は判断しない。Linux 向けには隔離されたコンテナで動かすことを勧める）。
- 24: Developer ID は用意しない。INSTALL.md を「ソースからビルドする」前提にする。
- 26: 英語 UI を完成させる。

## A. 隔離の境界を、文書の約束どおりにする

1. **`.tsugumi/` から読み込む値を信用しない**
   - journal を再適用するときに、`stage()` と同じ `valid_relative_path` の検査をかける（[store.cc:246](core/src/store.cc)）。今は絶対パスを入れれば任意のファイルを書ける。
   - 履歴を取り込むときと使うときの両方で、`run_id` と `artifact_id` が UUID の形かを確かめ、成果物のファイルパスも検査する（[history.cc:1046](core/src/history.cc)、[service.cc:1011](core/src/service.cc)、[service_run.cc:504-513](core/src/service_run.cc)）。今は `run_id = "../.."` で、「実行履歴を削除」がプロジェクト外まで消す。
   - 取り込む SQLite は defensive モードと `trusted_schema=OFF` で開く。
2. **Agent が自分で権限を広げられないようにする**
   - Agent からの保存で `network`・`sandbox`・`hostFolders` を変えようとしたら、core で拒否するか、UI で利用者に確認する。
   - system prompt から、Agent が設定してよい項目としての `network` と `hostFolders` を外す（[system-prompt.md:16](agent/Shared/system-prompt.md)）。
3. **Card の出力を読むときに symlink 競合を避ける**
   - ディレクトリの fd を基準に `O_NOFOLLOW` で開き、`fstat` で確かめてから、その fd から複写する（[scheduler.cc:843-948](core/src/scheduler.cc)）。Directory の中身も一つずつ同じように扱う。
   - Seatbelt の書き込み許可を、その実行の `out/` だけに絞る（[worker_proc.cc:133](core/src/worker_proc.cc)）。
4. **Web サーバーに認証を入れる**（[main.ts](web/server/main.ts)）
   - 起動のたびに乱数の秘密を作り、GET と `/api/events` を含むすべての `/api/*` で要求する。今は `GET /api/env/list` が tsugumid のトークンを返し、`POST /api/terminal/start` は固定ヘッダ `X-Tsugumi: 1` だけで端末を起動できる。
   - リクエスト処理全体を try/catch で包む。今は `/%E0` のような URL 一つでサーバーが落ちる。
   - 静的ファイルの範囲検査を、区切り文字まで含めて比べる（main.ts:447）。
   - CSP、`frame-ancestors`、`nosniff` を付ける。
   - Seatbelt でネットワークを許可するときも、loopback は拒否する（[worker_proc.cc:141](core/src/worker_proc.cc)）。
5. **ホスト上で動かす構成でも、許可したフォルダしか読めないようにする**
   - `AttachedFolderRead` が読めるフォルダを core 側で管理する。import grants と同じ方式で、control フォルダに一覧を置く（[service.cc:1378](core/src/service.cc)）。
6. **ホスト上で動かす構成の `sandbox: false` を扱う**
   - 拒否するか確認を求めるかを決め、[WebUI.md](docs/WebUI.md) と [security.md](docs/security.md) に書く。
7. **プロジェクトを閉じたら環境を片付ける**
   - その tsugumid を止め、フォルダの共有を外し、sessions から取り除く（[Workspace.swift:134](macOS/Sources/App/Model/Workspace.swift)、[AppModel.swift:136](macOS/Sources/App/Model/AppModel.swift)）。security.md:51 の記述と一致させる。
   - 今は一回のアプリ起動中に 16 個のプロジェクトを開くと、それ以上開けなくなる。
8. **VM の中でプロジェクト同士を分ける**
   - プロジェクトごとにネットワーク名前空間を分けるか、ポートを推測できないものにする（[vmd_main.cc:412-454](core/src/vmd_main.cc)）。
   - ビルド段階で使える固定出力 derivation の通信を、拒否するか文書に明記する。
9. **root で動く vmd の後始末**
   - ログを `O_NOFOLLOW` で開く（[vmd_main.cc:443](core/src/vmd_main.cc)）。
   - pty を close-on-exec にする。
   - `clone()` の子プロセスで行うメモリ確保を減らす。
   - `proc::spawn` は、1024 番以上の fd も閉じる（[process.cc:52](core/src/util/process.cc)）。
10. **VM のカーネルを更新して堅くする**
    - 6.12.28 から 6.12 系の最新に上げる。
    - 非特権 BPF を既定で無効にし、`io_uring` を無効にするか制限し、sysctl を見直す。
    - [kernel.nix](macOS/Container/kernel.nix) に残る Apple container 時代のコメントを直す。
11. **認証前の受信量に上限を付ける**
    - tsugumid の `rbuf` に上限を設ける（[server.cc](core/src/server.cc) の `on_readable`）。
    - Web の socket フォルダは、既にあるときに所有者とモードを検査する（[env.ts:69](web/server/env.ts)）。

## B. 正しさと安定性

12. **VM の起動に失敗したら VM を止める**
    - 失敗時には VM を停止して `vm` を片付ける（[SharedVM.swift:271, 427](macOS/Sources/Launcher/SharedVM.swift)）。今は再試行を二回押すと、同じ store と履歴のディスクに二台目の VM が起動する。
    - `stopped()` は、どの VM が止まったのかを照合する。
    - `bootCount` と `hostLock` はロックの内側で扱う。
13. **Agent の取消と次のターンの競合をなくす**
    - 会話を切り替える前に、古い Agent を取り消して終わるのを待つ（[ChatController.swift:213](macOS/Sources/App/Chat/ChatController.swift)）。今は新しい Agent を取り消し、古い方が動き続ける。
    - 取り消した後は、tool の結果を会話に追加しない（Swift: [Agent.swift:257](agent/Sources/TsugumiAgent/Agent.swift)、Web: [agent.ts:607](web/server/agent.ts)）。
    - 新しいターンは、前のターンが終わってから始める。Web の `run_status` の待機は、そのターン自身の signal を見る。
    - 再試行のとき、途中まで受け取った文が二重に表示されないようにする（Agent.swift:291-329）。
14. **`Failed` 以外の例外でも操作を必ず終わらせる**
    - 変更を伴う handler すべてで `std::exception` も受け止め、`finish_op` を呼ぶ（[service.cc:837, 869, 960, 985](core/src/service.cc)、[service_run.cc:88, 117](core/src/service_run.cc)）。
    - `nix.cc:595` の `directory_iterator` を、error_code を受け取る形にする。
15. **実行の終わり際の取消で起きる use-after-free を直す**
    - `submit` の callback は、生の `this` ではなく `shared_from_this()` か `weak_ptr` を保持する（[scheduler.cc:278-348](core/src/scheduler.cc)）。
16. **SIGPIPE で落ちないようにする**
    - プロセスで SIGPIPE を無視するか、socket に `SO_NOSIGPIPE` を設定する（[SharedVM.swift:580, 684](macOS/Sources/Launcher/SharedVM.swift)、[ProjectTerminal.swift:130](macOS/Sources/App/Workbench/ProjectTerminal.swift)）。
17. **再接続を一本化する**
    - 同時に走る再接続を一つにまとめる。
    - 環境の再起動では、まず `listenTask` を止める。
    - seq の欠けは、一つの通知のまとまりにつき一回だけ扱う（[ProjectSession.swift:417-488](macOS/Sources/App/Model/ProjectSession.swift)）。
    - 20 回試して失敗したら、再試行ボタン付きの問題ページを出す。今は読み込み表示が終わらずに残る。
    - Web では、EventSource がつながり直したときに状態を取得し直す（[api.ts:22](web/src/api.ts)）。
18. **終了時に未保存の編集を守る**
    - ⌘Q のとき `dirty` を確かめ、保存するか確認する（[TsugumiApp.swift:99](macOS/Sources/App/TsugumiApp.swift)）。
19. **Web の保存が止まったままにならないようにする**
    - `saveTask` を try/finally で包む（[session.ts:1099](web/src/model/session.ts)）。
20. **新規作成で既存のプロジェクトを上書きしない**
    - 中身のあるフォルダを選んだら拒否する（[AppModel.swift:164](macOS/Sources/App/Model/AppModel.swift)）。

## C. 凍結する前に形を決める

21. **プロトコル**
    - Hello で、対応するバージョンの範囲（最小と最大）を交渉できるようにする（[service.cc:274](core/src/service.cc)）。
    - `capabilities()` が返す内容を実装と一致させる（表示 7 種類、整形ツール 5 つ）。
    - 応答の型名を要求の名前に揃える（例: `RunHistoryDeleteRequest` の応答を `RunHistoryDeleteResult` に）。
22. **履歴の形式**
    - 版が違うときに丸ごと作り直すのをやめ、移行の仕組みを用意する（[history.cc:207](core/src/history.cc)）。
    - 版が新しすぎる履歴は消さずに拒否する。
23. **document.journal を圧縮する**
    - 確定済みの記録を定期的に切り詰める（[store.cc:666](core/src/store.cc)）。

## D. 公開と配布の体裁

24. **署名と公証**
    - Developer ID で署名し、hardened runtime を有効にして公証を受ける。`--deep` はやめる（[build-app.sh:64](macOS/build-app.sh)）。
    - Developer ID を用意しないなら、INSTALL.md を「ソースからビルドする」前提に書き直す。
25. **開発用のパスを配布物から外す**
    - [Package.swift:63](macOS/Package.swift) の、開発機を指す絶対パスの rpath を外す。
    - `#filePath` を使ったフォールバックは debug ビルドだけにする（SharedVM.swift:28、AppModel.swift:94、ChatController.swift:163、TsugumiApp.swift:125）。
    - バージョン番号を付ける規則を決める。
26. **英語 UI の文字列を揃える**
    - `String` で直接組み立てている文字列を、翻訳できる形に直す。
    - 抜けている約 490 件の訳を [en_strings.py](macOS/Resources/tools/en_strings.py) に補う。
27. **Keychain**
    - `kSecUseDataProtectionKeychain` を使うようにし、security.md §8 の記述と一致させる（[Provider.swift:145](agent/Sources/TsugumiAgent/Provider.swift)）。
    - Web 側（`security` コマンド経由、[chat.ts:116](web/server/chat.ts)）とアプリ側の保存方法を揃える。
    - Web の `GET /api/settings` も認証の対象に入れる（4 と同じ）。
28. **THIRD_PARTY_NOTICES と依存関係**
    - Web に同梱する react / react-dom / scheduler / marked / katex / flatbuffers を [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) に追加する。
    - npm の flatbuffers を 25.12.19 に揃え、package.json に `engines` を書く。
29. **古い記述の整理**
    - [CMakeLists.txt](CMakeLists.txt) と kernel.nix に残る Apple container の記述を直す。
    - ProjectSession.swift:415 のコメントを、実際の挙動に合わせる。

## E. テストと CI

30. **自動テストを足す**
    - A〜C の各修正に回帰テストを付ける。中心は、悪意のある `.tsugumi/`、symlink の差し替え、取消の競合、Web サーバーのアクセス制御、`Failed` 以外の例外。
    - SharedVM / EnvironmentManager のライフサイクル（失敗→再試行、停止→再起動）、再接続、`vmd_main.cc` のテストを足す。
    - WebSocket と Unix のフレーム解析には fuzz テストを入れる。
31. **CI を広げる**（[ci.yml](.github/workflows/ci.yml)）
    - Web で `npm run build` を流す。
    - macOS の runner で `macOS/CoreTests` と `macOS/Tests` を流す（VM を使わない部分）。
