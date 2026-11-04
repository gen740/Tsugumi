# Tsugumi ドキュメント一覧

プロジェクトのコンセプトと実画面によるウォークスルーは [README](../README.md) にあります。
読む目的に合わせて、次の文書から始めてください。

## はじめる

| 文書 | 内容 |
| --- | --- |
| [INSTALL.md](../INSTALL.md) | GitHub Releases のアプリの導入・自動更新と、ソースからのビルド手順 |
| [Usage.md](Usage.md) | 画面ごとの基本操作と、同梱デモの試し方 |
| [WebUI.md](WebUI.md) | ブラウザで同じ画面を使う Web UI（Mac 上の tsugumid、Seatbelt での隔離） |

## 考え方を知る

| 文書 | 内容 |
| --- | --- |
| [QA.md](QA.md) | 課題意識、Draft、再現性、記録と再実行についての Q&A と、Nix 上流参照の検証の補足 |
| [novelty.md](novelty.md) | 同じ Workflow で設計・実装・検証をつなぐ独自性、なぜ今この設計なのか、試行錯誤を支える実装上の工夫 |
| [Vision.md](Vision.md) | 研究の判断を学び、ツールを育て、次の人へ仕事を渡すための応用と、業務自動化の構想 |
| [Philosophy.md](Philosophy.md) | 「AI に使われないために、何を作るか」——背景にある問題意識と制作体験 |
| [Name.md](Name.md) | 「継ぐ・積む・組む」「紡ぐ」、そして鳥のツグミに重ねた名前の由来 |

## 仕組みを知る

| 文書 | 内容 |
| --- | --- |
| [features.md](features.md) | 主要機能の技術解説（全体構成・Card と Workflow・Nix・SDK と worker・保存・履歴・可視化・UI・Agent・デモ） |
| [Versions.md](Versions.md) | 版の管理: プロジェクトの git、実行ごとの版と結果（git notes）、版のツリーと「この版から始める」 |
| [security.md](security.md) | 4 段階の隔離境界（VM・プロジェクト・Card・RPC）、API キーの管理、データの置き場所、既知の制限 |
| [ui-view.md](ui-view.md) | macOS アプリの画面構成と操作の仕様 |

## 記録と開発

| 文書 | 内容 |
| --- | --- |
| [AgentRun.md](AgentRun.md) | 実際の Claude API で、Draft から Card を生成して実行・検証するまでを通した記録 |
| [Development.md](Development.md) | 実行基盤の設計決定、テストと確認範囲、構成ごとの計測、現在の実装状況と解決済みの記録 |
| [Lock.md](Lock.md) | 実行環境と依存ライブラリの固定バージョン一覧 |
