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

1. **失敗を一件たどる。** 依頼、最初に誤った判断、その時モデルが見た指示・tool 説明・引数・返答、最終状態を並べる。Tsugumi チャットは共有文をモデルの指示として送り、外部 MCP では最初の `tsugumi_open` の返答で渡す。モデル名だけが同じでも、この経路の違いは残る。
2. **正しさと効率を別に採点する。** 実行前に、依頼どおりの状態、必要な根拠、許されない副作用を決める。保存・実行なら、モデルの最終回答を、再読した定義・run の終端・対象 Card の記録と照合する。正しく完了した回同士で、不要な読み取り、再試行、出力、キャッシュを分けた入力を比べる。手順自体が要件のときだけ、特定の tool や順序を合格条件にする。
3. **原因を一つだけ変える。** 簡単な契約検査や記録の再生で直せるものを先に確認する。指示の問題なら対象の文だけ、引数の問題なら wire 上のスキーマを変える。tool の説明を直した場合は再ビルドし、モデルに届く定義で差分を確かめる。障害を再現する依頼と対照課題を、同じ初期状態・入力・モデル・effort・tool 群で新規会話から比較する。
4. **代理環境から実際の経路へ進む。** 下の probe は読み取り判断の切り分け用。採用前には対象クライアントで、調整に使わなかった依頼を一つ試す。skill の発火を変えたら、名前を明示する試験に加え、適用すべき自然文と、語彙は似ていて適用すべきでない自然文を出し、本文を読んだ記録と作業結果を別々に確かめる。
5. **次の一回が判断を変えるかで止める。** 現行が目標を満たす場合や候補で同じ失敗が続く場合は、指示を増やす前に原因を見直す。単発の正答やトークン差から成功率・費用改善を推定しない。それが採用理由なら現行と候補の実行順を入れ替えて再測定する。PR に条件・版・除外した環境エラー・観測できた差・未検証の範囲を記す。

評価方法を選び直すときは[手法を取り入れた根拠](references/evaluation.md)を参照。

## project_inspect の試行

一般質問と現在値の質問には `tests/prompt/inspect_probe.py` が使える。サインイン済みの Codex CLI で **1 コマンドにつき実モデルを 1 ターン** 使う。標準は `gpt-6.1-sol` / `low`。リポジトリの RPC tool 定義を公開する読み取り専用の模擬サーバーで、検査結果だけを返す。実プロジェクトを書き換える作業や、macOS・Web の UI を試す際は、コピーしたデモと実アプリを使う。

リポジトリルートで、試したい側と課題を選ぶ。候補を編集中なら `--prompt` を省くと作業ツリーの原本を使う。CLI の場所は `--codex` で指定できる。

```sh
git show HEAD:agent/Shared/system-prompt.md > /tmp/tsugumi-baseline-prompt.md
python3 tests/prompt/inspect_probe.py concept --prompt /tmp/tsugumi-baseline-prompt.md
python3 tests/prompt/inspect_probe.py concept
```

`concept` は「Draft はそのまま実行できますか」に、tool を使わず「実装するまで実行できない」と答えれば合格。`project` は `project_inspect` が成功し、読み取った 43 を返すことが正しさの条件。2回目以降の読み取りは余分なコストとして記録する。実デモの既定値 27 を推測で返した場合や、不要な保存・実行を試みた場合は不合格。`invalid` は環境または記録の不備、`fail` は観測できる要件違反。`needs_review` は人が `reply` の意味と `other_items` を確認する段階。前後の記録で CLI 版・effort・fixture ハッシュも照合する。
