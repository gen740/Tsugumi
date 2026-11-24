"""Code Card behaviour in the real environment (TSUGUMI_TEST_SHARED_VM=1; the
shared VM): backpressure stalls, cancellation, isolation, delegated limits,
Nix upstream references, stages and caching, the process a Card runs in,
skipped Cards and worker crashes. Most fixtures use small Python Cards.
The artifact probe drives the native ABI directly."""

from __future__ import annotations

import json
import os
import shutil
import time

import pytest

from conftest import real_environment
from harness.observations import decode_scalar, read_output
from harness.real_cards import (ENVIRONMENT_MAIN, NUM, ONE_MAIN, TXT, check_process_environment, log,
                                python_card, txt_main)
from projects import card, conn, run_and_wait, start_run, wait_for_log, wait_run, write_nix_project as write
from test_worker_loading import DIRECTORY, native_card
from tsugumi_client import RpcFailure, new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.Phase import Phase
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.RPC.RunEventValue import RunEventValue

pytestmark = pytest.mark.skipif(os.environ.get("TSUGUMI_TEST_SHARED_VM") != "1",
                                reason="needs the shared VM (TSUGUMI_TEST_SHARED_VM=1)")


@pytest.fixture(scope="module")
def env(tmp_path_factory):
    """One container for the module; each test swaps in its own project files."""
    root = tmp_path_factory.mktemp("cards") / "p"
    write(root, {"trigger": {"type": "trigger"}}, [card("start", "trigger")], [])
    e = real_environment(root)
    yield e
    e.cleanup()


def reload(c):
    """Retry until the guest sees the host write. Shared folders send no visibility event."""
    deadline = time.monotonic() + 60
    while True:
        snap = c.call("ProjectInspectRequest")
        try:
            return c.call("ProjectReloadRequest", baseRev=snap.rev, opId=new_id(), origin="test")
        except RpcFailure as e:
            if e.kind != "invalid_document" or time.monotonic() >= deadline:
                raise
            time.sleep(0.05)


def load(env, defs, cards, conns):
    """Replaces the project's documents and takes them in with Reload."""
    root = env.project
    shutil.rmtree(root / "definitions", ignore_errors=True)
    write(root, defs, cards, conns)
    c = env.client()
    reload(c)
    return c


def shown(c, run_id, name="show"):
    """The string a Card returned on its Txt output `out`."""
    return decode_scalar(read_output(c, run_id, card=name))


def test_artifacts_cross_virtiofs_with_read_only_grants(tmp_path):
    source = native_card(r"""
        tsugumi_artifact_id id;
        const auto directory = fs::path(create_artifact(call, id));
        fs::create_directory(directory / "nested");
        std::ofstream(directory / "nested/report.txt") << "shared VM";
        out.artifact(id);
    """, outputs=DIRECTORY)
    sink = native_card(r"""
        if (fs::exists("/project/.tsugumi/artifacts")) return 1;
        const auto directory = fs::path(open_artifact(call, in));
        if (directory.empty()) return 2;
        const auto member = directory / "nested/report.txt";
        std::ifstream report(member);
        std::string contents;
        std::getline(report, contents);
        if (contents != "shared VM") return 3;
        const int fd = open(member.c_str(), O_WRONLY | O_APPEND);
        const int write_error = errno;
        if (fd >= 0) {
          close(fd);
          return 4;
        }
        // The sealed inode can reject write-open before the mount check.
        if (write_error != EROFS && write_error != EACCES) return 5;
        if (chmod(member.c_str(), 0600) == 0 || errno != EROFS) return 6;
        if (mkdir((directory / "extra").c_str(), 0700) == 0 || errno != EROFS)
          return 7;
        out.number(contents.size());
    """, declarations="#include <cerrno>", inputs=["doc"], recordIO="on")
    project = tmp_path / "artifact-project"
    write(project, {"source": source, "show": sink},
          [card("source", "source"), card("show", "show")],
          [conn("source", "doc", "show", "doc")])
    guest = real_environment(project)
    try:
        with guest.client() as client:
            run_id, status = run_and_wait(client, "source")
            assert status.state == RunState.succeeded, (
                text(status.failure.diagnostic) if status.failure else None,
                log(client, run_id, "source", LogStream.build),
                log(client, run_id, "show", LogStream.build),
                log(client, run_id, "source", LogStream.standard_error),
                log(client, run_id, "show", LogStream.standard_error),
            )
            value = read_output(client, run_id, card="show")
            assert decode_scalar(value, "uint64") == len("shared VM")
        root = project / ".tsugumi" / "artifacts" / run_id / "source" / "@1" / "0"
        member = root / "nested" / "report.txt"
        assert member.read_text() == "shared VM"
        assert all(not node.stat().st_mode & 0o222 for node in (root, root / "nested", member))
    finally:
        guest.cleanup()


