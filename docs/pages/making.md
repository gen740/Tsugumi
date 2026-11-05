# 制作記録

Tsugumi をどう作ったかの記録です。人間が意図と仕様を書き、Claude Code が形にする往復で作りました。
何が事前準備で何がこの期間の制作かの区分は[制作範囲と事前準備](provenance.md)、なぜ作ったかは [AI に使われないために、何を作るか](philosophy.md)にあります。

ここの数字は [228587d](https://github.com/gen740/Tsugumi/commit/228587d) までの Git の履歴から数えたものです。コミットの日時は記録された日時で、作業した時間そのものではありません。

<!-- 作者へ: 手応えや迷いなど、履歴に残らない振り返りはこの下に書いてください。AI は書きません。 -->

## 数字

| 指標 | 値 | 確かめるコマンド |
| --- | --- | --- |
| コミット | 245 | `git rev-list --count 228587d` |
| 日ごとのコミット | 11-02: 37 / 11-03: 55 / 11-04: 150 / 11-05: 3 | `git log 228587d --format=%ad --date=short \| sort \| uniq -c` |
| Claude との共著のコミット | 204 | `git log 228587d --format=%b \| grep -c 'Co-Authored-By: Claude'` |
| マージした Pull Request | 19 | `gh pr list --state merged --limit 100` |
| `review-from-user.md` の変更 | 12 回（追加した行は計 709 行） | `git log --oneline -- review-from-user.md` |

## 人間と AI の往復

指示は主に 2 つのファイルで渡しました。どちらも今は Git の履歴にだけあります。

1. **最初の仕様**（[instruction.md](https://github.com/gen740/Tsugumi/blob/98f90945ed92477e6e65ff48f1f410271edaaf96/instruction.md)）: 開始前に ChatGPT と練ったもので、Card・Workflow、実行環境、通信契約、UI・Agent の境界を決めています。
2. **レビューのメモ**（`review-from-user.md`）: 実際に動かして気づいたことを人間が書き、AI が直すことを繰り返しました。

| 日時 | レビューの内容 |
| --- | --- |
| [11-02 18:19](https://github.com/gen740/Tsugumi/commit/0b1c769) | 動かして気づいた最初の修正点 |
| [11-03 10:03](https://github.com/gen740/Tsugumi/commit/6ca9fde) | 実行とビルドの速さ |
| [11-03 14:02](https://github.com/gen740/Tsugumi/commit/e3b815e) | UI と定義のモデル |
| [11-04 01:56](https://github.com/gen740/Tsugumi/commit/afe7a68) | レビューの指摘を優先順に |
| [11-04 07:04](https://github.com/gen740/Tsugumi/commit/e60f569) | メモを完了して、履歴に残す |

## 経過

| 日時 | 出来事 |
| --- | --- |
| [11-02 13:15](https://github.com/gen740/Tsugumi/commit/98f9094) | 最初の仕様をコミット |
| [11-02 14:22](https://github.com/gen740/Tsugumi/commit/eaaa7eb) | protocol と `tsugumid` |
| [11-02 14:34](https://github.com/gen740/Tsugumi/commit/cfcb711) | C++ / Python の SDK と Collatz デモ |
| [11-02 14:51](https://github.com/gen740/Tsugumi/commit/5fb7a1d) | Apple container の環境で Card を隔離して実行 |
| [11-02 15:43](https://github.com/gen740/Tsugumi/commit/e8b14d5) | SwiftUI アプリと Agent |
| [11-03 02:12](https://github.com/gen740/Tsugumi/commit/3041d5d) | Virtualization.framework の共通 VM へ移行 |
| [11-03 22:37](https://github.com/gen740/Tsugumi/commit/4024439) | Web UI |
| [11-04 01:44](https://github.com/gen740/Tsugumi/commit/f801fa0) | 実際の Claude API で Draft から実行までを通した記録 |
| [11-04 08:56](https://github.com/gen740/Tsugumi/commit/60f3030) | Agent が Card の権限を広げるときの承認 |
| [11-04 13:21](https://github.com/gen740/Tsugumi/commit/894a0f3) | タグからのリリース |
| [11-04 14:01](https://github.com/gen740/Tsugumi/commit/31b92a5) | ChatGPT プランでの接続 |

## 変えた判断

| 何を変えたか | なぜ | 記録 |
| --- | --- | --- |
| Apple container をやめて、共通 VM にした | 人間のレビューで「1 アプリに 1 VM、複数のプロジェクトで同じ `/nix/store` を共有する」と決めた。プロジェクトごとの VM と store を増やさず、Virtualization.framework を直接使う | [レビュー](https://github.com/gen740/Tsugumi/commit/1e9fed3)、[移行](https://github.com/gen740/Tsugumi/commit/3041d5d)、[撤去](https://github.com/gen740/Tsugumi/commit/ce9968f)、[当時の問題](development.md#解決済みの記録)、[当時の計測](development.md#計測2026-11-02apple-container-時代) |
| 実行履歴を共有フォルダから VM のディスクへ移した | 計測で、Run の時間の大半が virtiofs 上の履歴の確定待ちだと分かった | [計測の変化](development.md#計測2026-11-03高速化後共通-vm) |
| 大きなデータを実行中に取得するのをやめ、Nix で固定した | Agent が 60 手の上限で止まった。1 回の実行が 483 秒かかり、手数を待ち時間で使い切っていた | [AgentRun の改善前の試行](agentrun.md#改善前の試行とそれを受けて直したこと) |
| Agent に Draft デモの答えを読ませないようにした | 完成形を読めると、答えを写せてしまい検証にならない | [変更](https://github.com/gen740/Tsugumi/commit/c759adb) |
| 自前の SHA-256 をやめ、プラットフォームのものにした | 256 MB のハッシュが 5,572 ms から 134 ms になった（最適化なしのビルド） | [変更](https://github.com/gen740/Tsugumi/commit/fc984c3) |
| worker のモード切替を取り入れなかった | トグルを言い換えただけで意味が変わらず、表示がログの書式に頼っていた | [PR #8](https://github.com/gen740/Tsugumi/pull/8) |
| 専用の Mac での定期 VM テストを取り入れなかった | self-hosted runner を用意しておらず、一度も動かない CI が残るだけだった。public リポジトリに置くこと自体も負担とリスクになる。VM のテストは手元で動かす | [PR #18](https://github.com/gen740/Tsugumi/pull/18) |

## テストで見つかり、直したもの

レビューや計測で見つけた不具合は、回帰テストと一緒に残しています。

| 不具合 | 記録 |
| --- | --- |
| 履歴の保存に失敗した実行が成功になる | [`test_regressions.py`](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_regressions.py)、[修正](https://github.com/gen740/Tsugumi/commit/9371391) |
| 終了した sandbox の helper が zombie として残る | [当時の計測](development.md#計測2026-11-02apple-container-時代) |
| キャッシュの削除で環境の実行時パスまで回収される | [解決済みの記録](development.md#解決済みの記録) |
| Agent の書いた Card が開き直すまで Canvas に出ない | [AgentRun](agentrun.md#改善前の試行とそれを受けて直したこと) |
