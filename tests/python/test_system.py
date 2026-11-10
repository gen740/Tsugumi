"""A Card's "system": built and run for another Nix system under an emulator
the kernel has registered (binfmt_misc). Which systems a Card may name,
which Cards can use one, and what a run says when no emulator is there.

Emulated runs themselves are checked in the shared VM on a Mac, with Rosetta
and with qemu (TSUGUMI_VM_X86=qemu): docs/Evidence.md.
"""
from __future__ import annotations

import platform
import sys
from pathlib import Path

import pytest

from conftest import Daemon
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
    root = tmp_path / "p"
    defs = {"py": {"language": "python"}, "cc": {"language": "cpp"}, "sh": {"language": "bash"},
            "r": {"language": "r"}}
    cards = [card("unknown", "sh", system="riscv64-linux"), card("py", "py", system=FOREIGN),
             card("cc", "cc", system=FOREIGN), card("sh", "sh", system=FOREIGN), card("r", "r", system=FOREIGN)]
    if NATIVE.endswith("-linux"):
        cards.append(card("native", "py", system=NATIVE))
    write(root, defs, cards, [])
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        assert "invalid_setting" in diagnostics(c, "unknown")
        # C++ and Python run inside the worker, which is built for this machine.
        assert "unsupported_system" in diagnostics(c, "py")
        assert "unsupported_system" in diagnostics(c, "cc")
        # bash, R and Julia run as programs the kernel hands to the emulator.
        for ok in ("sh", "r"):
            assert not {"unsupported_system", "invalid_setting"} & set(diagnostics(c, ok)), ok
        if NATIVE.endswith("-linux"):
            # This machine's own system is the default, written out.
            assert not {"unsupported_system", "invalid_setting"} & set(diagnostics(c, "native"))
    finally:
        d.cleanup()


@pytest.mark.skipif(emulator_registered(FOREIGN), reason=f"an emulator for {FOREIGN} is registered here")
def test_without_an_emulator_the_run_says_what_is_missing(tmp_path):
    root = tmp_path / "p"
    defs = {"trigger": {"template": "trigger"},
            "sh": {"language": "bash", "inputs": [{"name": "in"}],
                   "_outputs": MSG_FBS,
                   "_nix": '{ pkgs, inputs, params }:\n{ run = "uname -m"; }\n'}}
    write(root, defs, [card("start", "trigger"), card("x", "sh", system=FOREIGN)], [conn("start", "out", "x", "in")])
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run_id, status = run_and_wait(c, "start")
        assert status.state == RunState.failed
        failure = c.call("RunRecordRequest", runId=run_id, card=path("x")).status.failure
        assert failure.kind == FailureKind.unsupported
        message = text(failure.diagnostic)
        assert FOREIGN in message and "binfmt_misc" in message, message
    finally:
        d.cleanup()