def test_backpressure_stall_ends_the_run(env):
    """A feedback producer fills wait.value while wait.condition awaits the
    same join, so the run reports the full queue."""
    defs = {
        "trigger": {"type": "trigger"},
        "seed": python_card(ONE_MAIN),
        "loop": {"type": "merge", "inputs": [{"name": "init"}, {"name": "again"}], "maxRuns": 1000},
        "step": python_card(ONE_MAIN, inputs=["x"], maxRuns=1000, reuseWorker=True),
        "wait": python_card(ONE_MAIN, inputs=["value", "condition"]),
        "gate": python_card(ONE_MAIN, inputs=["x"]),
    }
    c = load(env, defs,
             [card(name, name) for name in ("seed", "loop", "step", "wait", "gate")]
             + [card("start", "trigger")],
             [conn("seed", "out", "loop", "init"), conn("loop", "out", "step", "x"),
              conn("step", "out", "loop", "again"), conn("step", "out", "wait", "value"),
              conn("wait", "out", "gate", "x"), conn("gate", "out", "wait", "condition")])
    run_id, st = run_and_wait(c, "start")
    assert st.state == RunState.failed
    assert st.failure.kind == FailureKind.execution_limit
    assert {text(arg.key): text(arg.value) for arg in st.failure.args}.items() >= {
        "card": "wait", "port": "value"
    }.items()


def proc_count(env, pattern: str) -> int:
    """Processes in the environment whose command line contains `pattern`
    (NUL separators removed). Bash builtins only: the image has no grep."""
    # The pattern is written with a bracket so this probe never matches itself.
    glob = pattern[:-1] + "[" + pattern[-1] + "]"
    script = ("n=0; for f in /proc/[0-9]*/cmdline; do c=$(< \"$f\") 2>/dev/null; "
              f"case \"$c\" in *{glob}*) n=$((n+1));; esac; done; echo $n")
    out = env.exec(script).strip().splitlines()
    return int(out[-1] if out else "0")


# Children, one of them in a session of its own, and then the Card waits.
HANG_MAIN = """import subprocess, time

def run(input):
    subprocess.Popen(["sleep", "3141"], start_new_session=True)
    subprocess.Popen(["sleep", "2718"])
    subprocess.Popen(["sleep", "2719"])
    print("descendants-ready", flush=True)
    while True:
        time.sleep(60)
"""


@pytest.mark.parametrize("sandbox", [True, False])
def test_cancel_ends_every_descendant(env, sandbox):
    defs = {"trigger": {"type": "trigger"},
            "hang": python_card(HANG_MAIN, tools="[ pkgs.coreutils ]", sandbox={"filesystem": sandbox})}
    c = load(env, defs, [card("start", "trigger"), card("hang", "hang")], [])
    run_id = start_run(c, "start")
    try:
        wait_for_log(c, run_id, "hang", 1, lambda log: "descendants-ready" in text(log.text or b""))
        c.call("RunCancelRequest", runId=run_id)
        assert wait_run(c, run_id).state in (RunState.cancelled, RunState.failed)
        # The controller exposes a guest process snapshot, without exit events
        # for adopted descendants inside the shared VM.
        deadline = time.monotonic() + 60
        while proc_count(env, "sleep271") or proc_count(env, "sleep3141"):
            assert time.monotonic() < deadline, "a descendant survived cancellation"
            time.sleep(0.05)
    finally:
        c.call("RunCancelRequest", runId=run_id)


ISOLATION_BODY = """
    import os, socket
    found = []
    def network():
        try:
            socket.create_connection(("1.1.1.1", 80), timeout=3).close()
            return True
        except OSError:
            return False
    CHECKS
    RESULT = ";".join(found)"""

SANDBOXED = """for name in ("/project/tsugumi.json", "/project/definitions", "/control/server.ready", "/persist"):
        if os.path.exists(name):
            found.append(name + " visible")
    if network():
        found.append("network reachable")"""

