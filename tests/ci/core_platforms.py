"""Core cases that exercise a native boundary on each CI platform."""

from pathlib import PurePosixPath


# Files that exercise Unix/WebSocket transport, platform paths and permissions,
# process signals and cleanup, native worker loading, or machine capabilities.
NATIVE_CORE_FILES = {
    "test_frame_fuzz.py", "test_input_file_paths.py", "test_local_workerd.py",
    "test_project_shell.py", "test_rpc_smoke.py", "test_source_transfers.py",
    "test_system.py", "test_test_isolation.py", "test_transport.py",
    "test_untrusted_project.py", "test_worker_loading.py",
}
NATIVE_CORE_CASES = {
    "test_contract.py": {"test_session_requires_hello_and_matching_project"},
    "test_rpc_execution.py": {
        "test_cancel_a_worker_waiting_to_return",
        "test_timeout_ends_a_worker_that_ignores_cancellation",
        "test_build_wait_can_be_interrupted",
    },
    "test_run_scratch_usage.py": {"test_unreadable_scratch_is_listed_and_explicitly_deletable"},
    "test_remote_recovery.py": {
        "test_stop_is_accepted_while_early_build_catalog_is_pending",
        "test_stop_cancels_remote_job_before_the_pending_reply",
        "test_network_cut_within_same_core_life_resumes",
        "test_core_shutdown_releases_an_unresponsive_gateway",
    },
}


def is_native_core(path, name):
    file = PurePosixPath(path).name
    return path.startswith("tests/core/") and (
        file in NATIVE_CORE_FILES or name in NATIVE_CORE_CASES.get(file, ())
    )
