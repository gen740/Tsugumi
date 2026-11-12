"""Real SDK Card fixtures shared by host and VM integration scenarios."""

import json
import socket
import threading

from projects import card, run_and_wait
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState

NUM = "namespace t; table Num { value:ulong; } table Outputs { out:Num (required); } root_type Outputs;"


TXT = "namespace t; table Txt { value:string; } table Outputs { out:Txt (required); } root_type Outputs;"


FLAG = "namespace t; table Flag { value:bool; } table Outputs { flag:Flag (required); } root_type Outputs;"


def accept_in_background(listener: socket.socket) -> list:
    """Record accepted peers without leaking sockets or a blocking accept thread."""
    accepted = []
    listener.listen(1)
    listener.settimeout(0.1)

    def accept():
        while listener.fileno() != -1:
            try:
                peer, address = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            peer.close()
            accepted.append(address)

    threading.Thread(target=accept, daemon=True).start()
    return accepted


def nix_string(s: str) -> str:
    """A Nix string literal; `${` stays literal shell text (not antiquotation)."""
    return json.dumps(s).replace("${", "\\${")


def bash(script: str, inputs=(), outputs=NUM, pre=None, post=None, extra="", **settings) -> dict:
    attrs = [f"run = {nix_string(script)};"]
    if pre:
        attrs.append(f"preRun = {nix_string(pre)};")
    if post:
        attrs.append(f"postRun = {nix_string(post)};")
    body = {"language": "bash", "inputs": [{"name": i} for i in inputs], **settings}
    body["_outputs"] = outputs
    body["_nix"] = "{ pkgs, inputs, params }:\n{\n  " + "\n  ".join(attrs) + "\n" + extra + "}\n"
    return body


def log(c, run_id, card_id, stream=1, inv=1):
    return text(c.call("LogRangeRequest", runId=run_id, card=path(*card_id.split("/")), invocation=inv,
                       stream=stream, limitBytes=262144).text)


PYTHON_NIX = ('{ pkgs, inputs, params }:\nlet sdk = inputs.tsugumi.sdk { languages = [ "python" ]; }; in\n'
              '{ run = inputs.tsugumi.pythonCard { inherit sdk; src = ./src; };%s }\n')


PYTHON_OUTPUTS = "namespace t; table Num { value:ulong; } table Outputs { out:Num; } root_type Outputs;"


CWD_AT_LOAD_MAIN = '''
import os
AT_LOAD = os.getcwd()

def main(ctx):
    ctx.display_text(AT_LOAD)
'''


ENV_MAIN = '''
import ctypes, json, os
os.environ["FROM_TOP"] = "top"
os.environ["FROM_TOP_AND_PRE"] = "top"
libc = ctypes.CDLL(None)
libc.getenv.argtypes = [ctypes.c_char_p]
libc.getenv.restype = ctypes.c_char_p

def main(ctx):
    names = ["FROM_PRE", "FROM_TOP", "FROM_TOP_AND_PRE", "FROM_MAIN", "TSUGUMI_RUN_ID", "XDG_CACHE_HOME", "MPLBACKEND"]
    values = {n: os.environ.get(n) for n in names}
    native = {n: (libc.getenv(n.encode()) or b"").decode() or None for n in names}
    ctx.display_text(json.dumps({"pid": os.getpid(), "cwd": os.getcwd(), "python": values, "native": native}))
    os.environ["FROM_MAIN"] = "main"
'''


ENV_PRE_RUN = "mkdir -p there && cd there && export FROM_PRE=pre FROM_TOP_AND_PRE=pre"


def python_probe_project(main: str, pre: str | None = None) -> tuple:
    """(defs, cards, conns, files) of a trigger and a Python Card `show`."""
    attrs = f" preRun = {nix_string(pre)};" if pre else ""
    defs = {"trigger": {"template": "trigger"},
            "show": {"language": "python", "reuseWorker": True, "_outputs": PYTHON_OUTPUTS, "_nix": PYTHON_NIX % attrs}}
    return defs, [card("start", "trigger"), card("show", "show")], [], {"definitions/show/src/main.py": main}


def shown_by_probe(c, timeout=1800) -> tuple[str, str]:
    """A run from the trigger: its id and what `show` displayed."""
    run_id, st = run_and_wait(c, "start", timeout=timeout)
    assert st.state == RunState.succeeded, log(c, run_id, "show", 2)
    return run_id, text(c.call("RunRecordRequest", runId=run_id, card=path("show")).record.display.value.text)


def check_python_card_environment(c) -> None:
    """preRun's exports and `cd`, and what the module's top set, reach main
    through both os.environ and the C environment, on every run of the worker;
    what main itself set lasts for that call."""
    first_id, first = shown_by_probe(c)
    first = json.loads(first)
    second_id, second = shown_by_probe(c)
    second = json.loads(second)
    assert first["pid"] == second["pid"], "the second run needs the same worker"
    assert first["cwd"].endswith("/work/there"), first
    for seen, run_id in ((first, first_id), (second, second_id)):
        for values in (seen["python"], seen["native"]):
            assert values["FROM_PRE"] == "pre", seen
            assert values["FROM_TOP"] == "top", seen
            assert values["FROM_TOP_AND_PRE"] == "pre", seen  # preRun runs later, so it wins
            assert values["FROM_MAIN"] is None, seen
            assert values["TSUGUMI_RUN_ID"] == run_id, seen
            # What the worker was started with and the SDK's own setting stay.
            assert values["XDG_CACHE_HOME"].endswith("/home/.cache"), seen
            assert values["MPLBACKEND"] == "module://tsugumi.mpl", seen
