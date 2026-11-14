"""The native worker's loading boundary, using socketpairs without a daemon."""

import contextlib
import os
from pathlib import Path
import select
import shutil
import socket
import struct
import subprocess
import sys

import flatbuffers
import pytest

from Tsugumi.Data.CardPath import CardPathT
from Tsugumi.Data.FlatBufferData import FlatBufferDataT
from Tsugumi.Worker.EnvVar import EnvVarT
from Tsugumi.Worker.WorkerCall import WorkerCallT
from Tsugumi.Worker.WorkerMessage import WorkerMessage, WorkerMessageT
from Tsugumi.Worker.WorkerPayload import WorkerPayload

ROOT = Path(__file__).resolve().parents[2]
WORKER = Path(os.environ.get("TSUGUMI_WORKER", ROOT / "build/workerd/tsugumi-worker"))


def library(tmp_path, source, name="main"):
    if not shutil.which("cc"):
        pytest.skip("needs a C compiler")
    lib = tmp_path / "lib"
    lib.mkdir(exist_ok=True)
    c = tmp_path / (name + ".c")
    c.write_text(source)
    subprocess.run(["cc", "-shared", "-fPIC", "-I", str(ROOT / "sdk/include"),
                    str(c), "-o", str(lib / (name + ".so"))], check=True)


