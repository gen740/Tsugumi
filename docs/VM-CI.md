# 共通 VM の CI

`.github/workflows/vm.yml` は Apple Silicon の専用 Mac で共有 VM のテストを実行します。毎週火曜 03:30 JST と手動実行が入口です。通常の PR チェックは `.github/workflows/ci.yml` の hosted runner で続けます。

## Runner の準備

1. テスト専用の Apple Silicon Mac に macOS 26 以降と Xcode、Nix を用意し、Xcode の初回セットアップを終える。VM と Nix store の容量を確保する。
2. `macOS/Container/build-image.sh --vm` で起動イメージを用意する。公開イメージがない場合のビルドには Apple `container` とその起動済みサービスが必要。
3. このリポジトリ専用の GitHub Actions self-hosted runner を登録し、標準の `self-hosted`, `macOS`, `ARM64` に `tsugumi-vm` ラベルを追加する。runner の実行ユーザーで `nix`・Xcode が使えるようにする。
4. `macOS/tools/test-vm.sh --check`、次に `macOS/tools/test-vm.sh` を実行して環境を確認する。
5. repository variable `TSUGUMI_VM_CI` を `1` にする。登録・有効化はこの変更では行わない。無効時はジョブがスキップされる。

この runner では信頼済みの `main` のみ実行します。workflow に `pull_request` / `pull_request_target` は設けず、手動実行でも main 以外はスキップします。runner に個人用の認証情報・プロジェクトを置かず、ほかの workflow にも untrusted PR をこの runner へ渡す入口を追加しないでください。

## 実行と調査

`macOS/tools/test-vm.sh` が起動イメージの準備、vmctl のビルド・署名、ホストのプロトコル bindings の生成、実 VM テストを実行します。

対象は `test_shared_vm.py`、`test_workers.py`、`test_cards_container.py`。プロジェクト間の隔離、再起動、worker の再利用・初期化、実行モードの記録、Card 実行と表示を確認します。テストが 0 件またはスキップされた場合は失敗にします。

- `build/vm-ci/`: ビルド・pytest・VM のログと JUnit XML。成功・失敗にかかわらず artifact に 14 日保持する。
- `build/test-vm/`: テスト用 VM の永続ディスク。Nix のダウンロードを再利用する。`TSUGUMI_TEST_VM_SUPPORT` で場所を変更できる。
- ワークフローを同時に走らせず、120 分で打ち切る。異常終了後は残った vmctl / VM を確認してから再実行する。使用中のディスクは削除しない。

同じ runner の checkout はキャッシュを残すため `clean: false` です。信頼境界は専用 runner と main の保護にあります。CI の定期稼働には runner の登録と variable の設定が必要です。
