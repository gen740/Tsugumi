# 主張と、それを確かめる方法

Tsugumi が言っていることと、それを自分で確かめる方法の表です。長い説明を読む代わりに、気になった行から動かしてください。

テスト名が実在することは [`test_docs.py`](../tests/python/test_docs.py) が確かめています。

## 動かし方

ビルドします（前提は Nix のみ）。

```bash
nix develop -c bash -c 'cmake -S . -B build -G Ninja && ninja -C build tsugumid tsugumi-worker tsugumi_core_tests'
```

C++ のテストを流します。

```bash
nix develop -c ctest --test-dir build --output-on-failure
```

Python のテストを流します。ファイルやテスト名を指定できます（例: `test_scheduler.py`）。

```bash
nix develop -c bash -c 'cd tests/python && python3 -m pytest -q'
```

共通 VM のテストは、Virtualization.framework を使える Mac で `TSUGUMI_TEST_CONTAINER=1` を付けて流します。

## 最後に確かめた結果

| 環境 | commit | 結果 |
| --- | --- | --- |
| Linux・macOS（GitHub Actions） | [1d3d419](https://github.com/gen740/Tsugumi/actions/runs/41987549543) | [PR #35](https://github.com/gen740/Tsugumi/pull/35) で成功（2026-11-05）。範囲は [ci.yml](../.github/workflows/ci.yml) |
| Mac 上の `tsugumid`（M3 Max, macOS 27.2、Seatbelt） | dd3dd0c に文書の変更を加えたもの | C++ は全件成功。Python は 155 件成功・26 件スキップ（2026-11-05）。スキップはすべて VM のテスト |
| 共通 VM | | **この commit では未確認。** 26 件はこの環境で流していません |
| Swift のテスト（`macOS/CoreTests`・`macOS/Tests`） | 1d3d419 | 上の GitHub Actions でのみ確認 |

## 主張ごとの確かめ方

「VM」の印があるテストは共通 VM でだけ動きます。

| 主張 | テスト | 実装と記録 |
| --- | --- | --- |
| Card ごとの環境を Nix で固定し、ビルド結果を再利用する | `test_shared_vm.py::test_build_roots_across_reopen_store_cleanup_and_copies`（VM）、`test_cards_container.py::test_nix_upstream_references`（VM）、C++ の `upstream_*` | [sdk/nix/default.nix](../sdk/nix/default.nix)、[主要機能 §3](features.md#3-nix-による再現可能な実行環境) |
| ポートの型で配線の整合性を検査する | C++ の `structural_compatibility`・`merge_union_keeps_origin` | [core/src/graph.cc](../core/src/graph.cc)、[型付きのポート](features.md#型付きのポート) |
| 分岐・合流・有限ループ・Card in Card を実行できる | `test_scheduler.py::test_subgraph_runs_as_one_card_and_keeps_paths_apart`、`test_scheduler.py::test_max_runs_is_enforced`、`test_demos.py::test_collatz_27`、`test_demos.py::test_collatz_limit_overflow_and_skipped`（C++ と Python のループ。CI で動く）、`test_demo_collatz.py`（4 言語と R の範囲。`TSUGUMI_TEST_DEMO_RUNS=1` のとき） | [core/src/scheduler.cc](../core/src/scheduler.cc)、[Collatz デモ](Demos.md#collatz-c--python--julia--rcollatz) |
| 「完了した」と表示されたものには保存済みの記録が伴う | `test_regressions.py::test_history_write_failure_fails_the_run`、`test_contract.py::test_fault_injection_never_leaves_partial_updates` | [設計の独自性](novelty.md#完了したという表示に保存済みの記録が伴うようにする) |
| 実行ごとの版と結果をたどり、古い版から始め直せる | `test_versions.py::test_runs_commit_note_and_branch`、`test_versions.py::test_a_run_note_says_what_ran`、`test_versions.py::test_a_version_has_its_canvas_diff_and_summary`、`test_versions.py::test_the_casa_demo_carries_its_trials` | [core/src/versions.cc](../core/src/versions.cc)、[版の管理](Versions.md) |
| 途中から部分実行できる（保存済みの入力で） | `test_regressions.py::test_saved_file_inputs_survive_cache_clear_and_restart`、`test_regressions.py::test_saved_inputs_missing_port_fails`、`test_scheduler.py::test_run_from_one_branch_meets_the_saved_values_of_the_others` | [部分実行の設計](novelty.md#部分実行で入力を集め直す手間を減らす) |
| Card は隔離され、ネットワークは既定で遮断される | `test_host_isolation.py::test_a_sandboxed_card_reaches_no_port_and_no_file_of_this_host`（CI の Linux・macOS）、`test_host_sandbox.py::test_a_card_with_network_reaches_nothing_on_this_host`、`test_cards_container.py::test_isolation`（VM）、`test_workers.py::test_network_is_off_unless_the_card_allows_it`（VM）、`test_shared_vm.py::test_projects_share_the_store_but_not_users_views_or_processes`（VM） | [セキュリティと隔離](security.md)。Nix の固定出力 derivation の通信は[§5](security.md#5-nix-評価の制限) |
| 他人のプロジェクトの `.tsugumi/` を読んでも、外を書き換えられない | `test_untrusted_project.py::test_a_journal_record_writing_outside_the_project_is_not_applied`、`test_untrusted_project.py::test_a_copied_history_names_no_folder_outside_the_project` | [Card の権限の確認は別の問題](Limitations.md#データと権限) |
| Agent が Card の権限を広げる保存は、人間の承認なしでは通らない | `test_permissions.py::test_an_agent_save_widening_permissions_needs_the_users_approval`、`test_permissions.py::test_a_card_in_card_is_checked_too`、`test_claude_code_plugin.py::test_the_agent_key_cannot_answer_approvals_or_reach_the_page_routes`（plugin の鍵は承認に答えられない） | [core/src/permissions.cc](../core/src/permissions.cc)、[Agent の権限境界](security.md#agent-の権限境界) |
| Card のコメントは人間と Agent のどちらも書け、誰が何をしたかが記録される（Agent の分はモデルの申告によらない）。各版でどう変わったかをたどれる | `test_card_comments.py::test_a_model_resolving_a_comment_is_recorded_with_what_it_changed`、`test_card_comments.py::test_what_a_model_says_about_who_did_it_does_not_count`、`test_card_comments.py::test_each_version_says_what_it_did_to_the_comments` | [主要機能](features.md#コメントから-card-を生成)、[版の管理](Versions.md#コメント)、[core/src/comments.cc](../core/src/comments.cc) |
| UI と Agent は同じ契約の対等なクライアント | `test_agent_contract.py::test_web_agent_keeps_the_contract`、`test_agent_contract.py::test_swift_copy_of_the_shared_files_is_current` | [protocol/tsugumi.fbs](../protocol/tsugumi.fbs)、[RPC 契約](Protocol.md) |
| 常駐 worker で、試行錯誤を重くしない | `test_workers.py::test_resident_worker_serves_later_runs`（VM）、`tests/python/measure_stages.py collatz`（計測は C++ と Python の 2 言語だった Collatz。いまは Julia・R を含む） | [計測](Development.md#計測2026-11-03高速化後共通-vm) |
| 実際の Claude API で、Draft から Card を作って実行・検証できる | 記録のみ（1 題材）。実 API のテストは `TSUGUMI_LIVE_API=1` で `macOS/CoreTests` の `liveModelChangesRunsAndReports` | [実行記録](AgentRun.md)。テスト用モデルでの自動テストは [Development.md](Development.md#テスト) |
| 文書のとおりに Card を作って動かせる | `test_docs.py::test_card_guide_runs_as_written` | [はじめての Card](CardGuide.md) |
| AI にも同じ文書を渡せる（Markdown 版と `llms.txt`） | `test_docs.py::test_llms_txt_guides_agents_to_every_document`、`test_docs.py::test_markdown_pages_have_links_that_work_on_the_site`、`test_docs.py::test_pages_point_to_their_markdown_and_the_guide` | [llms.txt](llms.txt) |
| 文書のリンク・パス・一覧が実物と合っている | `test_docs.py::test_links_and_anchors_exist`、`test_docs.py::test_paths_named_in_backticks_exist`、`test_docs.py::test_protocol_page_is_generated_from_the_schema` | [test_docs.py](../tests/python/test_docs.py) |
| 何をハッカソン前に用意し、何を期間中に作ったか | テストなし。Git の履歴を見る | [制作範囲と事前準備](Provenance.md) |

できないことは[まだできないこと](Limitations.md)に分けています。
