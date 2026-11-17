#!/usr/bin/env bash
# One command list for Actions and ci-local. x86 Linux owns shared rules in CI;
# the local Linux pass represents that full job even on an ARM Mac.
set -euo pipefail
suite=$1
system=$2
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
run() { "$root/build-ci/bin/tsugumi-test-env" "$@"; }
bin="$root/build-ci/libexec/tsugumi-tests"
full=false
if [[ "$system" == x86_64-linux || "${TSUGUMI_FULL_RULES:-0}" == 1 ]]; then full=true; fi
# Socket-free rules: keep native transport, processes, filesystem and privacy on
# every target. No wildcard for all Host* or Agent* suites.
pure='Layout.*:HostLayout.*:HostLayoutSaving.*:HostCardPlacement.*:DocumentConnect.*:DocumentTemplate.*:DocumentCopy.*:DocumentSubgraph.*:HostPermissionKinds.*:AgentPresentation.*:ThoughtScheduleTest.*:AgentCatalog.*:AgentPolicy.*:AgentApprovalDescription.*:AgentSavePolicy.*:AgentContext.*:ContextBudget.*:HostContextWindow.*:HostPolicyTest.*:HostSaveInspection.*:HostSettingsStore.*:AgentContextMeasure.*:ProjectEdit.*:ProjectEditDiagnostics.*:ProjectOperation*.*:FileMiss.*:VersionNote.*:VersionText.*:SchemaEncode.*:HelperScope.*:HelperGraphScope.*:HelperCapability.*:VMResetPolicy.*:WorkerCondition.*:Table/WorkerCondition*.*'
host_filter='*'
marker='not environment'
sdk_marker='not static'
if ! $full; then
  host_filter="*-$pure"
  marker='not environment and not shared_rules'
  sdk_marker='not static and (not sdk_pure or package_smoke)'
fi
case "$suite" in
  agent)
    if $full; then run "$bin/tsugumi_input_types_tests"; fi
    run "$bin/tsugumi_peer_tls_tests"
    run "$bin/tsugumi_host_settings_tests" "--gtest_filter=$host_filter"
    run "$bin/tsugumi_agent_rpc_tests" "--gtest_filter=$host_filter"
    ;;
  core)
    run "$bin/tsugumi_rpc_cli_tests"
    remote_filter='*'
    if ! $full; then remote_filter='*-InitialDocument.*:OpaqueMetadata.*:LayoutBoundary.*:CardIdentity.MetadataIsNonComputationalOnlyAtDefinitionPaths'; fi
    run "$bin/tsugumi_remote_result_tests" "--gtest_filter=$remote_filter"
    if $full; then run "$bin/tsugumi_worker_condition_tests"; fi
    # Evaluation cache invalidation reads real dependencies and filesystem state.
    run "$bin/tsugumi_definition_evaluation_tests"
    run python3 -m pytest -q --core-report -m "$marker" -n "${TSUGUMI_PYTEST_WORKERS:-2}" --dist load --durations=40 tests/core
    run "$bin/tsugumi_workerd_tests"
    run "$bin/tsugumi_native_contract_tests"
    run "$bin/tsugumi_execution_service_tests"
    run "$bin/tsugumi_worker_staging_tests"
    for scenario in large sparse shrink-stream shrink-sparse; do
      run "$bin/tsugumi_file_transfer_tests" "$scenario"
    done
    ;;
  adapters)
    if $full; then
      run python3 -m pytest -q tests/python/test_claude_code_plugin.py tests/python/test_web_server.py tests/python/test_cli.py
    else
      run python3 -m pytest -q -m package_smoke tests/python/test_web_server.py tests/python/test_cli.py
    fi
    ;;
  sdk)
    python=$3
    run "$python" -c 'import matplotlib, plotly, altair'
    run "$python" -m pytest -q -m "$sdk_marker" -n "${TSUGUMI_PYTEST_WORKERS:-2}" --dist loadfile --durations=40 tests/python \
      --ignore=tests/python/test_sdk_examples.py --ignore=tests/python/test_csharp_sdk.py \
      --ignore=tests/python/test_claude_code_plugin.py --ignore=tests/python/test_web_server.py --ignore=tests/python/test_cli.py
    TSUGUMI_TEST_HOST_SANDBOX=1 run "$python" -m pytest -q tests/python/test_csharp_sdk.py
    run "$python" -m pytest -q tests/python/test_sdk_examples.py
    ;;
  *) echo "Unknown suite: $suite" >&2; exit 2 ;;
esac
