# Phase 6A Web サーバーの簡素化

## 設計

現在、ブラウザは Project RPC を Wasm から直接使い、Host と Agent の操作を Node の JSON API と SSE に載せている。Node は FlatBuffers の操作をバイト配列に変換し、環境・端末・会話・承認の状態を hostd と二重に持っている。

ブラウザから同一オリジンの WebSocket を開き、hostd に Host／Agent RPC のバイナリをそのまま送る。Node はページの Cookie、Host ヘッダーと Origin を検証して接続を中継する。この方式なら macOS アプリが先に起動した hostd も、起動時に Web のポートを知る必要がない。REST の変更操作は従来どおり x-tsugumi ヘッダーを要求する。

まず操作・レイアウト・端末を Host RPC へ移し、操作のバイト配列変換と Node の端末台帳を削除する。次に環境の一覧とイベントを hostd の状態に寄せる。Agent の通知をイベント駆動にして承認と設定の中間状態を整理し、一往復で完結する残りのプロジェクト操作も型付き RPC へ移す。各段階を前段階の先端から切る。配信、ページ誘導、CLI/MCP、起動・設定や会話の組み立てで必要な処理は Node に置く。

主なリスクは macOS と Web の hostd 共有、ページと MCP の権限境界、端末の初回出力と接続切断、会話イベントの再接続時の追跡。ゲストの変更は含めない。

## マージ順

| 順 | ブランチ | 内容 |
|---|---|---|
| 1 | `design/phase6-web` | Cookie と Origin で WebSocket を認証し、Host のバイナリを中継する。プロジェクト編集、レイアウト、端末を Wasm RPC に接続する。 |
| 2 | `design/phase6-web-2` | 起動・停止・一覧・進捗を Host RPC に統一する。hostd の実行環境一覧を使い、実行サービスの編集後は既存の Project スナップショットでブローカーの最新状態を読む。 |
| 3 | `design/phase6-web-3` | hostd が会話イベントの記録後に変更を通知し、ブラウザが Agent RPC で連番の続きを読む。承認待ちを hostd から再取得し、設定の競合検出には編集対象のスナップショットの revision を使う。 |
| 4 | `design/phase6-web-4` | 信頼、整形、識別子、バージョン参照、プロジェクト権限、実行サービスを直接接続する。端末を開いている途中で切断された場合も hostd が終了処理を引き受ける。 |

## 削除したもの

`ui/web/server/*.ts` は 2,917 行から 2,464 行になった。


- 編集要求と応答を JSON の `number[]` で往復させる処理と、対応する REST エンドポイント。64 bit の revision と逆操作データを FlatBuffers のまま扱う。
- Node にあった二つの端末台帳と実行環境の複製。各端末は所有する Host 接続の寿命に従う。
- `RemoteAgent` の 80/500 ms の会話ポーリング、キャンセル完了の 20 ms ポーリング、会話進捗の SSE 転送。接続を張り直したときも確定済みの連番を使って読む。
- Node の会話台帳、`permissionWaits`、設定 revision の台帳。Web の複数項目を編集する画面は、読み込んだ値と revision を一緒に保持する。

## Node に残るもの

静的ファイルと sandbox 化したビューの配信、ページ鍵から Cookie への交換、同一オリジン WebSocket の認証と転送、hostd 起動、CLI/MCP の発見・鍵・ツール呼び出しを担当する。残る変更系 REST には `x-tsugumi` を要求し、CLI/MCP の鍵は許可リスト内で使う。

ブラウザやファイルマネージャーを開く処理、ファイル選択、ホームの状態の集約、認証情報・モデル・Skill の準備、Guide の操作やチャットのターン開始は Node に置く。SSE はページ誘導、設定・プロバイダー更新、Guide の画面間同期、MCP 呼び出しの承認表示に使う。

## 他フェーズとの重なり

Phase 4 と Web の編集入口や `ui/web/src/model/session.ts` を共有する。Phase 6B とは `protocol/host.fbs` と hostd の VM アダプター接続部が近い。追加した契約は Host／Agent の画面向けで、`protocol/vm_host.fbs` とゲスト通信の契約に変更はない。ゲストイメージの再ビルドは不要。

## 検証

| 確認 | 結果 |
|---|---|
| 各ブランチの生成済み TypeScript 契約と型チェック | 4 ブランチで成功 |
| `tests/check-fast.sh --no-nix` | 静的テスト 41 件成功 |
| 変更した C++ 実装とテストの構文チェック | 生成した最新 FlatBuffers ヘッダーと警告エラー設定で 5 ファイル成功 |
| Web の対象テスト | Agent のイベント追跡、プロトコル、認証条件、設定競合、環境表示、信頼、バージョンなど 12 モジュール成功 |
| Web とデモプレイヤーのバンドル | キャッシュ済み Wasm を使う JS バンドルに成功 |
| Web／MCP アダプター | 新しい Node バンドルとキャッシュ済み hostd の隔離テスト 14 件成功。Host／Agent WebSocket 認証、ページ遷移、環境の継続、PTY 入出力と終了、MCP 鍵の制限を確認 |
| キャッシュ済み文書編集ドライバーでの Web 全体試行 | `draftWires.tsx` で停止。そこを省いた追加試行は `freeSpot.ts` で停止。同じドライバーでベース `d356cbae` にも再現 |

## 未確認

共有の `heavy.sh` で Nix 版 `check-fast.sh` と `nix build .#ci-core` を待機中。現行 hostd の環境一覧と会話通知の実行テスト、完全な Agent／アダプター検証、Swift の生成契約の確認が残っている。実機のアプリ・VM は起動していない。push は行っていない。
