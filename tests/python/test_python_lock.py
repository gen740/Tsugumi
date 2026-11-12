"""PyPI packages of a Python Card (pythonCard's `lock` and `hash`): the Card
names its own uv.lock and the hash of the wheels, and gets exactly those,
installed without the network. Nothing is resolved when a Card is built: a
lock without a hash, a wrong hash and a lock that no longer matches
pyproject.toml are failures that say what to do. With
TSUGUMI_TEST_CONTAINER=1 the Cards run in the shared VM's sandbox."""

from __future__ import annotations

import json
import os

import pytest

from conftest import Daemon, real_environment
from projects import put, run_and_wait
from tsugumi_client import path, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RunState import RunState

IN_VM = os.environ.get("TSUGUMI_TEST_CONTAINER") == "1"
OUTPUTS = "namespace t;\ntable Outputs { }\nroot_type Outputs;\n"

# tabulate is not among the packages of a Card without `packages`, and has one
# wheel for every system, so its hash is the same wherever the test runs.
PYPROJECT = """[project]
name = "card"
version = "0"
requires-python = ">=3.11"
dependencies = ["tabulate==0.9.0"]
"""
LOCK = """version = 1
revision = 5
requires-python = ">=3.11"

[[package]]
name = "card"
version = "0"
source = { virtual = "." }
dependencies = [
    { name = "tabulate" },
]

[package.metadata]
requires-dist = [{ name = "tabulate", specifier = "==0.9.0" }]

[[package]]
name = "tabulate"
version = "0.9.0"
source = { registry = "https://pypi.org/simple" }
sdist = { url = "https://files.pythonhosted.org/packages/ec/fe/802052aecb21e3797b8f7902564ab6ea0d60ff8ca23952079064155d1ae1/tabulate-0.9.0.tar.gz", hash = "sha256:0095b12bf5966de529c0feb1fa08671671b3368eec77d7ef7ab114be2c068b3c", size = 81090, upload-time = "2022-10-06T17:21:48.54Z" }
wheels = [
    { url = "https://files.pythonhosted.org/packages/40/44/4a5f08c96eb108af5cb50b41f76142f0afa346dfa99d5296fe7202a11854/tabulate-0.9.0-py3-none-any.whl", hash = "sha256:024ca478df22e9340661486f85298cff5f6dcdba14f3813e8830015b9ed1948f", size = 35252, upload-time = "2022-10-06T17:21:44.262Z" },
]
"""
HASH = "sha256-i/PVjQGOqKYOYPOJ+APBtL2v29Ab6hqV+4Xg3S2VzfI="
WRONG = "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="

# The locked package is importable, comes from the locked set and not from the
# network: the Card has no network permission, and says so.
USES_TABULATE = """import socket

import tabulate


def main(ctx):
    assert tabulate.__version__ == "0.9.0"
    assert "tsugumi-python-locked" in tabulate.__file__, tabulate.__file__
    try:
        socket.create_connection(("1.1.1.1", 443), timeout=3).close()
        reached = "network"
    except OSError:
        reached = "offline"
    ctx.display_text(reached + "\\n" + tabulate.tabulate([[1, 2]], headers=["a", "b"], tablefmt="plain"))
"""
WITHOUT_LOCK = """def main(ctx):
    try:
        import tabulate
        ctx.display_text("found " + tabulate.__file__)
    except ImportError:
        ctx.display_text("no tabulate")
"""


def nix(arguments: str) -> str:
    return ("{ pkgs, inputs, params }:\nlet sdk = inputs.tsugumi.sdk { languages = [ \"python\" ]; }; in\n"
            "{ run = inputs.tsugumi.pythonCard { inherit sdk; src = ./src; " + arguments + " }; }\n")


def write_project(root, arguments: str, main: str, pyproject: str | None = PYPROJECT, sandbox: bool = True):
    project = {"name": "lock", "pkgsPath": "tsugumi:default", "databaseConnections": [], "canvas": {
        "cards": [{"id": "start"}, {"id": "card"}],
        "connections": [{"from": {"card": "start", "port": "out"}, "to": {"card": "card", "port": "start"}}]}}
    put(root / "tsugumi.json", json.dumps(project))
    put(root / "layout.json", "{}")
    put(root / "definitions/start/_card.json", json.dumps({"template": "trigger"}))
    d = root / "definitions/card"
    put(d / "_card.json", json.dumps({"language": "python", "inputs": [{"name": "start"}],
                                      **({} if sandbox or IN_VM else {"sandbox": False})}))
    put(d / "default.nix", nix(arguments))
    put(d / "_outputs.fbs", OUTPUTS)
    put(d / "src/main.py", main)
    put(d / "uv.lock", LOCK)
    if pyproject is not None:
        put(d / "pyproject.toml", pyproject)


