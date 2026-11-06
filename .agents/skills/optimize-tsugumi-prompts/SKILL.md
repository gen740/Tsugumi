---
name: optimize-tsugumi-prompts
description: Tsugumi の Agent に指示や skill を追加・整理するとき、置き場所を選び、実モデルの少数の対照試行から採否を決める。macOS と Web の共有プロンプト、Draft、ガイド、tool 選択が対象。
---

## まず置き場所を決める

直前の失敗を一件読み、どの入力で何をしてほしかったかを書く。成功した実行と比較するには `docs/AgentRun.md` の tool 列と run 記録が使える。トークン失効や不正な RPC スキーマなど、同じ要求で必ず起こる失敗は実装か契約テストで直す。

| 誰が、いつ必要とするか | 編集する場所 |
| --- | --- |
| 通常のチャットと helper が毎回守る判断 | `agent/Shared/system-prompt.md`。同じ判断を書いた既存の段落をまず探す |
| helper の依頼文、Draft ボタンから送る定型文、個別 tool の説明 | `agent/Shared/agent.json` の該当フィールド。RPC 生成の説明なら `protocol/*.fbs` の定義を確認 |
| ホームのガイド、またはユーザーが選んだデモツアー | それぞれ `agent/Shared/guide-prompt.md`、`agent/Shared/demo-skill.md` |
| 特定のプロジェクトで明示して使う手順 | そのプロジェクトの `skills/<name>/SKILL.md`。Tsugumi チャットの `$name` で本文が入る |
| 言語別の実装例、SDK の書き方 | `agent/Reference/card-guide.md` と同梱デモ。モデルが `reference_read` で引ける例を使う |
| このリポジトリを開く Codex の開発手順 | `.agents/skills/<name>/SKILL.md`。今回のような評価手順はここに置く |

発火条件が特定モデルや特定画面に限られるときは、共有文を書き換える前に、どのクライアント・role に届くかを確かめる。`agent/Shared/` を編集したら `python3 agent/Shared/generate.py` で Swift 側を生成し、`--check` で一致を確かめる。Web は原本を直接使う。

## 少ない試行で採否を決める

1. **変えたい判断を一つ選ぶ。** 実行前に二つの依頼を固定する。一つは失敗の再現、もう一つは似ているが反対の判断を要する依頼。各々で正答の根拠、必要な tool、許されない副作用、改善したい指標を書く。保存なら revision と差分、実行なら run の終端と記録、読み取りなら答えと出所を採点する。
2. **現行版で改善の余地を確かめる。** 代表例が目標を満たし、削りたい呼び出しも既にゼロならそこで終了できる。失敗したときは最初に分岐を誤った箇所を探す。認証・fixture の起動・権限などで tool に届かなかった試行は環境エラーとして別に記録する。
3. **候補は一案だけ。** 分岐条件、参照先、完了条件のうち原因に当たる指示だけを書き換える。履歴を空にし、同じプロジェクト状態・入力・tool 群・モデル・reasoning effort で再実行する。最初の目安は二課題×現行・候補の四ターン。モデルの一ターンに複数の推論や tool 呼び出しが含まれる点に注意する。
4. **動作が合格した案同士で負担を比べる。** 必要な tool と結果が揃い、対照課題も守れたかを先に判定する。次に tool 数・再試行・ターン全体の入出力トークンを比較し、キャッシュ分を分ける。短い指示でも往復が増えることがある。差が混在するときは結論をその課題に限定し、判断を変え得る追加試行だけ行う。
5. **PR には追試できるものを残す。** 現行版と候補のリビジョン、依頼文、採点条件、各回の結果、除外した回と理由、採否を書く。実モデル＋模擬 tool、実アプリ＋実 RPC、固定応答の自動テストを区別する。

## project_inspect の試行

一般質問と現在値の質問には `tests/prompt/inspect_probe.py` が使える。サインイン済みの Codex CLI で **1 コマンドにつき実モデルを 1 ターン** 使う。標準は `gpt-6.1-sol` / `low`。リポジトリの RPC tool 定義を公開する読み取り専用の模擬サーバーで、検査結果だけを返す。実プロジェクトを書き換える作業や、macOS・Web の UI を試す際は、コピーしたデモと実アプリを使う。

リポジトリルートで、試したい側と課題を選ぶ。候補を編集中なら `--prompt` を省くと作業ツリーの原本を使う。CLI の場所は `--codex` で指定できる。

```sh
git show HEAD:agent/Shared/system-prompt.md > /tmp/tsugumi-baseline-prompt.md
python3 tests/prompt/inspect_probe.py concept --prompt /tmp/tsugumi-baseline-prompt.md
python3 tests/prompt/inspect_probe.py project
```

`concept` は「Draft はそのまま実行できますか」に、tool を使わず「実装するまで実行できない」と答えれば合格。`project` は `project_inspect` が一度成功し、読み取った 43 を返せば合格。実デモの既定値 27 を推測で返した場合や、不要な保存・実行を試みた場合は不合格。表示される `reply` の意味と `calls` の成否を両方読む。