OPEN = """if network():
        found.append("network reachable")
    if os.path.exists("/proc/1/root/control/server.ready"):
        found.append("control visible through proc")
    try:
        open("/control/server.ready").read(1)
        found.append("token readable")
    except OSError:
        pass
    if os.getuid() == 65534:
        found.append("uid 65534")"""


def test_isolation(env):
    probe = python_card(txt_main(ISOLATION_BODY.replace("CHECKS", SANDBOXED)), outputs=TXT, recordIO="on")
    opened = python_card(txt_main(ISOLATION_BODY.replace("CHECKS", OPEN)), outputs=TXT, recordIO="on",
                         sandbox={"filesystem": False, "pid": False, "ipc": False, "identity": False})
    defs = {"trigger": {"type": "trigger"}, "probe": probe, "open": opened}
    c = load(env, defs, [card("start", "trigger"), card("probe", "probe"), card("open", "open")], [])
    run_id, st = run_and_wait(c, "start")
    assert st.state == RunState.succeeded, {k: log(c, run_id, k, 2)[-600:] for k in ("probe", "open")}
    assert shown(c, run_id, "probe") == "" and shown(c, run_id, "open") == ""


OOM_MAIN = """from card import Output
from t.Num import NumT

def run(input):
    buffer = bytearray(128 << 20)
    buffer[::4096] = b"x" * len(buffer[::4096])
    return Output(out=NumT(value=len(buffer)))
"""


def test_card_limits_are_delegated_and_released(env):
    terminal_probe = (
        'root=/sys/fs/cgroup/tsugumi/$TSUGUMI_PROJECT_ID; found=0; '
        'for group in "$root"/tsugumi-card-*; do '
        '[ -d "$group" ] || continue; '
        'printf "card-limits %s / %s\\n" "$(<"$group/cpu.max")" "$(<"$group/memory.max")"; '
        'found=1; done; [ "$found" = 1 ] || echo no-card-cgroup'
    )
    limits = {"limits": {"cpu": 0.5, "memoryMiB": 32}}
    resident = python_card(ONE_MAIN, sandbox=limits, reuseWorker=True)
    connection = load(env, {"limited": resident}, [card("limited", "limited")], [])
    _, status = run_and_wait(connection, "limited")
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    assert "card-limits 500000 1000000 / 33554432" in env.exec(terminal_probe)

    oom = python_card(OOM_MAIN, sandbox=limits, reuseWorker=True, timeoutSeconds=60)
    # The old resident must be gone before running the replacement. Keep reuse
    # enabled so no unused standby worker can hide a leaked cgroup.
    connection = load(env, {"limited": oom}, [card("limited", "limited")], [])
    assert "no-card-cgroup" in env.exec(terminal_probe).splitlines()
    run_id, status = run_and_wait(connection, "limited")
    assert status.state == RunState.failed
    invocation, = connection.call("RunInvocationsRequest", runId=run_id, card=path("limited")).records
    assert invocation.failure.kind == FailureKind.worker_failed
    assert "no-card-cgroup" in env.exec(terminal_probe).splitlines()


GREETING_MAIN = """from card import Output
from t.Num import NumT

def run(input):
    assert GREETING == "hello b", GREETING
    return Output(out=NumT(value=2))
"""


def test_nix_connected_exports(env):
    a = python_card(ONE_MAIN, extra='exports = { greet = name: "hello ${name}"; }; ')
    b = python_card(GREETING_MAIN, inputs=["x"], values={"GREETING": 'inputs.a.greet "b"'})
    defs = {"trigger": {"type": "trigger"}, "a": a, "b": b}
    c = load(env, defs, [card("start", "trigger"), card("a", "a"), card("b", "b")], [conn("a", "out", "b", "x")])
    run_id, st = run_and_wait(c, "start")
    assert st.state == RunState.succeeded, log(c, run_id, "b", 2)


