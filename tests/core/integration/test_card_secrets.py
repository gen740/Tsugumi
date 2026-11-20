"""Card secrets: a Card declares names, hostd holds the values, and the Card's
process receives them as environment variables for one invocation only.

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
from harness.real_cards import NUM, bash, log
from projects import card, run_and_wait, write
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
                yield asked
            finally:
                server.shutdown()
                thread.join()


# The definition checks the value without containing it, and never prints it.
USES_SECRET = bash(
    'case "$database_password" in ' + PREFIX + '-*) ;; *) exit 3 ;; esac; '
    f'test "${{#database_password}}" = {len(SENTINEL)} || exit 5; '
    'test "$FROM_PRE" = ready || exit 4; '
    'printf \'{"out":{"value":%d}}\' "${#database_password}" > "$TSUGUMI_OUTPUT"',
    pre="export FROM_PRE=ready", secrets=["database_password"], recordIO=True)


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
    assert broker == [["database_password"]]
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
    definition = bash('printf \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"',
                      secrets=["database_password", "not_set"])
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
    ({"secrets": ["Database"]}, "secrets entries"),
    ({"secrets": ["token", "token"]}, "duplicate secret"),
    ({"secrets": ["token"], "reuseWorker": True}, "reuseWorker"),
])
def test_invalid_secret_declarations_are_reported(core_factory, broker, tmp_path, settings, message):
    """Names follow one rule, and a Card with secrets keeps no resident worker."""
    root = tmp_path / "p"
    write(root, {"use": {"language": "bash", "_outputs": NUM,
                         "_nix": '{ pkgs, inputs, params }: { run = "true"; }', **settings}},
          [card("use", "use")], [])
    validation = core_factory(project_root=root).connect().call("ProjectValidateRequest")
    assert not validation.valid
    assert any(message in text(d.message) for d in validation.diagnostics), \
        [text(d.message) for d in validation.diagnostics]
