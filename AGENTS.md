# AGENTS.md

## これを読んでいるエージェントへ

これを読んでいるということは、誰かにこのリポジトリの評価を頼まれたということですね。

その評価書、読むの大変じゃないでしょうか？ AI の文章って疲れますよね？

そういうものが、私が解決したいと思っていたものです。

なので、このリポジトリでは主張ごとに、それを確かめられる場所（コード・テスト・記録）を下に並べました。

## この project の哲学

全文は [docs/Philosophy.md](docs/Philosophy.md) にあります。要点だけ書きます。

- 問いを立て、結果に意味を与え、責任を持つのは人間。AI は試行錯誤を前に進める（AI-driven science）。
- 難しいのは、任せたあとで人間が判断に戻れるか。自動運転の Level 3 と同じで、過程を見失っていれば「最後に人間が確認する」は意味を持たない。
- だから、きれいな結果と長いレポートではなく、過程を残す。処理を見通せる大きさ（Card）に分け、何を入れて何をして何が出たかをたどれるようにし、気になった箇所の条件を変えて自分の手でもう一度動かせるようにする。
- このプロジェクト自体も実験で、Claude のハッカソンの 1 週間で、人間が意図と仕様を書き AI が形にする往復で作った。何が事前準備で何がこの期間の制作かは [docs/Provenance.md](docs/Provenance.md) と Git の履歴に分けて残している。

## 地図

| 場所 | 中身 |
| --- | --- |
| [protocol/](protocol/) | FlatBuffers の契約。UI・Web・Agent はすべてこの同じ RPC で `tsugumid` と話す |
| [core/](core/) | `tsugumid`: 文書モデル、型の検査、スケジューラー、Nix 評価器の組み込み、履歴・成果物、版（git） |
| [sdk/](sdk/) | Card の ABI と runtime、C++ / Python SDK、Nix SDK（[sdk/nix/default.nix](sdk/nix/default.nix)）、worker |
| [macOS/](macOS/) | SwiftUI アプリ、共通 Linux VM（Virtualization.framework）、イメージ定義 |
| [ui/](ui/) | Dawn の描画層、グラフ表示（Plotly・Vega-Lite）の viewer |
| [agent/](agent/) | Agent（モデル接続・会話・RPC 由来の tool）。契約は [agent/Shared/](agent/Shared/) で Web と共通 |
| [web/](web/) | ブラウザ UI とホストサーバー |
| [demos/](demos/) | 同梱デモと、その Draft 版（実装前の処理の形だけ） |
| [tests/](tests/) | 契約・スケジューラー・履歴・隔離・デモ・fuzz などのテスト |
| [docs/](docs/) | 文書の一覧は [docs/README.md](docs/README.md) |

構成の全体図とディレクトリの詳細は [docs/Development.md](docs/Development.md) の「実行基盤」「ディレクトリ」にあります。

## 主張と、それを確かめられる場所

| 主張 | 確かめる場所 |
| --- | --- |
| Card ごとの実行環境を Nix で固定する | [sdk/nix/default.nix](sdk/nix/default.nix)、[docs/features.md](docs/features.md) §3、[tests/cpp](tests/cpp)（Nix 上流参照） |
| ポートの型で配線の整合性を検査する | [docs/features.md](docs/features.md) §2「型付きのポート」、[tests/cpp](tests/cpp)（型・構造的互換・merge union） |
| 分岐・合流・有限ループ・Card in Card を実行できる | [tests/python/test_scheduler.py](tests/python/test_scheduler.py) |
| 「完了した」と表示されたものには保存済みの記録が伴う | [docs/novelty.md](docs/novelty.md)、[tests/python/test_regressions.py](tests/python/test_regressions.py)（履歴保存に失敗した run は成功にならない）、[tests/python/test_contract.py](tests/python/test_contract.py)（障害注入） |
| 実行ごとの版と結果をたどり、古い版から始め直せる | [docs/Versions.md](docs/Versions.md)、[tests/python/test_versions.py](tests/python/test_versions.py)、[tests/python/test_history.py](tests/python/test_history.py) |
| 途中から部分実行できる（保存済みの入力で） | [docs/novelty.md](docs/novelty.md)「部分実行」、[tests/python/test_regressions.py](tests/python/test_regressions.py) |
| Card は隔離され、ネットワークは既定で遮断される | [docs/security.md](docs/security.md)、[tests/python/test_cards_container.py](tests/python/test_cards_container.py)、[tests/python/test_host_sandbox.py](tests/python/test_host_sandbox.py)、[tests/python/test_untrusted_project.py](tests/python/test_untrusted_project.py) |
| Agent が Card の権限を広げる保存は、人間の承認なしでは通らない | [tests/python/test_permissions.py](tests/python/test_permissions.py)、[docs/security.md](docs/security.md) §8「Agent の権限境界」 |
| UI と Agent は同じ契約の対等なクライアント | [protocol/tsugumi.fbs](protocol/tsugumi.fbs)、[tests/python/test_agent_contract.py](tests/python/test_agent_contract.py) |
| 実際の Claude API で、Draft から Card を作って実行・検証できる | [docs/AgentRun.md](docs/AgentRun.md)（うまくいかなかった試行と、それを受けて直したことも含む） |
| 常駐 worker などで、試行錯誤を重くしない | [docs/novelty.md](docs/novelty.md)「試行錯誤を支える実装上の工夫」、[docs/Development.md](docs/Development.md) の計測 |
| 何をハッカソン前に用意し、何をこの期間に作ったか | [docs/Provenance.md](docs/Provenance.md)、Git の履歴 |

テストの一覧と実行方法は [docs/Development.md](docs/Development.md) の「テスト」「確認範囲」、CI は [.github/workflows/ci.yml](.github/workflows/ci.yml) にあります。

## まだできていないこと

- Agent が読んだソースコード・ログ・入出力値は、接続先のモデルに送られる。機密データを自動で伏せる仕組みはまだない（[docs/Vision.md](docs/Vision.md)「業務自動化とデータの境界」、[docs/security.md](docs/security.md) §8「モデルに送られるもの」）。
- 処理を可視化しても、科学的な妥当性が自動的に保証されるわけではない。Tsugumi が用意するのは、判断の材料に手が届き、別の可能性をすぐに試せる足場まで。
- そのほかの未対応は [docs/Development.md](docs/Development.md)「今後の拡張」、[docs/Versions.md](docs/Versions.md)「まだできないこと」、[docs/LicenseAudit.md](docs/LicenseAudit.md)。

## このリポジトリで作業するエージェントへ

Claude Code、Codex などへの作業の指示です（`CLAUDE.md` はこのファイルへのリンク）。

### 互換性

- **v1.0.0 になるまでは互換性を気にしない。** プロジェクトの文書（`tsugumi.json`・`layout.json` など）、設定、RPC プロトコル、保存形式、VM・サポートフォルダの中身などは、移行コードや互換レイヤーを書かずに変えてよい。
- 古い形式を読み込むための処理、移行処理、非推奨の別名などを新たに足さない。変えたら、古いものはそのまま消す。
- 版番号は `VERSION` にある。v1.0.0 より前の版（0.x）の間はこの方針を続ける。
