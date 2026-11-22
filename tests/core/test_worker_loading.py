"""The native worker's loading boundary, using socketpairs without a daemon."""

import contextlib
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys

import flatbuffers
import pytest

from Tsugumi.Data.CardPath import CardPathT
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.FlatBufferData import FlatBufferDataT
from Tsugumi.Worker.EnvVar import EnvVarT
from Tsugumi.Worker.WorkerCall import WorkerCallT
from Tsugumi.Worker.WorkerInput import WorkerInputT
from Tsugumi.Worker.WorkerMessage import WorkerMessage, WorkerMessageT
from Tsugumi.Worker.WorkerPayload import WorkerPayload
from Tsugumi.Worker.WorkerStart import WorkerStartT
from harness.values import encode_message

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
def worker(package, *, stderr=None, startup=None):
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
                             pass_fds=(child_data.fileno(), child_control.fileno()), cwd=package,
                             env={**os.environ, "HOME": str(package), "TMPDIR": str(package / "tmp"),
                                  "TSUGUMI_WORK_DIR": str(package), "TSUGUMI_TMP_DIR": str(package / "tmp"),
                                  "TSUGUMI_OUTPUT_DIR": str(package / "out")},
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE if stderr is None else stderr)
        child_data.close()
        child_control.close()
        if startup:
            message = WorkerMessageT()
            message.valueType, message.value = WorkerPayload.WorkerStart, WorkerStartT()
            message.value.environment = [EnvVarT(name=k, value=v) for k, v in startup.items()]
            send_message(data, message)
        try:
            yield p, data, control
        finally:
            p.kill()
            out, err = p.communicate(timeout=5)
            if p.returncode not in (-9, 0):
                print("worker exit", p.returncode, out, err)


def send_call(data, tmp_path, call_id=1, deadline=None, inputs=()):
    call = WorkerCallT()
    call.runId = "recorded-run"
    call.card = CardPathT()
    call.card.segments = ["card"]
    call.invocation = call_id
    call.inputs = list(inputs)
    call.outputs = []
    call.params = FlatBufferDataT()
    call.params.schemaBfbs = []
    call.params.rootType = "Unused"
    call.params.bytes = [0, 0, 0, 0]
    call.workingDirectory = str(tmp_path)
    call.temporaryDirectory = str(tmp_path)
    call.outputDirectory = ""
    call.environment = []
    call.environment = [EnvVarT(name="TSUGUMI_PARAMS_JSON", value=str(call_id))]
    call.deadlineMs = deadline
    msg = WorkerMessageT()
    msg.callId, msg.valueType, msg.value = call_id, WorkerPayload.WorkerCall, call
    send_message(data, msg)


def send_message(data, msg):
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


def test_worker_rejects_incomplete_input_before_loading_card(tmp_path):
    schema = "table Reading { label:string (required); } root_type Reading;"
    payload = encode_message(tmp_path, schema, {"label": "sample"}, root_type="Reading")
    subprocess.run(["flatc", "--binary", "--schema", "-o", str(tmp_path),
                    str(tmp_path / "message.fbs")], check=True)
    value = FlatBufferDataT()
    value.schemaBfbs = (tmp_path / "message.bfbs").read_bytes()
    value.rootType = "Reading"
    value.bytes = payload[:4]
    port = WorkerInputT()
    port.name, port.value = "reading", value
    library(tmp_path, r'''
#include <tsugumi/sdk/abi.h>
int tsugumi_main(const tsugumi_call* c) {
  return c->phase == TSUGUMI_LOAD ? tsugumi_display_text(c, "loaded", 0) : 0;
}
''')
    with worker(tmp_path) as (_, data, _):
        send_call(data, tmp_path, inputs=[port])
        failure = receive(data)
        assert failure.valueType == WorkerPayload.WorkerFailure
        assert failure.value.failure.kind == FailureKind.type_mismatch
        value.bytes = payload
        send_call(data, tmp_path, 2, inputs=[port])
        assert receive(data).valueType == WorkerPayload.WorkerDisplay
        assert receive(data).valueType == WorkerPayload.WorkerResult


