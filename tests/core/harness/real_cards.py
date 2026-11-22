"""Real SDK Card fixtures shared by host and VM integration scenarios."""

import json
import socket
import threading
from pathlib import Path

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
    # Closing a listening descriptor in another thread need not wake accept.
    # The caller owns this socket and has no separate shutdown channel.
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


# main.py that prints PRINTED (bound with values=) and returns no outputs.
PRINT_MAIN = """from card import Output

def run(input):
    print(PRINTED, flush=True)
    return Output()
"""


def python_run(main: str, *, values: dict | None = None, tools: str = "") -> str:
    """The Nix expression of an ABI 9 run whose main.py is `main`, built with
    the kit's pythonCard. `values` binds Python names to Nix string expressions
    (store paths, evaluated names); their string context builds them into the
    Card's closure. `tools` is a Nix list of packages for subprocesses."""
    parts = [f'"{name} = " (builtins.toJSON ({expr})) "\\n"' for name, expr in (values or {}).items()]
    source = "(builtins.concatStringsSep \"\" [ " + " ".join(parts + [nix_string(main)]) + " ])"
    options = f" tools = {tools};" if tools else ""
    return "(inputs.tsugumi.pythonSdk.pythonCard { inherit schemas; src = pkgs.writeTextDir \"main.py\" " + source + ";" + options + " })"


def python_nix(main: str, *, values: dict | None = None, let: str = "", tools: str = "", extra: str = "") -> str:
    """default.nix of a Card whose run is python_run(main, ...)."""
    return ("{ pkgs, params, inputs, schemas }:\n" + (f"let\n{let}\nin\n" if let else "")
            + "{ run = " + python_run(main, values=values, tools=tools) + "; " + extra + "}\n")


def printing_run(text_nix: str) -> str:
    """A run that prints the value of the Nix string expression `text_nix`."""
    return python_run(PRINT_MAIN, values={"PRINTED": text_nix})


def python_card(main: str, *, outputs=NUM, inputs=(), values=None, let="", tools="", extra="", **settings) -> dict:
    body = {"type": "code", "inputs": [{"name": i} for i in inputs], **settings}
    body["_outputs"] = outputs
    body["_nix"] = python_nix(main, values=values, let=let, tools=tools, extra=extra)
    return body


def txt_main(body: str) -> str:
    """main.py running `body` (Python, indented by 4) and returning out = RESULT."""
    return ("from card import Output\nfrom outputs.out.t.Txt import TxtT\n\ndef run(input):\n"
            + body + "\n    return Output(out=TxtT(value=RESULT))\n")


def bash(script: str, inputs=(), outputs=NUM, pre=None, post=None, extra="", **settings) -> dict:
    attrs = [f"run = {nix_string(script)};"]
    if pre:
        attrs.append(f"preRun = {nix_string(pre)};")
    if post:
        attrs.append(f"postRun = {nix_string(post)};")
    body = {"type": "code", "inputs": [{"name": i} for i in inputs], **settings}
    body["_outputs"] = outputs
    body["_nix"] = "{ pkgs, inputs, params, ... }:\n{\n  " + "\n  ".join(attrs) + "\n" + extra + "}\n"
    return body


def log(c, run_id, card_id, stream=1, inv=1):
    return text(c.call("LogRangeRequest", runId=run_id, card=path(*card_id.split("/")), invocation=inv,
                       stream=stream, limitBytes=262144).text)


PYTHON_NIX = ('{ pkgs, inputs, params, ... }:\nlet sdk = inputs.tsugumi.sdk { languages = [ "python" ]; }; in\n'
              '{ run = inputs.tsugumi.pythonCard { inherit sdk; src = ./src; };%s }\n')


PYTHON_OUTPUTS = "namespace t; table Num { value:ulong; } table Outputs { out:Num; } root_type Outputs;"


CWD_AT_LOAD_MAIN = '''
import os
AT_LOAD = os.getcwd()

def main(inputs, report):
    report.text(AT_LOAD)
'''


