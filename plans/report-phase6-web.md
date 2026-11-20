# Phase 6A Web サーバーの簡素化

## 設計

現在、ブラウザは Project RPC を Wasm から直接使い、Host と Agent の操作を Node の JSON API と SSE に載せている。Node は FlatBuffers の操作をバイト配列に変換し、環境・端末・会話・承認の状態を hostd と二重に持っている。

ブラウザから同一オリジンの WebSocket を開き、hostd に Host RPC のバイナリをそのまま送る。Node はページの Cookie、Host ヘッダーと Origin を検証して接続を中継する。この方式なら macOS アプリが先に起動した hostd も、起動時に Web のポートを知る必要がない。REST の変更操作は従来どおり x-tsugumi ヘッダーを要求する。

まず操作・レイアウト・端末を Host RPC へ移し、操作のバイト配列変換と Node の端末台帳を削除する。次に環境の一覧とイベントを hostd の状態に寄せる。最後に Agent の通知をイベント駆動にし、承認と設定の中間状態を整理する。各段階を前段階の先端から切る。配信、ページ誘導、CLI/MCP、起動・設定や会話の組み立てで必要な処理は Node に置く。

主なリスクは macOS と Web の hostd 共有、ページと MCP の権限境界、端末の初回出力と接続切断、会話イベントの再接続時の追跡。ゲストの変更は含めない。