def test_card_constructors_wait_for_the_invocation(tmp_path):
    """Constructors and both calls see the startup secrets, cwd and stable directories."""
    library(tmp_path, r'''
#include <tsugumi/sdk/abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
__attribute__((constructor)) static void constructor(void) {
  puts("card constructor"); fflush(stdout);
  const char* marker = getenv("LOAD_MARKER");
  if (marker && getenv("startup_key")) { FILE* f = fopen(marker, "w"); fputs("loaded", f); fclose(f); }
}
int tsugumi_main(const tsugumi_call* call) {
  if (call->phase == TSUGUMI_LOAD) {
    puts("card load"); setenv("FROM_SOURCE", "source", 1); return 0;
  }
  if (call->phase != TSUGUMI_RUN) return 0;
  char cwd[4096];
  if (!getcwd(cwd, sizeof(cwd)) || strcmp(cwd, getenv("HOME")) ||
      strcmp(cwd, getenv("TSUGUMI_WORK_DIR")) ||
      strcmp(getenv("TMPDIR"), getenv("TSUGUMI_TMP_DIR")) ||
      !getenv("TSUGUMI_OUTPUT_DIR") || !getenv("startup_key")) return 1;
  if (getenv("TSUGUMI_RUN_ID") || getenv("TSUGUMI_CARD") ||
      getenv("TSUGUMI_INVOCATION") || !getenv("FROM_SOURCE")) return 2;
  if (atoi(getenv("TSUGUMI_PARAMS_JSON")) != call->invocation) return 3;
  if ((getenv("FROM_MAIN") != NULL) != (call->invocation == 2)) return 4;
  setenv("FROM_MAIN", "transient", 1);
  return 0;
}
''')
    startup = {"LOAD_MARKER": str(tmp_path / "loaded"), "startup_key": "test-value"}
    with worker(tmp_path, startup=startup) as (p, data, _):
        send_call(data, tmp_path)
        assert receive(data).valueType == WorkerPayload.WorkerResult
        assert (tmp_path / "loaded").read_text() == "loaded"
        send_call(data, tmp_path, 2)
        assert receive(data).valueType == WorkerPayload.WorkerResult
        data.shutdown(socket.SHUT_WR)
        markers = p.stdout.read().splitlines()
        assert markers.count(b"card constructor") == markers.count(b"card load") == 1


def test_runtime_and_card_loading_wait_for_the_invocation(tmp_path):
    """Library initialization and source loading happen inside the first invocation."""
    library(tmp_path, r'''
#include <tsugumi/sdk/abi.h>
#include <stdio.h>
#include <stdlib.h>
__attribute__((constructor)) static void constructor(void) {
  puts("constructor ready"); fflush(stdout);
}
int tsugumi_main(const tsugumi_call* c) {
  if (c->phase == TSUGUMI_PREPARE) { puts("runtime ready"); fflush(stdout); }
  if (c->phase == TSUGUMI_LOAD) return tsugumi_display_text(c, c->run_id, 0);
  return 0;
}
''')
    with worker(tmp_path) as (p, data, _):
        send_call(data, tmp_path)
        display = receive(data)
        assert display.callId == 1 and display.valueType == WorkerPayload.WorkerDisplay
        assert display.value.display.runId == b"recorded-run"
        assert display.value.display.invocation == 1
        assert display.value.display.value.text == b"recorded-run"
        assert receive(data).valueType == WorkerPayload.WorkerResult
        send_call(data, tmp_path, 2)
        assert receive(data).valueType == WorkerPayload.WorkerResult
        data.shutdown(socket.SHUT_WR)
        markers = p.stdout.read().splitlines()
        assert markers.count(b"constructor ready") == markers.count(b"runtime ready") == 1


@pytest.mark.parametrize("stop", ["deadline", "cancel"])
def test_source_loading_obeys_the_call_stop(tmp_path, stop):
    """Loading has the call's deadline and cancellation; main never starts after a stop."""
    from Tsugumi.Worker.WorkerCancel import WorkerCancelT
    library(tmp_path, r'''
#include <tsugumi/sdk/abi.h>
#include <stdio.h>
#include <unistd.h>
int tsugumi_main(const tsugumi_call* c) {
  if (c->phase == TSUGUMI_LOAD) {
    puts("loading"); fflush(stdout);
    // The Card ABI exposes cancellation and remaining time as queries.
    while (!c->effects->cancelled(c->host) && c->effects->remaining_ms(c->host) != 0) usleep(1000);
  }
  if (c->phase == TSUGUMI_RUN) {
    puts("main ran");
    return 0;
  }
  return 0;
}
''')
    with worker(tmp_path) as (p, data, control):
        send_call(data, tmp_path, deadline=50 if stop == "deadline" else None)
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
        data.shutdown(socket.SHUT_WR)
        assert p.stdout.read() == b"", "main ran after loading was stopped"


