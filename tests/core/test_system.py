"""A Card's "system": built and run for another Nix system under an emulator
the kernel has registered (binfmt_misc). Which systems a Card may name (Cards
of every language may), and what a run says when no emulator is there.

Emulated runs themselves are checked in the shared VM on a Mac, with Rosetta
and with qemu (TSUGUMI_VM_X86=qemu): docs/Development.md#tests.
"""
from __future__ import annotations

import platform
import sys
from pathlib import Path

import pytest

from harness.daemon import Daemon
from projects import MSG_FBS, card, conn, run_and_wait, write
from tsugumi_client import path, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState

MACHINE = {"arm64": "aarch64", "aarch64": "aarch64", "x86_64": "x86_64", "amd64": "x86_64"}[platform.machine().lower()]
NATIVE = f"{MACHINE}-{'darwin' if sys.platform == 'darwin' else 'linux'}"
FOREIGN = "x86_64-linux" if MACHINE == "aarch64" else "aarch64-linux"


def emulator_registered(system: str) -> bool:
    """Whether the kernel runs this system's ELF executables (as the core reads it)."""
    machine = {"x86_64-linux": "3e00", "aarch64-linux": "b700"}[system]
    root = Path("/proc/sys/fs/binfmt_misc")
    if not root.is_dir():
        return False
    for entry in root.iterdir():
        if entry.name in ("register", "status"):
            continue
        try:
            lines = entry.read_text().splitlines()
        except OSError:
            continue
        magic = next((line[6:] for line in lines if line.startswith("magic ")), "")
        if "enabled" in lines and magic.startswith("7f454c4602") and magic[36:40] == machine:
            return True
    return False


def diagnostics(c, card_id: str) -> list[str]:
    for r in c.call("ProjectInspectRequest").resolution:
        if [text(s) for s in r.card.segments] == [card_id]:
            return [text(d.code) for d in r.diagnostics]
    raise KeyError(card_id)


def test_which_systems_and_cards(tmp_path):
    """Supported target systems are accepted for each Card language."""
    root = tmp_path / "p"
    defs = {"py": {"language": "python"}, "cc": {"language": "cpp"}, "sh": {"language": "bash"},
            "r": {"language": "r"}}
    cards = [card("unknown", "sh", system="riscv64-linux"), card("py", "py", system=FOREIGN),
             card("cc", "cc", system=FOREIGN), card("sh", "sh", system=FOREIGN), card("r", "r", system=FOREIGN)]
    if NATIVE.endswith("-linux"):
        cards.append(card("native", "py", system=NATIVE))
    write(root, defs, cards, [])
    with Daemon(root) as daemon:
        c = daemon.connect()
        assert "invalid_setting" in diagnostics(c, "unknown")
        # bash runs as a program the kernel hands to the emulator;
        # C++, Python, R and Julia in a worker built for that system.
        for ok in ("py", "cc", "sh", "r"):
            assert "invalid_setting" not in diagnostics(c, ok), ok
        if NATIVE.endswith("-linux"):
            # This machine's own system is the default, written out.
            assert "invalid_setting" not in diagnostics(c, "native")


@pytest.mark.skipif(emulator_registered(FOREIGN), reason=f"an emulator for {FOREIGN} is registered here")
def test_without_an_emulator_the_run_says_what_is_missing(tmp_path):
    """A foreign-system run reports when its required emulator is unavailable."""
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
        failure = c.call("RunRecordRequest", runId=run_id, card=path("x")).status.failure
        assert failure.kind == FailureKind.unsupported
        message = text(failure.diagnostic)
        assert FOREIGN in message and "binfmt_misc" in message, message
