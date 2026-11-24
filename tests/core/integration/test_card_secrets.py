"""Card secrets: a Card declares names, hostd holds the values, and the Card's
process receives them as environment variables at launch.

The value must never reach what Tsugumi writes: the project (documents, run
history and its WAL), derivations, run scratch, control files and the daemon's
own log. A fake hostd broker supplies the value over the authenticated
execution broker connection, as hostd does.
"""
from __future__ import annotations

import socketserver
import tempfile
import threading
from pathlib import Path

import pytest

from harness import remote_worker as R
from harness.daemon import StartupFailure
from harness.observations import decode_scalar, read_output
from harness.real_cards import log
from test_worker_loading import native_card
from projects import card, run_and_wait, write_nix_project as write
from tsugumi_client import path, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState

SENTINEL = "card-secret-sentinel-3f9c71d2a8"
PREFIX = SENTINEL.rsplit("-", 1)[0]


@pytest.fixture
def broker(monkeypatch):
    """hostd's execution broker endpoint: a catalog and the Card secret values."""
    asked: list[list[str]] = []
    values = {"database_password": SENTINEL}

    class Broker(socketserver.StreamRequestHandler):
        def handle(self):
            message = R.read(self.rfile)
            assert R.string(message.payload.capability) == "fixture-capability"
            request = message.payload.request
            if request.bodyType == R.RequestBody.CatalogRequest:
                reply = R.reply(R.ReplyBody.Catalog, R.CatalogT(services=[]))
            elif request.bodyType == R.RequestBody.CardSecretsRequest:
                names = [R.string(name) for name in request.body.names]
                asked.append(names)
                reply = R.reply(R.ReplyBody.CardSecrets, R.CardSecretsT(secrets=[
                    R.CardSecretT(name=name, value=values[name]) for name in names if name in values]))
            else:
                reply = R.failure(R.ErrorCode.Invalid, "unexpected broker request")
            self.wfile.write(R.frame(reply))

    with tempfile.TemporaryDirectory(prefix="tsugumi-broker-") as directory:
        endpoint = str(Path(directory) / "socket")
        capability = Path(directory) / "capability"
        capability.write_text("fixture-capability")
        monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER", "unix:" + endpoint)
        monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE", str(capability))
        with socketserver.UnixStreamServer(endpoint, Broker) as server:
            thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.02})
            thread.start()
            try:
                yield asked, values
            finally:
                server.shutdown()
                thread.join()


# The definition checks the value without containing it, and never prints it.
USES_SECRET = native_card(f"""
    const auto* secret = std::getenv("database_password");
    if (!secret || !std::string_view(secret).starts_with("{PREFIX}-")) return 3;
    if (std::strlen(secret) != {len(SENTINEL)}) return 5;
    out.number(std::strlen(secret));
""", secrets=["database_password"], recordIO="on")


def files_containing(root: Path, needle: bytes) -> list[str]:
    return [str(f) for f in root.rglob("*") if f.is_file() and not f.is_symlink() and needle in f.read_bytes()]


def test_secret_reaches_the_process_and_no_record(core_factory, broker, tmp_path):
    """The Card reads its secret from the environment; only its name is recorded."""
    root = tmp_path / "p"
    write(root, {"use": USES_SECRET}, [card("use", "use")], [])
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    run_id, status = run_and_wait(c, "use")
    assert status.state == RunState.succeeded, log(c, run_id, "use", 2)
    assert broker[0] == [["database_password"]]
    record = c.call("RunRecordRequest", runId=run_id, card=path("use"))
    assert [text(name) for name in record.record.summary.secrets] == ["database_password"]
    invocation, = c.call("RunInvocationsRequest", runId=run_id, card=path("use")).records
    assert [text(name) for name in invocation.secrets] == ["database_password"]

    needle = SENTINEL.encode()
    derivations = [Path(text(e.derivation)) for e in c.call("RunEnvironmentsRequest", runId=run_id).environments]
    assert derivations
    daemon.stop()
    leaks = files_containing(root, needle) + files_containing(daemon.directory, needle)
    leaks += [str(d) for d in derivations if needle in d.read_bytes()]
    assert not leaks


def test_missing_secret_fails_before_the_card_runs(core_factory, broker, tmp_path):
    """A declared secret that hostd does not hold fails the invocation, naming it."""
    root = tmp_path / "p"
    definition = native_card("out.number(1);", secrets=["database_password", "not_set"])
    write(root, {"use": definition}, [card("use", "use")], [])
    c = core_factory(project_root=root).connect()
    run_id, status = run_and_wait(c, "use")
    assert status.state == RunState.failed
    invocation, = c.call("RunInvocationsRequest", runId=run_id, card=path("use")).records
    assert invocation.failure.kind == FailureKind.not_found
    assert "not_set" in text(invocation.failure.diagnostic)
    assert "database_password" not in text(invocation.failure.diagnostic)
    assert not invocation.environmentId, "the Card's environment was never entered"


@pytest.mark.parametrize("settings,message", [
    ({"secrets": ["Database"]}, "secrets name"),
    ({"secrets": ["token", "token"]}, "duplicate secret"),
])
def test_invalid_secret_declarations_are_reported(core_factory, broker, tmp_path, settings, message):
    """Secret names follow the same rule as the worker launch channel."""
    root = tmp_path / "p"
    write(root, {"use": native_card("out.number(1);", **settings)},
          [card("use", "use")], [])
    with pytest.raises(StartupFailure) as failure:
        core_factory(project_root=root)
    assert failure.value.kind == "invalid_document"
    assert message in failure.value.record["message"]



def test_secret_rotation_retires_resident(core_factory, broker, tmp_path):
    root = tmp_path / "p"
    definition = native_card(r"""
        const auto* secret = std::getenv("database_password");
        if (!secret) return 1;
        static unsigned count = 0;
        out.number(++count * 1000 + std::strlen(secret));
    """, secrets=["database_password"], reuseWorker=True, recordIO="on")
    write(root, {"use": definition}, [card("use", "use")], [])
    client = core_factory(project_root=root).connect()
    for count, rotates in ((1, False), (2, False), (1, True)):
        if rotates:
            broker[1]["database_password"] = SENTINEL + "-rotated"
        run_id, status = run_and_wait(client, "use")
        assert status.state == RunState.succeeded, log(client, run_id, "use", 2)
        invocation, = client.call("RunInvocationsRequest", runId=run_id, card=path("use")).records
        assert invocation.workerReused == (count == 2)
        observed = decode_scalar(read_output(client, run_id, card="use"), "uint64")
        assert observed == count * 1000 + len(broker[1]["database_password"])
