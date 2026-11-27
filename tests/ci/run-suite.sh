#!/usr/bin/env bash
# One command list for Actions and ci-local. x86 Linux owns shared rules in CI;
# the local Linux pass represents that full job even on an ARM Mac.
# --list emits argv as NUL-delimited fields and commands for the static audit.
set -euo pipefail
list=false
if [[ "$1" == --list ]]; then list=true; shift; fi
suite=$1
system=$2
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
run() {
  if $list; then
    printf '%s\0' "$@" ''
  else
    "$root/build-ci/bin/tsugumi-test-env" "$@"
  fi
}
bin="$root/build-ci/libexec/tsugumi-tests"
full=false
if [[ "$system" == x86_64-linux || "${TSUGUMI_FULL_RULES:-0}" == 1 ]]; then full=true; fi
# Keep the native branches of shell isolation, PTY, credential storage,
# locking, environment/process lifecycle, path handling and RPC transport.
native_settings=(
  'NativeEnvironment.*' 'NativeTerminalTest.*' 'AttachedFolderTest.*' 'HostFormat.*'
  'EnvironmentAdapterTest.ShutdownPreservesReplacementControlFiles'
  'EnvironmentAdapterTest.VerifyStoreCancelsAnInProgressStart'
  'BinaryCaches.NixDownloadsFromAPrivateCacheWithTheHostSnapshot'
  'ConversationStoreTest.ProjectDatabasesKeepTheirLocksAndRecoverySeparateFromHome'
  'ConversationStoreTest.IncrementalEventsCopyOnlyChangedRows'
  'ConversationStoreTest.TurnPreparationLoadsExclusionsWithoutTheGate'
  'AgentPresentation.AFailedHelperMarksItsStepFailed'
  'ConversationStoreTest.KnownConversationsRemainReadableWhileAnotherProjectIsLocked'
  'ConversationStoreTest.ProjectStateLinksAndForeignConversationIdentitiesStayRejected'
  'ConversationStoreTest.RecreatedFolderReplacesEvenAnOpenedIdentity'
)
native_agent=(
  'EnvironmentFixture.*' 'RealEnvironmentFixture.*' 'AgentShellProbe.*'
  'AgentFixture.HostUnixAndWebSocketReturnTypedReplies'
  'AgentFixture.HostRejectsTheExternalAgentCredentialOnBothTransports'
  'AgentFixture.UnixAndWebSocketExposeTheSameAgentProtocol'
  'AgentFixture.ProviderSetAndStatusNeverReturnTheSecretAndKeepItInHostd'
  'ViewRpcFixture.*'
  'ConversationStoreFixture.ASecondHostCannotWriteTheSameProjectUntilTheFirstStops'
  'ConversationStoreFixture.ProjectStateSymlinksAreRejected'
  'AgentProjectFixture.*Shell*' 'NestedFileFixture.NestedScratchIsPrivateToEachHelperAtEveryDepth'
  '*/NestedHelperFiles.ShellStaysInsideEveryParentsExactSets/*'
  'AgentExclusionRPC.ShellProfileDeniesExcludedProjectDemoAndMountedPaths'
  'AgentProjectFixture.CopyRefusesAnAttachedRootReplacedAfterItsReadGrant'
  'SkillsFixture.SwiftUnicodeNamesWhitespaceAndCRLFBehavior'
  'SkillsFixture.StrictSizeBoundaryAndUnreadableFilesAndFolders'
  'BrokerFixture.*Handshake*' 'BrokerFixture.WrongWorkerKeyIsRejectedBeforeAnyApplicationRequest'
  'BrokerFixture.FragmentedFramesWaitForTheCompleteRequest'
  'BrokerFixture.CatalogAndCancelProceedWhileWorkerRepliesAreBlocked'
)
# Python suites that exercise native workers, paths or transports keep their
# own marker. Shared rules and the rest of each mixed fixture run on x86 Linux.
core_args=(--ignore=tests/core/integration)
if ! $full; then
  core_args+=(-m native_core)