ENV_MAIN = '''
import ctypes, json, os
assert os.path.isdir(os.getcwd())
os.environ["FROM_TOP"] = "top"
os.environ["FROM_TOP_AND_PRE"] = "top"
libc = ctypes.CDLL(None)
libc.getenv.argtypes = [ctypes.c_char_p]
libc.getenv.restype = ctypes.c_char_p

def main(inputs, report):
    names = ["FROM_PRE", "FROM_TOP", "FROM_TOP_AND_PRE", "FROM_MAIN", "TSUGUMI_RUN_ID"]
    values = {n: os.environ.get(n) for n in names}
    native = {n: (libc.getenv(n.encode()) or b"").decode() or None for n in names}
    report.text(json.dumps({"cwd": os.getcwd(), "python": values, "native": native}))
    os.environ["FROM_MAIN"] = "main"
'''


ENV_PRE_RUN = "mkdir -p there && cd there && export FROM_PRE=pre FROM_TOP_AND_PRE=pre"


def python_probe_project(main: str, pre: str | None = None) -> tuple:
    """(defs, cards, conns, files) of a trigger and a Python Card `show`."""
    attrs = f" preRun = {nix_string(pre)};" if pre else ""
    defs = {"trigger": {"type": "trigger"},
            "show": {"type": "code", "reuseWorker": True, "_outputs": PYTHON_OUTPUTS, "_nix": PYTHON_NIX % attrs}}
    return defs, [card("start", "trigger"), card("show", "show")], [], {"definitions/show/src/main.py": main}


def shown_by_probe(c) -> tuple[str, str]:
    """A run from the trigger: its id and what `show` displayed."""
    run_id, st = run_and_wait(c, "start")
    assert st.state == RunState.succeeded, log(c, run_id, "show", 2)
    return run_id, text(c.call("RunRecordRequest", runId=run_id, card=path("show")).record.display.value.text)


def check_python_card_environment(c) -> None:
    """preRun's exports and `cd`, and what the module's top set, reach main
    through both os.environ and the C environment, on every run of the worker;
    changes main makes to its environment survive reuse."""
    _, first = shown_by_probe(c)
    first = json.loads(first)
    second_id, second = shown_by_probe(c)
    second = json.loads(second)
    reused, = c.call("RunInvocationsRequest", runId=second_id, card=path("show")).records
    assert reused.workerReused
    assert first["cwd"].endswith("/there"), first
    for seen, previous in ((first, None), (second, "main")):
        for values in (seen["python"], seen["native"]):
            assert values["FROM_PRE"] == "pre", seen
            assert values["FROM_TOP"] == "top", seen
            assert values["FROM_TOP_AND_PRE"] == "pre", seen  # preRun runs later, so it wins
            assert values["FROM_MAIN"] == previous, seen
            assert values["TSUGUMI_RUN_ID"] is None, seen


# A Card that declares jq to its builder: the worker puts only the package's
# bin directory on PATH, so jq resolves there and nothing else does.
DEFINITION_TOOLS_MAIN = """import os, shutil, subprocess
from card import Output
from outputs.out.t.Txt import TxtT

def run(input):
    jq = shutil.which("jq")
    assert jq and shutil.which("bash") is None and shutil.which("sh") is None, os.environ.get("PATH")
    subprocess.run([jq, "--version"], check=True)
    return Output(out=TxtT(value=os.environ["PATH"] + ";" + os.path.realpath(jq)))
"""
DEFINITION_TOOLS_NIX = python_nix(DEFINITION_TOOLS_MAIN, tools="[ pkgs.jq ]")


def check_definition_tools(client, run_id, name="work"):
    """The Card's PATH is its package's bin, and the declared tool is in its recorded closure."""
    from harness.observations import decode_scalar, read_output

    environment = next(e for e in client.call("RunEnvironmentsRequest", runId=run_id).environments
                       if text(e.id) == text(client.call("RunRecordRequest", runId=run_id,
                                                        card=path(name)).record.summary.environmentId))
    paths, offset = [], 0
    while True:
        page = client.call("RunEnvironmentPathsRequest", runId=run_id,
                           environmentId=text(environment.id), offset=offset, limit=3)
        paths.extend(text(value) for value in page.paths)
        if page.nextOffset is None:
            break
        offset = page.nextOffset
    search, tool = decode_scalar(read_output(client, run_id, card=name)).split(";")
    assert ":" not in search and search.endswith("/bin"), search
    assert str(Path(search).parent) in paths
    assert "/".join(tool.split("/")[:4]) in paths, tool
    return text(environment.id), set(paths), search, tool
