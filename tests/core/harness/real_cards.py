"""Small Card fixtures shared by host and VM integration scenarios.

Python builders here require the redesigned kit pin with sdk/python/default.nix.
"""

import json
import socket
import threading
from pathlib import Path

from tsugumi_client import path, text

NUM = "namespace t; table Num { value:ulong; } table Outputs { out:Num (required); } root_type Outputs;"


TXT = "namespace t; table Txt { value:string; } table Outputs { out:Txt (required); } root_type Outputs;"


FLAG = "namespace t; table Flag { value:bool; } table Outputs { flag:Flag (required); } root_type Outputs;"

PYTHON_BINDINGS = """  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };"""

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
    """The Nix expression of a run whose main.py is `main`, built with
    the kit's pythonCard. `values` binds Python names to Nix string expressions
    (store paths, evaluated names); their string context builds them into the
    Card's closure. The core writes the input catalog only for wired Cards.
    `tools` is a Nix list of packages for subprocesses."""
    parts = [f'"{name} = " (builtins.toJSON ({expr})) "\\n"' for name, expr in (values or {}).items()]
    source = "(builtins.concatStringsSep \"\" [ " + " ".join(parts + [nix_string(main)]) + " ])"
    options = f" tools = {tools};" if tools else ""
    return ("(sdk.pythonCard { inherit params; outputs = ./_outputs.fbs;"
            " inputs = if builtins.pathExists ./_inputs.fbs then ./_inputs.fbs else null;"
            " src = pkgs.writeTextDir \"main.py\" " + source + ";" + options + " })")


def python_nix(main: str, *, values: dict | None = None, let: str = "", tools: str = "", extra: str = "") -> str:
    """default.nix of a Card whose run is python_run(main, ...)."""
    return ("{ params, inputs }:\nlet\n" + PYTHON_BINDINGS + f"\n{let}\nin\n"
            + "{ run = " + python_run(main, values=values, tools=tools) + "; " + extra + "}\n")


def python_card(main: str, *, outputs=NUM, inputs=(), values=None, let="", tools="", extra="", **settings) -> dict:
    body = {"type": "code", "inputs": [{"name": i} for i in inputs], **settings}
    body["_outputs"] = outputs
    body["_nix"] = python_nix(main, values=values, let=let, tools=tools, extra=extra)
    return body


def txt_main(body: str) -> str:
    """main.py running `body` (Python, indented by 4) and returning out = RESULT."""
    return ("from card import Output\nfrom t.Txt import TxtT\n\ndef run(input):\n"
            + body + "\n    return Output(out=TxtT(value=RESULT))\n")


# main.py returning out = Num(1).
ONE_MAIN = """from card import Output
from t.Num import NumT

def run(input):
    return Output(out=NumT(value=1))
"""


def log(c, run_id, card_id, stream=1, inv=1):
    return text(c.call("LogRangeRequest", runId=run_id, card=path(*card_id.split("/")), invocation=inv,
                       stream=stream, limitBytes=262144).text)


# main.py reporting what its process environment holds: what the module's top
# set, what an earlier run set, HOME, TMPDIR and the working directory, through
# os.environ and the C environment (what native libraries read).
ENVIRONMENT_MAIN = """import ctypes, json, os
from card import Output
from t.Txt import TxtT

AT_LOAD = os.getcwd()
os.environ["FROM_TOP"] = "top"
libc = ctypes.CDLL(None)
libc.getenv.argtypes = [ctypes.c_char_p]
libc.getenv.restype = ctypes.c_char_p
NAMES = ["FROM_TOP", "FROM_RUN", "HOME", "TMPDIR", "TSUGUMI_RUN_ID"]

def run(input):
    seen = {"load": AT_LOAD, "cwd": os.getcwd(),
            "python": {n: os.environ.get(n) for n in NAMES},
            "native": {n: (libc.getenv(n.encode()) or b"").decode() or None for n in NAMES}}
    os.environ["FROM_RUN"] = "run"
    return Output(out=TxtT(value=json.dumps(seen)))
"""


def check_process_environment(first: dict, second: dict) -> None:
    """Two runs of ENVIRONMENT_MAIN on one reused worker: the environment,
    HOME, TMPDIR and the working directory (HOME) are the process's, set once
    at launch and never reset per call."""
    import os
    home = os.path.realpath(first["python"]["HOME"])
    assert os.path.isdir(first["load"]) and os.path.realpath(first["load"]) == home, first
    assert os.path.realpath(second["cwd"]) == home, second
    for seen, previous in ((first, None), (second, "run")):
        for values in (seen["python"], seen["native"]):
            assert values["FROM_TOP"] == "top", seen
            assert values["FROM_RUN"] == previous, seen
            assert values["TSUGUMI_RUN_ID"] is None, seen
            assert values["HOME"] == first["python"]["HOME"], seen
            assert values["TMPDIR"] == first["python"]["TMPDIR"], seen


# A Card that declares jq to its builder: the worker puts only the package's
# bin directory on PATH, so jq resolves there and nothing else does.
DEFINITION_TOOLS_MAIN = """import os, shutil, subprocess
from card import Output
from t.Txt import TxtT

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
