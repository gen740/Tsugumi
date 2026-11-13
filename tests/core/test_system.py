"""A Card's "system": built and run for another Nix system under an emulator
the kernel has registered (binfmt_misc). Which systems a Card may name (Cards
of every language may), and what a run says when no emulator is there.

Emulated runs themselves are checked in the shared VM on a Mac, with Rosetta
and with qemu (TSUGUMI_VM_X86=qemu): docs/Development.md#tests.
"""
from __future__ import annotations

import platform
import socketserver
import sys
import tempfile
import threading
from pathlib import Path

import pytest

from harness import remote_worker as R
from harness.daemon import Daemon
from harness.scenario import run
from projects import MSG_FBS, card, conn, run_and_wait, write
from tsugumi_client import RpcFailure, path, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.ExecutionServiceKind import ExecutionServiceKind

MACHINE = {"arm64": "aarch64", "aarch64": "aarch64", "x86_64": "x86_64", "amd64": "x86_64"}[platform.machine().lower()]
NATIVE = f"{MACHINE}-{'darwin' if sys.platform == 'darwin' else 'linux'}"
FOREIGN = "x86_64-linux" if MACHINE == "aarch64" else "aarch64-linux"


def emulator_registered(system: str) -> bool:
    """Whether the kernel runs this system's ELF executables (as the core reads it)."""
    machine = {"x86_64-linux": "3e00", "aarch64-linux": "b700"}[system]
    root = Path("/proc/sys/fs/binfmt_misc")
    status = root / "status"
    if not status.is_file() or status.read_text().strip() != "enabled":
        return False
    for entry in root.iterdir():
        if entry.name in ("register", "status"):
            continue
        try:
            lines = entry.read_text().splitlines()
        except OSError:
            continue
        magic = next((line[6:] for line in lines if line.startswith("magic ")), "")
        interpreter = next((line[12:] for line in lines if line.startswith("interpreter ")), "")
        if "enabled" in lines and interpreter and magic.startswith("7f454c4602") and magic[36:40] == machine:
            return True
    return False


def diagnostics(c, card_id: str) -> list[str]:
    snapshot = c.call("ProjectInspectRequest")
    service = [text(d.code) for d in snapshot.diagnostics
               if d.card is not None and [text(s) for s in d.card.segments] == [card_id]]
    for r in snapshot.resolution:
        if [text(s) for s in r.card.segments] == [card_id]:
            return service + [text(d.code) for d in r.diagnostics]
    raise KeyError(card_id)


@pytest.fixture
def registered_remote(monkeypatch, resource_limits):
    class Catalog(socketserver.StreamRequestHandler):
        def handle(self):
            assert R.read(self.rfile).payload.bodyType == R.RequestBody.CatalogRequest
            caps = R.CapabilitiesT(serverId="2" * 64, nativeSystem=FOREIGN, systems=[FOREIGN],
                                   resourceLimits=resource_limits, maxJobs=8, workerContract=3)
            self.wfile.write(R.frame(R.reply(R.ReplyBody.Catalog, R.CatalogT(
                services=[R.TargetT(id="cluster", available=True, capabilities=caps)]))))

    with tempfile.TemporaryDirectory(prefix="tsg-broker-", dir="/tmp") as directory:
        endpoint = str(Path(directory) / "socket")
        monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER", "unix:" + endpoint)
        monkeypatch.delenv("TSUGUMI_EXECUTION_BROKER_TOKEN_FILE", raising=False)
        with socketserver.UnixStreamServer(endpoint, Catalog) as server:
            thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.02})
            thread.start()
            try:
                yield
            finally:
                server.shutdown()
                thread.join()


