---
name: rerun-compare
description: Rerun selected Cards and compare output content / 再実行して出力の内容を比較する
scope: [read, run]
---
For each selected Card, inspect its last retained run record and current definition first. Check the effective recording settings (Card “project” inherits the project setting). Rerun that Card with propagate=false, using the last inputs and the same parameters, code and environment. If the available record or current definition cannot establish the same conditions, report that limitation instead of claiming a controlled comparison. Never edit the Card or change conditions to make a run succeed.
Use run_plan before run; a plan that would execute an unselected Card is outside your scope. Await the terminal run state through run_record. Compare the before/after outputs by content using the run comparison rules: schema, content_hash, then complete inline bytes. Equal byte counts, artifact ids and file paths do not establish equal content. Missing retained data means unknown, never unchanged. Report each output as same, changed or unknown and link both run records. Do not keep, reuse or delete run data automatically.
Write findings as separate Markdown paragraphs linking [file:line](card:permanent/card?file=src%2Ffile.py&lines=12-18). Say **Run and observed** only for a completed run whose records you read; otherwise say **Read only** and explain what was not observed. The person chooses what to keep as comments; a finding is not a change request.

各 Card の前回の記録と現在の定義を読みます。記録設定が「project」ならプロジェクトの値を確認します。最後の入力、同じパラメータ・コード・環境で、その Card だけを propagate=false で再実行します。同じ条件を確認できなければ、その限界を報告します。成功させるための編集はしません。
run_plan で対象を確認し、選択外の Card が実行されるなら止めます。run_record で終端を確認し、既存の実行比較と同じく型・スキーマ・内容のハッシュまたは完全なインライン値で出力を比較します。サイズ・パス・成果物 id だけでは同一としません。記録不足は「不明」とし、各出力を「同じ・異なる・不明」で示して両方の実行記録にリンクします。実行データの保存・再利用・削除は自動化しません。
指摘は上記の Card・ファイル・行範囲リンクを付けて一つずつ別の段落に書きます。終端と記録を読めたものだけ **実行して観測** とし、それ以外は **読んだだけ** と未観測の範囲を書きます。人が選んだものだけをコメントに残し、変更依頼にはしません。
