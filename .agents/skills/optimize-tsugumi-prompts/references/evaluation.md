# Tsugumi で採用する評価方法

| 確かめたいこと | 今回選んだ方法と理由 |
| --- | --- |
| プロンプトを変える必要があるか | エラーの直前にモデルへ渡った値と、core が要求する契約を比べる。任意の JSON フィールドを省略すること自体が難しい場合は、言い換えより先に Responses の `strict` と tool のスキーマを見る。[OpenAI の function calling](https://developers.openai.com/api/docs/guides/function-calling#strict-mode) |
| skill の説明文が機能するか | 新規会話で使うべき依頼と隣接した別の依頼を出す。skill の発見・本文の読み取り・依頼の達成を切り分ける。Codex では JSONL の tool 記録を使える。[OpenAI の skill 評価例](https://developers.openai.com/blog/eval-skills)・[Claude Code の skill 評価手順](https://code.claude.com/docs/en/skills#evaluate-and-iterate-on-a-skill) |
| 複数手の仕事を正しく終えたか | モデルが成功と言った回でも、期待する定義と保存済みの run・出力を独立して読む。別経路でも目的を達成できるなら、正しい最終状態を主判定に置く。確認したかった中間手順の省略は別の所見として残す。[Anthropic の Agent 評価](https://www.anthropic.com/research/demystifying-evals-for-ai-agents) |
| 安価な試行をどこまで信用するか | 固定応答は採点器の検査、実モデル＋模擬 tool は判断の切り分け、実モデル＋対象クライアントは製品経路の確認に使う。異なる harness の入力トークンやキャッシュの値を並べてモデルの優劣を決めない。[OpenAI の harness 比較実験](https://developers.openai.com/cookbook/examples/partners/harness_aware_plugin_evals/harness_aware_plugin_evals) |
| どの変更が効いたか | 対象の一箇所だけ変え、直接の再現課題とは別の課題も残す。共通文であれば影響するモデル・role の範囲を特記する。Claude Code でも行単位の切り分けにより、短さを狙った指示が別課題で性能を下げていたと判明した。[Anthropic の品質調査](https://www.anthropic.com/research/april-23-postmortem) |

**今回は導入を見送った方法。** MIPRO は多段の LM プログラムで、下流の評価値を使って指示や例の候補を探索する。Tsugumi でも「最後にユーザーが必要とする結果」を指標にする考え方は使える。少数の決まった障害を調べる段階では、候補を多数生成する基盤を入れるより、未使用の課題を残す方を優先する。run の成功と保存内容はコードで照合できるため、その採点に別の LLM は使わない。[Opsahl-Ong ほか、EMNLP 2024](https://aclanthology.org/2024.emnlp-main.525/)