@pytest.mark.parametrize("resource_limits,settings,message", [
    (False, {"executionService": "cluster"}, None),
    (True, {"executionService": "local", "executionResources": {"cpuMillis": 500, "memoryBytes": 2 ** 28}},
     "requires an external execution service"),
    (True, {"executionService": "cluster", "system": NATIVE.split("-")[0] + "-linux",
      "executionResources": {"cpuMillis": 500, "memoryBytes": 2 ** 28}}, "does not support system"),
    (True, {"executionService": "cluster", "executionResources": {"cpuMillis": 500, "memoryBytes": 2 ** 28}}, None),
    (False, {"executionService": "cluster", "executionResources": {"cpuMillis": 500, "memoryBytes": 2 ** 28}},
     "cannot enforce resource limits"),
], ids=["unlimited", "local-resources", "wrong-system", "remote-native", "unsupported-limits"])
def test_external_service_validation_uses_host_catalog(
        project, core_factory, registered_remote, resource_limits, settings, message):
    """The service's advertised system and resource rules apply before accepting a run."""
    definition = {"language": "bash", "_outputs": MSG_FBS,
                  "_nix": '{ pkgs, inputs, params }: { run = "true"; }', **settings}
    write(project, {"work": definition}, [card("work", "work")], [])
    client = core_factory().connect()
    services = client.call("ProjectInspectRequest").executionServices
    assert [text(service.id) for service in services] == ["local", "cluster"]
    assert services[1].kind == ExecutionServiceKind.remote_daemon
    assert text(services[1].nativeSystem) == FOREIGN
    assert services[1].resourceLimits == resource_limits
    assert services[1].maxJobs == 8
    validation = client.call("ProjectValidateRequest")
    if message is None:
        assert validation.valid, [text(diagnostic.message) for diagnostic in validation.diagnostics]
    else:
        assert not validation.valid
        assert any(message in text(diagnostic.message) for diagnostic in validation.diagnostics)
        with pytest.raises(RpcFailure) as failure:
            run(client)
        assert failure.value.kind == "validation_failed"
        assert not client.call("RunHistoryRequest").runs


def test_snapshot_lists_the_systems_the_local_service_can_run(client):
    """The local service describes the daemon's actual execution environment."""
    services = client.call("ProjectInspectRequest").executionServices
    assert [text(service.id) for service in services] == ["local"]
    service = services[0]
    assert text(service.nativeSystem) == NATIVE
    assert not service.resourceLimits
    assert service.maxJobs is None
    expected = {NATIVE}
    if sys.platform == "linux" and emulator_registered(FOREIGN):
        expected.add(FOREIGN)
    assert {text(system) for system in service.systems} == expected


@pytest.mark.parametrize("definition", [
    {"language": "bash", "executionService": "unavailable", "_outputs": MSG_FBS,
     "_nix": '{ pkgs, inputs, params }: { run = "true"; }'},
    {"template": "trigger", "executionService": "local"},
], ids=["unknown-service", "built-in-card"])
def test_invalid_execution_service_selection_prevents_the_run(project, core_factory, definition):
    """An invalid selection is reported before a run can start."""
    write(project, {"work": definition}, [card("work", "work")], [])
    client = core_factory().connect()
    assert "invalid_setting" in diagnostics(client, "work")
    with pytest.raises(RpcFailure) as error:
        run(client)
    assert error.value.kind == "validation_failed"
    assert not client.call("RunHistoryRequest").runs


def test_which_systems_and_cards(tmp_path):
    """Supported target systems are accepted for each Card language."""
    root = tmp_path / "p"
    defs = {"py": {"language": "python"}, "cc": {"language": "cpp"}, "sh": {"language": "bash"},
            "r": {"language": "r"}}
    cards = [card("unknown", "sh", system="riscv64-linux"), card("py", "py", system=FOREIGN),
             card("cc", "cc", system=FOREIGN), card("sh", "sh", system=FOREIGN), card("r", "r", system=FOREIGN)]
    cards.append(card("native", "py", system=NATIVE))
    write(root, defs, cards, [])
    with Daemon(root) as daemon:
        c = daemon.connect()
        assert "invalid_setting" in diagnostics(c, "unknown")
        # bash runs as a program the kernel hands to the emulator;
        # C++, Python, R and Julia in a worker built for that system.
        for ok in ("py", "cc", "sh", "r"):
            assert "invalid_setting" not in diagnostics(c, ok), ok
        # This machine's own system is the default, written out.
        assert "invalid_setting" not in diagnostics(c, "native")


@pytest.mark.skipif(emulator_registered(FOREIGN), reason=f"an emulator for {FOREIGN} is registered here")
def test_an_unavailable_foreign_system_stops_the_run(tmp_path):
    """A foreign-system run stops when the local service cannot run that system."""
    root = tmp_path / "p"
    defs = {"trigger": {"template": "trigger"},
            "sh": {"language": "bash", "inputs": [{"name": "in"}],
                   "_outputs": MSG_FBS,
                   "_nix": '{ pkgs, inputs, params }:\n{ run = "uname -m"; }\n'}}
    write(root, defs, [card("start", "trigger"), card("x", "sh", system=FOREIGN)], [conn("start", "out", "x", "in")])
    with Daemon(root) as daemon:
        c = daemon.connect()
        run_id, status = run_and_wait(c, "start")
        assert status.state == RunState.failed
        record = c.call("RunRecordRequest", runId=run_id, card=path("x"))
        assert not record.record.summary.executionService
        failure = record.status.failure
        assert failure.kind == FailureKind.unsupported
        message = text(failure.diagnostic)
        assert FOREIGN in message and "binfmt_misc" in message, message
