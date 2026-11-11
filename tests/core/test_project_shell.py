"""Commands use the public project RPC and the launcher's explicit shell opt-in."""

from concurrent.futures import ThreadPoolExecutor
import os
import time

import pytest

from tsugumi_client import RpcFailure, new_id


def shell(client, command, **fields):
    return client.call("ProjectShellRequest", opId=fields.pop("opId", new_id()), command=command, **fields)


def test_shell_is_disabled_without_the_isolated_launchers_opt_in(client):
    """A normal host daemon never silently falls back to a host shell."""
    with pytest.raises(RpcFailure) as failure:
        shell(client, "printf should-not-run")
    assert failure.value.kind == "unsupported"


@pytest.fixture
def shell_daemon(core_factory):
    if os.geteuid() == 0:
        pytest.skip("project shells must use an unprivileged project user")
    return core_factory(args=["--project-shell"])


def test_shell_uses_project_user_and_cwd_and_returns_both_streams(shell_daemon, project):
    """A command returns its separate stdout, stderr and exit status in the project environment."""
    result = shell(shell_daemon.connect(), "pwd; id -u; printf diagnostic >&2; exit 7")
    assert bytes(result.stdout).decode().splitlines() == [str(project.resolve()), str(os.geteuid())]
    assert bytes(result.stderr) == b"diagnostic"
    assert result.exitCode == 7
    assert result.signal == 0 and not result.cancelled and not result.timedOut


def test_binary_output_is_preserved(shell_daemon):
    """Shell output is returned as bytes, including NUL and non-UTF-8 data."""
    result = shell(shell_daemon.connect(), "printf '\\000\\377'; printf '\\376' >&2")
    assert bytes(result.stdout) == b"\0\xff"
    assert bytes(result.stderr) == b"\xfe"


@pytest.mark.parametrize("command", ["exec 1>&- 2>&-; sleep 20", "trap '' TERM; while :; do :; done"])
def test_timeout_works_after_pipes_close_or_term_is_ignored(shell_daemon, command):
    """A command cannot evade its deadline by closing output or ignoring TERM."""
    started = time.monotonic()
    result = shell(shell_daemon.connect(), command, timeoutMs=100)
    assert result.timedOut
    assert result.exitCode == -1 and result.signal > 0
    assert time.monotonic() - started < 3


def test_output_without_newlines_is_bounded_and_does_not_block_timeout(shell_daemon):
    """Flooding stderr without newlines is truncated while cancellation remains responsive."""
    result = shell(shell_daemon.connect(), "while :; do printf '012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789' >&2; done", timeoutMs=500)
    assert result.timedOut and result.stderrTruncated
    assert len(result.stderr) == 262144


def test_operation_receipts_prevent_reexecution_after_restart(shell_daemon, project):
    """Retrying an operation after restart returns its result without repeating its side effect."""
    client = shell_daemon.connect()
    operation = new_id()
    command = "printf x >> executions; printf complete"
    assert bytes(shell(client, command, opId=operation).stdout) == b"complete"
    shell_daemon.stop()
    shell_daemon.start()
    client = shell_daemon.connect()
    assert bytes(shell(client, command, opId=operation).stdout) == b"complete"
    assert (project / "executions").read_text() == "x"
    with pytest.raises(RpcFailure) as failure:
        shell(client, "printf different", opId=operation)
    assert failure.value.kind == "operation_conflict"


def test_disconnecting_cancels_the_command_and_saves_the_result(shell_daemon, project):
    """Cancelling an Agent connection stops its shell and leaves a queryable operation result."""
    client = shell_daemon.connect()
    observer = shell_daemon.connect()
    operation = new_id()
    with ThreadPoolExecutor(max_workers=1) as threads:
        pending = threads.submit(shell, client, "printf ready > ready; sleep 20; printf escaped > escaped", opId=operation)
        deadline = time.monotonic() + 3
        while not (project / "ready").exists():
            assert time.monotonic() < deadline
            time.sleep(0.01)
        client.close()
        with pytest.raises((ConnectionError, OSError)):
            pending.result(timeout=3)
    deadline = time.monotonic() + 3
    while True:
        receipt = observer.call("OperationGetRequest", opId=operation).receipt
        if receipt is not None and hasattr(receipt.result, "cancelled"):
            assert receipt.result.cancelled
            break
        assert time.monotonic() < deadline
        time.sleep(0.01)
    assert not (project / "escaped").exists()


@pytest.mark.parametrize("command,fields", [("", {}), ("x\0y", {}), ("true", {"timeoutMs": 0}), ("true", {"timeoutMs": 30001})])
def test_invalid_shell_requests_do_not_start_a_command(shell_daemon, command, fields):
    """Invalid command strings and deadlines fail before execution."""
    with pytest.raises(RpcFailure) as failure:
        shell(shell_daemon.connect(), command, **fields)
    assert failure.value.kind == "invalid_request"


def test_a_crash_never_reexecutes_an_operation_with_an_unknown_outcome(shell_daemon, project):
    """After a daemon crash, an uncertain command is reported instead of being executed again."""
    client = shell_daemon.connect()
    operation = new_id()
    # The command kills only its parent: this fixture's dedicated daemon.
    # Closing the client first would exercise cancellation instead of a crash.
    command = 'printf x >> crash-executions; kill -KILL "$PPID"'
    with pytest.raises((ConnectionError, OSError)):
        shell(client, command, opId=operation)
    shell_daemon.stop(crash=True)
    shell_daemon.start()
    with pytest.raises(RpcFailure) as failure:
        shell(shell_daemon.connect(), command, opId=operation)
    assert failure.value.kind == "interrupted"
    assert (project / "crash-executions").read_text() == "x"


def test_running_shell_receipts_are_pending_and_duplicate_calls_join(shell_daemon, project):
    """An in-flight command stays pending and duplicate callers share its final result."""
    from Tsugumi.Data.OperationState import OperationState
    first = shell_daemon.connect()
    second = shell_daemon.connect()
    observer = shell_daemon.connect()
    operation = new_id()
    command = "printf x >> joined-executions; sleep 0.3; printf finished"
    with ThreadPoolExecutor(max_workers=2) as threads:
        running = threads.submit(shell, first, command, opId=operation)
        deadline = time.monotonic() + 3
        while not (project / "joined-executions").exists():
            assert time.monotonic() < deadline
            time.sleep(0.01)
        assert observer.call("OperationGetRequest", opId=operation).state == OperationState.pending
        joined = threads.submit(shell, second, command, opId=operation)
        assert bytes(running.result(timeout=3).stdout) == b"finished"
        assert bytes(joined.result(timeout=3).stdout) == b"finished"
    assert (project / "joined-executions").read_text() == "x"