@pytest.fixture
def project(tmp_path):
    """The project's folder, and a function that opens it after the test wrote it."""
    root = tmp_path / "lock"
    opened = []

    def connect():
        env = real_environment(root, "tsugumi-test-python-lock") if IN_VM else Daemon(root).start()
        opened.append(env)
        c = env.client()
        if not IN_VM:
            c.hello()
        return c

    yield root, connect
    for env in opened:
        env.cleanup()


def run(connect, timeout: float = 1800):
    c = connect()
    run_id, status = run_and_wait(c, "start", timeout=timeout)
    log = text(c.call("LogRangeRequest", runId=run_id, card=path("card"), stream=LogStream.build, limitBytes=262144).text)
    return c, run_id, status, log


def said(status, log: str) -> str:
    return (text(status.failure.diagnostic) if status.failure else "") + "\n" + log


def test_a_locked_package_is_installed_from_the_lock_and_runs_without_the_network(project):
    root, connect = project
    write_project(root, f'lock = ./uv.lock; hash = "{HASH}";', USES_TABULATE)
    c, run_id, status, log = run(connect)
    assert status.state == RunState.succeeded, said(status, log)
    shown = text(c.call("RunRecordRequest", runId=run_id, card=path("card")).record.display.value.text)
    assert shown.splitlines() == ["offline", "  a    b", "  1    2"], shown


def test_a_hash_per_system_is_accepted(project):
    root, connect = project
    systems = ["aarch64-linux", "x86_64-linux", "aarch64-darwin", "x86_64-darwin"]
    per_system = "{ " + " ".join(f'{s} = "{HASH}";' for s in systems) + " }"
    write_project(root, f"lock = ./uv.lock; hash = {per_system};", USES_TABULATE)
    _, _, status, log = run(connect)
    assert status.state == RunState.succeeded, said(status, log)


def test_a_card_without_a_lock_has_no_pypi_package(project):
    """The lock file alone changes nothing: only `lock` in default.nix uses it."""
    root, connect = project
    write_project(root, "", WITHOUT_LOCK, sandbox=False)
    c, run_id, status, log = run(connect)
    assert status.state == RunState.succeeded, said(status, log)
    assert text(c.call("RunRecordRequest", runId=run_id, card=path("card")).record.display.value.text) == "no tabulate"


@pytest.mark.parametrize("arguments, pyproject, why", [
    ("lock = ./uv.lock;", PYPROJECT, "`lock` needs `hash`"),
    (f'hash = "{HASH}";', PYPROJECT, "`hash` is given without `lock`"),
    (f'lock = ./uv.lock; hash = "{HASH}";', None, "no pyproject.toml beside the lock"),
    (f'lock = ./uv.lock; hash = {{ riscv64-linux = "{HASH}"; }};', PYPROJECT, "`hash` has no entry for"),
])
def test_an_incomplete_pin_is_refused_and_says_what_is_missing(project, arguments, pyproject, why):
    root, connect = project
    write_project(root, arguments, USES_TABULATE, pyproject=pyproject, sandbox=False)
    _, _, status, log = run(connect, timeout=600)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.nix_failed, said(status, log)
    assert why in said(status, log)


def test_a_wrong_hash_fails_and_reports_the_hash_of_what_was_fetched(project):
    root, connect = project
    write_project(root, f'lock = ./uv.lock; hash = "{WRONG}";', USES_TABULATE, sandbox=False)
    _, _, status, log = run(connect)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.nix_failed, said(status, log)
    assert "hash mismatch" in said(status, log) and HASH in said(status, log)


def test_a_lock_that_no_longer_matches_pyproject_fails_and_says_to_lock_again(project):
    """The hash of the old wheels does not let an edited pyproject.toml through."""
    root, connect = project
    write_project(root, f'lock = ./uv.lock; hash = "{HASH}";', USES_TABULATE,
                  pyproject=PYPROJECT.replace("0.9.0", "0.8.10"), sandbox=False)
    _, _, status, log = run(connect)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.nix_failed, said(status, log)
    assert "uv.lock does not match pyproject.toml" in said(status, log)
