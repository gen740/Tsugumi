---
name: review
description: Inspect selected Cards and their assumptions / 選択した Card と前提を読む
scope: [read]
---
Read only the selected Cards. Do not run, edit, resolve comments or retain findings automatically.
Read each Card's README.md title and description, and inspect its sandbox, grants and workerCapabilities beside the code that appears to use them: randomness, clocks, network and file access; dependence on a resident worker's state; and work that could move between build and run. Read the environment definition and sources together. No per-language analyser is needed. Explain uncertainty and cite the actual code rather than guessing a failure from a keyword.
For each finding, write a separate Markdown paragraph with a source link of the form [file:line](card:permanent/card?file=src%2Ffile.py&lines=12-18), the concrete concern and supporting evidence. Label it **Read only**; never claim it was observed in execution. Do not turn findings into change requests. The person decides which findings to keep as Card comments.

選択された Card だけを読みます。実行・編集・コメントの解決・指摘の自動保存はしません。
各 Card の README.md の title と description を読み、sandbox、grants、workerCapabilities と、それを使っているように見えるコードを並べて確認します。乱数、時計、ネットワーク、ファイルアクセス、常駐 worker の状態への依存、build と run の間で移せる処理を、環境定義とソースを合わせて読みます。キーワードだけで不具合と断定せず、不確かさも説明します。
指摘は一つずつ独立した Markdown 段落にし、上記の形式で Card id・ファイル・行範囲をリンクし、具体的な懸念と根拠を書き、**読んだだけ** と明示します。変更依頼にはせず、人が選んだ指摘だけがコメントとして保存されます。