def test_source_loading_failure_is_an_invocation_failure(tmp_path):
    """A failing loader returns a failure for the recorded call, with its stderr preserved."""
    library(tmp_path, r'''
#include <tsugumi/sdk/abi.h>
#include <stdio.h>
int tsugumi_main(const tsugumi_call* c) {
  if (c->phase == TSUGUMI_LOAD) { fputs("source failed\n", stderr); return 1; }
  return 0;
}
''')
    with worker(tmp_path) as (p, data, _):
        send_call(data, tmp_path)
        result = receive(data)
        assert result.callId == 1 and result.valueType == WorkerPayload.WorkerFailure
        assert p.stderr.readline() == b"source failed\n"


@pytest.mark.parametrize("source_failure", [False, True])
def test_embedded_python_import_is_inside_the_invocation(tmp_path, source_failure):
    """The real Python adapter imports Card code in the launch environment and reports import errors to that call."""
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
assert os.getcwd() == os.environ["HOME"]
assert "TSUGUMI_RUN_ID" not in os.environ
print("source imported", flush=True)
tsugumi.Report().text("loaded")
raise RuntimeError("import stopped")
'''.replace('raise RuntimeError("import stopped")',
            'raise tsugumi.Failure("worker_failed", "import stopped", source="src/notebook.py", line=8, cell="calculation")'
            if source_failure else 'raise RuntimeError("import stopped")'))
    defines = {
        "TSUGUMI_PY_HOME": sys.base_prefix,
        "TSUGUMI_LIBPYTHON": str(libdir / sysconfig.get_config_var("LDLIBRARY")),
    }
    entry = tmp_path / "entry.c"
    paths = str(ROOT / "sdk/python") + ":" + str(source)
    entry.write_text(f"const char tsugumi_python_path[] = {json.dumps(paths)};\n" + """
#include <tsugumi/sdk/abi.h>
extern int tsugumi_card_invoke(const tsugumi_call*);
int tsugumi_main(const tsugumi_call* c) {
  return tsugumi_card_invoke(c);
}
""")
    library = tmp_path / "lib/main.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-I", str(ROOT / "sdk/include"), "-I", str(include),
                    *["-D" + k + "=" + json.dumps(v) for k, v in defines.items()],
                    str(ROOT / "sdk/src/python_main.c"), str(entry), "-L", str(libdir),
                    "-lpython" + sysconfig.get_config_var("LDVERSION"), "-Wl,-rpath," + str(libdir),
                    "-o", str(library)], check=True)
    # Production WorkerProc drains logs while waiting for the result. A file
    # gives this direct socket fixture the same absence of pipe backpressure.
    with (tmp_path / "python.log").open("wb") as log, worker(tmp_path, stderr=log) as (p, data, _):
        send_call(data, tmp_path)
        shown = receive(data)
        assert shown.callId == 1 and shown.valueType == WorkerPayload.WorkerDisplay
        assert shown.value.display.value.text == b"loaded"
        failed = receive(data)
        assert failed.callId == 1 and failed.valueType == WorkerPayload.WorkerFailure
        assert failed.value.failure.kind == FailureKind.worker_failed
        if source_failure:
            assert failed.value.failure.diagnostic == b"import stopped"
            assert {a.key: a.value for a in failed.value.failure.args} == {
                b"card": b"card", b"source": b"src/notebook.py", b"line": b"8", b"cell": b"calculation"}
        else:
            assert b"import stopped" in failed.value.failure.diagnostic
        data.shutdown(socket.SHUT_WR)
        assert b"source imported" in p.stdout.read().splitlines()


def test_source_failure_position_survives_the_worker_reply(tmp_path):
    library(tmp_path, r'''
#include <tsugumi/sdk/abi.h>
int tsugumi_main(const tsugumi_call* c) {
  if (c->phase != TSUGUMI_RUN) return 0;
  c->effects->fail_at(c->host, "worker_failed", "cell traceback", "src/notebook.py", 8, "calculation");
  c->effects->fail(c->host, "output_invalid", "must not replace the first failure");
  return 1;
}
''')
    with worker(tmp_path) as (_, data, _):
        send_call(data, tmp_path)
        result = receive(data)
        assert result.callId == 1 and result.valueType == WorkerPayload.WorkerFailure
        failure = result.value.failure
        assert failure.kind == FailureKind.worker_failed
        assert failure.diagnostic == b"cell traceback"
        assert {a.key: a.value for a in failure.args} == {
            b"card": b"card", b"source": b"src/notebook.py", b"line": b"8", b"cell": b"calculation"}