fi
gtest() {
  local target=$1
  shift
  if $full; then
    run "$bin/$target"
  else
    local IFS=:
    run "$bin/$target" "--gtest_filter=$*"
  fi
}
if ! $list; then
  case "$suite" in
    core|adapters|demos)
      TSUGUMI_DEMOS_DIR=$(nix --extra-experimental-features 'nix-command flakes' build --no-link --print-out-paths .#demos)
      export TSUGUMI_DEMOS_DIR
      ;;
  esac
fi
case "$suite" in
  agent)
    if $full; then run "$bin/tsugumi_input_types_tests"; fi
    run "$bin/tsugumi_peer_tls_tests"
    gtest tsugumi_host_settings_tests "${native_settings[@]}"
    gtest tsugumi_agent_rpc_tests "${native_agent[@]}"
    if $full; then run python3 -m pytest -q tests/python/test_host_projects_rpc.py; fi
    ;;
  core)
    if [[ "$system" == aarch64-darwin ]]; then
      run "$bin/tsugumi-view-load-tests"
      run "$bin/tsugumi-view-decode-tests"
    fi
    (export TSUGUMI_RPC_TRACE=1; run "$bin/tsugumi_rpc_trace_tests")
    run "$bin/tsugumi_rpc_frame_tests"
    run "$bin/tsugumi_rpc_cli_tests"
    gtest tsugumi_remote_result_tests 'ShellTest.*' 'RunScratch.*'
    # Evaluation cache invalidation reads real dependencies and filesystem state.
    run "$bin/tsugumi_definition_evaluation_tests"
    run python3 -m pytest -q -n "${TSUGUMI_PYTEST_WORKERS:-2}" --dist load --durations=40 "${core_args[@]}" tests/core
    run "$bin/tsugumi_workerd_tests"
    run "$bin/tsugumi_native_contract_tests"
    run "$bin/tsugumi_execution_service_tests"
    run "$bin/tsugumi_worker_staging_tests"
    for scenario in large sparse shrink-stream shrink-sparse; do
      run "$bin/tsugumi_file_transfer_tests" "$scenario"
    done
    ;;
  host)
    if [[ "$system" == aarch64-darwin ]]; then
      run python3 -m pytest -q tests/core/integration/test_host_isolation.py tests/core/integration/test_host_sandbox.py \
        tests/core/integration/test_nix_build_log.py tests/core/integration/test_definition_evaluation.py \
        tests/core/integration/test_cache_server.py
    else
      host_args=(--ignore=tests/core/integration/test_host_sandbox.py)
      if ! $full; then host_args+=(--ignore=tests/core/integration/test_definition_evaluation.py); fi
      run python3 -m pytest -q "${host_args[@]}" tests/core/integration
    fi
    ;;
  adapters)
    if $full; then
      if ! $list; then
        nix build .#ci-mock --out-link build-ci-mock
        export TSUGUMI_MOCK_DAEMON="$root/build-ci-mock/libexec/tsugumi-tests/tsugumid-mock"
      fi
      run python3 -m pytest -q tests/python/test_claude_code_plugin.py tests/python/test_web_server.py tests/python/test_cli.py
    else
      run python3 -m pytest -q -m package_smoke tests/python/test_web_server.py tests/python/test_cli.py
    fi
    ;;
  kit)
    run nix build --no-link ".#checks.$system.pinned-kit"
    ;;
  demos)
    run "${3:?supply the demo Python executable from .#ci-python}" -m pytest -q -m 'not static' -n "${TSUGUMI_PYTEST_WORKERS:-2}" --dist loadfile \
      --durations=40 tests/python/test_demo*.py tests/python/test_collatz_compare.py
    ;;
  *) echo "Unknown suite: $suite" >&2; exit 2 ;;
esac