def test_stages_and_cache(env):
    defs = {"trigger": {"type": "trigger"}, "one": python_card(ONE_MAIN)}
    c = load(env, defs, [card("start", "trigger"), card("one", "one")], [])
    c.open()
    def collect_phases():
        run_id = start_run(c, "start")
        phases = set()

        def finished(note):
            if note.valueType != NotificationPayload.RunEvent or text(note.value.runId) != run_id:
                return False
            event = note.value
            if event.valueType == RunEventValue.RunCardChanged:
                invocation = event.value.invocation
                if [text(segment) for segment in invocation.card.segments] == ["one"]:
                    phases.add(invocation.phase)
            return event.valueType == RunEventValue.RunFinished

        c.wait_notification(finished)
        assert wait_run(c, run_id).state == RunState.succeeded
        record, = c.call("RunInvocationsRequest", runId=run_id, card=path("one")).records
        return phases, record.buildCached

    phases, _ = collect_phases()
    assert {Phase.execute, Phase.save} <= phases
    phases, cached = collect_phases()
    assert Phase.build not in phases
    assert cached


# Libraries that keep settings under the home folder (astropy, matplotlib) or
# ask for the current directory (torch) do so while the module is imported.
LOAD_MAIN = """import json, os
from pathlib import Path
from card import Output
from t.Txt import TxtT

HOME = Path.home()
(HOME / ".config").mkdir(parents=True, exist_ok=True)
(HOME / ".config" / "probe").write_text("ok")
AT_LOAD = os.getcwd()

def run(input):
    return Output(out=TxtT(value=json.dumps({"home": str(HOME), "cwd": AT_LOAD})))
"""


def test_python_card_has_a_home_and_a_directory_while_it_loads(env):
    defs = {"trigger": {"type": "trigger"}, "show": python_card(LOAD_MAIN, outputs=TXT, recordIO="on")}
    c = load(env, defs, [card("start", "trigger"), card("show", "show")], [])
    run_id, st = run_and_wait(c, "start")
    assert st.state == RunState.succeeded, log(c, run_id, "show", 2)
    seen = json.loads(shown(c, run_id))
    # The working directory is HOME, for the life of the process.
    assert seen["home"] == seen["cwd"], seen


def test_python_card_environment_lives_with_its_process(env):
    defs = {"trigger": {"type": "trigger"},
            "show": python_card(ENVIRONMENT_MAIN, outputs=TXT, reuseWorker=True, recordIO="on")}
    c = load(env, defs, [card("start", "trigger"), card("show", "show")], [])
    seen = []
    for _ in range(2):
        run_id, st = run_and_wait(c, "start")
        assert st.state == RunState.succeeded, log(c, run_id, "show", 2)
        seen.append(json.loads(shown(c, run_id)))
    reused, = c.call("RunInvocationsRequest", runId=run_id, card=path("show")).records
    assert reused.workerReused
    check_process_environment(*seen)


def test_skipped_card_is_not_evaluated(env):
    """The consumer never evaluates Nix when its connected output is omitted."""
    broken = {"type": "code", "inputs": [{"name": "in"}], "_outputs": NUM,
              "_nix": '{ params, inputs }: { run = builtins.throw "must never be evaluated"; }'}
    omit = python_card("from card import Output\n\ndef run(input):\n    return Output()\n",
                       outputs=NUM.replace(" (required)", ""))
    defs = {"trigger": {"type": "trigger"}, "omit": omit, "broken": broken}
    c = load(env, defs,
             [card("start", "trigger"), card("omit", "omit"), card("never", "broken")],
             [conn("omit", "out", "never", "in")])
    snap = c.call("ProjectInspectRequest")
    acc = c.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="t", card=path("start"), maxConcurrency=2)
    run_id = text(acc.run.runId)
    st = wait_run(c, run_id)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    assert c.call("RunRecordRequest", runId=run_id, card=path("never")).status.state == CardState.skipped


CRASH_MAIN = """import os, signal

def run(input):
    os.kill(os.getpid(), signal.SIGKILL)
"""


def test_worker_crash_then_edit_and_rerun(env):
    defs = {"trigger": {"type": "trigger"}, "crash": python_card(CRASH_MAIN)}
    c = load(env, defs, [card("start", "trigger"), card("crash", "crash")], [])
    run_id, st = run_and_wait(c, "start")
    rec = c.call("RunRecordRequest", runId=run_id, card=path("crash")).status
    assert st.state == RunState.failed and rec.state == CardState.failed
    assert rec.failure.kind == FailureKind.worker_failed
    # After the crash the environment keeps working: fix the source and run again.
    c = load(env, {**defs, "crash": python_card(ONE_MAIN)}, [card("start", "trigger"), card("crash", "crash")], [])
    _, st2 = run_and_wait(c, "start")
    assert st2.state == RunState.succeeded, st2.failure and text(st2.failure.diagnostic)