@contextlib.contextmanager
def worker(package):
    with contextlib.ExitStack() as stack:
        data, child_data = socket.socketpair()
        control, child_control = socket.socketpair()
        for channel in (data, child_data, control, child_control):
            stack.enter_context(channel)
        # exec after remapping avoids any dependence on the parent's FD numbers.
        launch = ('import os,sys; a,b=map(int,sys.argv[1:3]); '
                  'x=os.dup(a); y=os.dup(b); os.dup2(x,3); os.dup2(y,4); os.set_inheritable(3,True); os.set_inheritable(4,True); '
                  'os.execv(sys.argv[3],sys.argv[3:])')
        p = subprocess.Popen([sys.executable, "-c", launch, str(child_data.fileno()), str(child_control.fileno()),
                              str(WORKER), "--package", str(package), "--bash", "/bin/bash"],
                             pass_fds=(child_data.fileno(), child_control.fileno()), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        child_data.close()
        child_control.close()
        data.settimeout(3)
        try:
            yield p, data, control
        finally:
            p.kill()
            out, err = p.communicate(timeout=5)
            if p.returncode not in (-9, 0):
                print("worker exit", p.returncode, out, err)


def send_call(data, tmp_path, call_id=1, deadline=None):
    call = WorkerCallT()
    call.runId = "recorded-run"
    call.card = CardPathT()
    call.card.segments = ["card"]
    call.invocation = call_id
    call.inputs = []
    call.outputs = []
    call.params = FlatBufferDataT()
    call.params.schemaBfbs = []
    call.params.rootType = "Unused"
    call.params.bytes = [0, 0, 0, 0]
    call.workingDirectory = str(tmp_path)
    call.temporaryDirectory = str(tmp_path)
    call.outputDirectory = ""
    call.environment = []
    environment = {"TSUGUMI_RUN_ID": "recorded-run", "LOAD_MARKER": str(tmp_path / "loaded")}
    if call_id == 1:
        environment["ONLY_FIRST_CALL"] = "transient"
    for key, value in environment.items():
        var = EnvVarT()
        var.name, var.value = key, value
        call.environment.append(var)
    call.deadlineMs = deadline
    msg = WorkerMessageT()
    msg.callId, msg.valueType, msg.value = call_id, WorkerPayload.WorkerCall, call
    b = flatbuffers.Builder(256)
    b.Finish(msg.Pack(b), file_identifier=b"TSW1")
    frame = bytes(b.Output())
    data.sendall(struct.pack("<I", len(frame)) + frame)


def receive(data):
    def full(n):
        out = b""
        while len(out) < n:
            part = data.recv(n - len(out))
            assert part, "worker ended without a result"
            out += part
        return out
    size, = struct.unpack("<I", full(4))
    return WorkerMessageT.InitFromObj(WorkerMessage.GetRootAs(full(size)))


def test_card_constructors_wait_for_the_invocation(tmp_path):
    """Card constructors and tsugumi_load see the call environment and run once."""
    library(tmp_path, r'''
#include <tsugumi/sdk/abi.h>
#include <stdio.h>
#include <stdlib.h>
__attribute__((constructor)) static void constructor(void) {
  puts("card constructor"); fflush(stdout);
  const char* marker = getenv("LOAD_MARKER");
  if (marker) { FILE* f = fopen(marker, "w"); fputs(getenv("TSUGUMI_RUN_ID"), f); fclose(f); }
}
int tsugumi_load(const tsugumi_call* call) { puts("card load"); setenv("FROM_SOURCE", "source", 1); return 0; }
int tsugumi_main(const tsugumi_call* call) {
  if (!getenv("FROM_SOURCE") || getenv("FROM_MAIN")) return 1;
  if (call->invocation == 2 && getenv("ONLY_FIRST_CALL")) return 2;
  setenv("FROM_MAIN", "transient", 1);
  return 0;
}
''')
    with worker(tmp_path) as (p, data, _):
        assert not select.select([p.stdout], [], [], 0.3)[0], "Card code ran in standby"
        send_call(data, tmp_path)
        assert receive(data).valueType == WorkerPayload.WorkerResult
        assert (tmp_path / "loaded").read_text() == "recorded-run"
        assert p.stdout.readline() == b"card constructor\n"
        assert p.stdout.readline() == b"card load\n"
        send_call(data, tmp_path, 2)
        assert receive(data).valueType == WorkerPayload.WorkerResult
        assert not select.select([p.stdout], [], [], 0.1)[0]


def test_runtime_is_prepared_but_card_loading_is_recorded(tmp_path):
    """Only SDK preparation runs in standby; source loading can publish to its invocation."""
    library(tmp_path, r'''
#include <tsugumi/sdk/abi.h>
#include <stdio.h>
int tsugumi_prepare(void) { puts("runtime ready"); fflush(stdout); return 0; }
int tsugumi_load(const tsugumi_call* c) { return c->display_text(c->host, c->run_id); }
int tsugumi_main(const tsugumi_call* c) { return 0; }
''', "runtime")
    with worker(tmp_path) as (p, data, _):
        assert select.select([p.stdout], [], [], 3)[0]
        assert p.stdout.readline() == b"runtime ready\n"
        assert not select.select([data], [], [], 0.1)[0]
        send_call(data, tmp_path)
        display = receive(data)
        assert display.callId == 1 and display.valueType == WorkerPayload.WorkerDisplay
        assert display.value.display.runId == b"recorded-run"
        assert display.value.display.invocation == 1
        assert display.value.display.value.text == b"recorded-run"
        assert receive(data).valueType == WorkerPayload.WorkerResult


@pytest.mark.parametrize("stop", ["deadline", "cancel"])
def test_source_loading_obeys_the_call_stop(tmp_path, stop):
    """Loading has the call's deadline and cancellation; main never starts after a stop."""
    from Tsugumi.Data.FailureKind import FailureKind
    from Tsugumi.Worker.WorkerCancel import WorkerCancelT
    library(tmp_path, r'''
#include <tsugumi/sdk/abi.h>
#include <stdio.h>
#include <unistd.h>
int tsugumi_prepare(void) { return 0; }
int tsugumi_load(const tsugumi_call* c) {
  puts("loading"); fflush(stdout);
  while (!c->cancelled(c->host) && c->remaining_ms(c->host) != 0) usleep(1000);
  usleep(2000);
  return 0;
}
int tsugumi_main(const tsugumi_call* c) { puts("main ran"); return 0; }
''', "runtime")
    with worker(tmp_path) as (p, data, control):
        send_call(data, tmp_path, deadline=50 if stop == "deadline" else None)
        assert select.select([p.stdout], [], [], 3)[0]
        assert p.stdout.readline() == b"loading\n"
        if stop == "cancel":
            msg = WorkerMessageT()
            msg.callId, msg.valueType, msg.value = 1, WorkerPayload.WorkerCancel, WorkerCancelT()
            b = flatbuffers.Builder(32)
            b.Finish(msg.Pack(b), file_identifier=b"TSW1")
            frame = bytes(b.Output())
            control.sendall(struct.pack("<I", len(frame)) + frame)
        result = receive(data)
        assert result.valueType == WorkerPayload.WorkerFailure
        assert result.value.failure.kind == (FailureKind.timeout if stop == "deadline" else FailureKind.cancelled)
        assert not select.select([p.stdout], [], [], 0.1)[0], "main ran after loading was stopped"


def test_source_loading_failure_is_an_invocation_failure(tmp_path):
    """A failing loader returns a failure for the recorded call, with its stderr preserved."""
    library(tmp_path, r'''
#include <tsugumi/sdk/abi.h>
#include <stdio.h>
int tsugumi_load(const tsugumi_call* c) { fputs("source failed\n", stderr); return 1; }
int tsugumi_main(const tsugumi_call* c) { return 0; }
''')
    with worker(tmp_path) as (p, data, _):
        send_call(data, tmp_path)
        result = receive(data)
        assert result.callId == 1 and result.valueType == WorkerPayload.WorkerFailure
        assert p.stderr.readline() == b"source failed\n"


def test_embedded_python_import_is_inside_the_invocation(tmp_path):
    """The real Python adapter imports Card code with its run identity and reports import errors to that call."""
    import json
    import sysconfig
    include = Path(sysconfig.get_path("include"))
    libdir = Path(sysconfig.get_config_var("LIBDIR"))
    if not shutil.which("cc") or not (include / "Python.h").is_file():
        pytest.skip("needs the Python embedding headers and C compiler")
    (tmp_path / "lib").mkdir()
    source = tmp_path / "source"
    (source / "card").mkdir(parents=True)
    (source / "card/main.py").write_text('''
import os, tsugumi
print("import run=" + os.environ["TSUGUMI_RUN_ID"], flush=True)
tsugumi.Report().text(os.environ["TSUGUMI_RUN_ID"])
raise RuntimeError("import stopped")
''')
    defines = {
        "TSUGUMI_PY_HOME": sys.base_prefix,
        "TSUGUMI_PY_PATH": str(ROOT / "sdk/python") + ":" + str(source),
        "TSUGUMI_LIBPYTHON": str(libdir / sysconfig.get_config_var("LDLIBRARY")),
    }
    subprocess.run(["cc", "-shared", "-fPIC", "-I", str(ROOT / "sdk/include"), "-I", str(include),
                    *["-D" + k + "=" + json.dumps(v) for k, v in defines.items()],
                    str(ROOT / "sdk/src/python_main.c"), "-L", str(libdir),
                    "-lpython" + sysconfig.get_config_var("LDVERSION"), "-Wl,-rpath," + str(libdir),
                    "-o", str(tmp_path / "lib/runtime.so")], check=True)
    with worker(tmp_path) as (p, data, _):
        assert not select.select([p.stdout], [], [], 0.3)[0], "Python Card ran in standby"
        send_call(data, tmp_path)
        shown = receive(data)
        assert shown.callId == 1 and shown.valueType == WorkerPayload.WorkerDisplay
        assert shown.value.display.value.text == b"recorded-run"
        failed = receive(data)
        assert failed.callId == 1 and failed.valueType == WorkerPayload.WorkerFailure
        assert b"RuntimeError: import stopped" in failed.value.failure.diagnostic
        assert p.stdout.readline() == b"import run=recorded-run\n"
