"""The native ABI using real Card libraries and framed socketpairs."""

import contextlib
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import textwrap

import flatbuffers
import pytest

from Tsugumi.Card.CardFailure import CardFailureT
from Tsugumi.Card.CardPort import CardPortT
from Tsugumi.Card.CardResult import CardResultT
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.PortKind import PortKind
from Tsugumi.Worker.EnvVar import EnvVarT
from Tsugumi.Worker.WorkerCall import WorkerCallT
from Tsugumi.Worker.WorkerCancel import WorkerCancelT
from Tsugumi.Worker.WorkerPort import WorkerPortT
from Tsugumi.Worker.WorkerMessage import WorkerMessage, WorkerMessageT
from Tsugumi.Worker.WorkerPayload import WorkerPayload
from Tsugumi.Worker.WorkerStart import WorkerStartT

ROOT = Path(__file__).resolve().parents[2]
WORKER = Path(os.environ.get("TSUGUMI_WORKER", ROOT / "build/workerd/tsugumi-worker"))
PARAMS = bytes.fromhex("080000000400040004000000")


def packed(message, *, identifier=None):
    b = flatbuffers.Builder(256)
    b.Finish(message.Pack(b), file_identifier=identifier)
    return bytes(b.Output())


def c_bytes(value):
    return ", ".join(str(byte) for byte in value)


def library(tmp_path, source, name="main", *, result=None):
    if not shutil.which("cc"):
        pytest.skip("needs a C compiler")
    lib = tmp_path / "lib"
    lib.mkdir(exist_ok=True)
    c = tmp_path / (name + ".c")
    result = packed(CardResultT(outputs=[])) if result is None else result
    c.write_text(textwrap.dedent(source).replace("RESULT_BYTES", c_bytes(result)))
    subprocess.run(
        ["cc", "-shared", "-fPIC", "-pthread", "-I", str(ROOT / "protocol/include"),
         str(c), "-o", str(lib / (name + ".so"))],
        check=True,
    )


@contextlib.contextmanager
def worker(package, *, startup=None, arguments=()):
    with contextlib.ExitStack() as stack:
        data, child_data = socket.socketpair()
        control, child_control = socket.socketpair()
        for channel in (data, child_data, control, child_control):
            stack.enter_context(channel)
        data.settimeout(5)
        launch = textwrap.dedent("""
            import fcntl, os, sys
            a, b = map(int, sys.argv[1:3])
            x, y = [fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 10) for fd in (a, b)]
            os.dup2(x, 3)
            os.dup2(y, 4)
            os.execv(sys.argv[3], sys.argv[3:])
        """)
        if not arguments:
            arguments = (str(WORKER), "--package", str(package))
        cwd = package.resolve()
        p = subprocess.Popen(
            [sys.executable, "-c", launch, str(child_data.fileno()),
             str(child_control.fileno()), *arguments],
            pass_fds=(child_data.fileno(), child_control.fileno()),
            cwd=cwd, env={**os.environ, "HOME": str(cwd), "TMPDIR": str(cwd / "tmp")},
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        child_data.close()
        child_control.close()
        if startup:
            send_message(data, WorkerMessageT(
                valueType=WorkerPayload.WorkerStart,
                value=WorkerStartT(environment=[
                    EnvVarT(name=k, value=v) for k, v in startup.items()
                ]),
            ))
        try:
            yield p, data, control
        finally:
            p.kill()
            out, err = p.communicate(timeout=5)
            if p.returncode not in (-9, 0):
                print("worker exit", p.returncode, out, err)


def send_call(data, tmp_path, call_id=1, *, deadline=None, inputs=(), outputs=(), params=PARAMS):
    call = WorkerCallT(inputs=list(inputs), outputs=list(outputs),
                       params=list(params), deadlineMs=deadline)
    send_message(data, WorkerMessageT(
        callId=call_id, valueType=WorkerPayload.WorkerCall, value=call,
    ))


def send_message(data, message):
    frame = packed(message, identifier=b"TSW1")
    data.sendall(struct.pack("<I", len(frame)) + frame)


def receive(data):
    def full(n):
        out = bytearray()
        while len(out) < n:
            part = data.recv(n - len(out))
            assert part, "worker ended without a result"
            out.extend(part)
        return out

    size, = struct.unpack("<I", full(4))
    return WorkerMessageT.InitFromObj(WorkerMessage.GetRootAs(full(size)))


def test_worker_passes_inputs_unopened_and_preserves_binary_parameters(tmp_path):
    # Not a FlatBuffer: the worker hands input bytes to the Card's SDK as they are.
    port = CardPortT(name="reading", kind=PortKind.FlatBuffer, bytes=list(PARAMS[:4]))
    library(tmp_path, f'''
        #include <tsugumi/card_abi.h>
        #include <stdio.h>
        #include <string.h>
        __attribute__((constructor)) static void loaded(void) {{ puts("loaded"); }}
        int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {{
          static const uint8_t wire[] = {{RESULT_BYTES}};
          if (call->abi_version != 9 || call->input.size < sizeof(wire)) return 4;
          // Read the params vector in the CardInput table and inspect its
          // contents including embedded NUL bytes.
          const uint8_t* b = call->input.data;
          uint32_t root, offset, size;
          uint16_t params;
          memcpy(&root, b, 4);
          memcpy(&offset, b + root, 4);
          memcpy(&params, b + root - offset + 6, 2);
          b += root + params;
          memcpy(&offset, b, 4);
          b += offset;
          memcpy(&size, b, 4);
          static const uint8_t wanted[] = {{{c_bytes(PARAMS)}}};
          if (size != sizeof(wanted) || memcmp(b + 4, wanted, size)) return 5;
          *result = (tsugumi_bytes){{wire, sizeof(wire)}};
          return 0;
        }}
    ''')
    with worker(tmp_path) as (p, data, _):
        send_call(data, tmp_path, inputs=[port])
        assert receive(data).valueType == WorkerPayload.WorkerResult
        data.shutdown(socket.SHUT_WR)
        assert p.stdout.read() == b"loaded\n"


def test_worker_loads_once_with_launch_secrets_and_no_legacy_environment(tmp_path):
    library(tmp_path, r'''
        #include <tsugumi/card_abi.h>
        #include <stdio.h>
        #include <stdlib.h>
        #include <string.h>
        #include <unistd.h>
        static int loaded;
        __attribute__((constructor)) static void load(void) {
          puts("card constructor");
          loaded = getenv("startup_key") && !strcmp(getenv("startup_key"), "test-value");
        }
        int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
          static const uint8_t wire[] = {RESULT_BYTES};
          char cwd[4096];
          if (!loaded || !getcwd(cwd, sizeof(cwd)) ||
              strcmp(cwd, getenv("HOME")) || !getenv("TMPDIR")) return 1;
          if (getenv("TSUGUMI_RUN_ID") || getenv("TSUGUMI_CARD") ||
              getenv("TSUGUMI_INVOCATION") || getenv("TSUGUMI_PARAMS_JSON") ||
              getenv("TSUGUMI_OUTPUT_DIR") || getenv("TSUGUMI_TMP_DIR") ||
              getenv("TSUGUMI_WORK_DIR")) return 2;
          *result = (tsugumi_bytes){wire, sizeof(wire)};
          return call->abi_version != 9;
        }
    ''')
    with worker(tmp_path, startup={"startup_key": "test-value"}) as (p, data, _):
        for call_id in (1, 2):
            send_call(data, tmp_path, call_id)
            assert receive(data).valueType == WorkerPayload.WorkerResult
        data.shutdown(socket.SHUT_WR)
        assert p.stdout.read() == b"card constructor\n"


def test_v8_card_reports_failure_cleanly(tmp_path):
    library(tmp_path, '''
        #include <stdint.h>
        int tsugumi_main(const void* call) {
          if (*(const uint32_t*)call != 8) return 8;
          __builtin_trap();
        }
    ''')
    with worker(tmp_path) as (_, data, _):
        for call_id in (1, 2):
            send_call(data, tmp_path, call_id)
            result = receive(data)
            assert result.callId == call_id
            assert result.valueType == WorkerPayload.WorkerFailure
            assert result.value.failure.kind == FailureKind.worker_failed
            assert b"returned 8" in result.value.failure.diagnostic


@pytest.mark.parametrize("kind", [FailureKind.type_mismatch, 60000])
def test_encoded_failure_preserves_source(tmp_path, kind):
    library(tmp_path, '''
        #include <tsugumi/card_abi.h>
        int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
          static const uint8_t wire[] = {RESULT_BYTES};
          *result = (tsugumi_bytes){wire, sizeof(wire)};
          return 0;
        }
    ''', result=packed(CardResultT(failure=CardFailureT(
        kind=kind, message="bad row", source="src/card.cc", line=31, cell="sample",
    ))))
    with worker(tmp_path) as (_, data, _):
        send_call(data, tmp_path)
        message = receive(data)
        assert message.valueType == WorkerPayload.WorkerFailure
        failure = message.value.failure
        assert failure.kind == (kind if kind < 60000 else FailureKind.worker_failed)
        assert failure.diagnostic == b"bad row"
        assert {arg.key: arg.value for arg in failure.args} == {
            b"source": b"src/card.cc", b"line": b"31", b"cell": b"sample",
        }


@pytest.mark.parametrize("result", ["invalid", "missing", "unknown", "wrong-kind", "duplicate", "path"])
def test_invalid_results_fail_cleanly(tmp_path, result):
    declaration = WorkerPortT(name="reading", kind=PortKind.FlatBuffer, required=True)
    output = CardPortT(name="reading", kind=PortKind.FlatBuffer, bytes=PARAMS)
    if result == "unknown":
        output.name = "extra"
    elif result == "wrong-kind":
        output.kind = PortKind.Path
    elif result == "path":
        output.path = "file"
    card = CardResultT(outputs={"missing": [], "duplicate": [output, output]}.get(result, [output]))
    library(tmp_path, '''
        #include <tsugumi/card_abi.h>
        int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
          static const uint8_t wire[] = {RESULT_BYTES};
          *result = (tsugumi_bytes){wire, sizeof(wire)};
          return 0;
        }
    ''', result=b"invalid" if result == "invalid" else packed(card))
    with worker(tmp_path) as (_, data, _):
        send_call(data, tmp_path, outputs=[declaration])
        message = receive(data)
        assert message.valueType == WorkerPayload.WorkerFailure
        assert message.value.failure.kind == FailureKind.output_invalid


@pytest.mark.parametrize("stop", ["deadline", "cancel"])
def test_stop_callbacks_are_callable_from_card_threads(tmp_path, stop):
    library(tmp_path, '''
        #include <tsugumi/card_abi.h>
        #include <pthread.h>
        #include <stdio.h>
        #include <unistd.h>
        static void* query(void* data) {
          const tsugumi_call* call = data;
          puts("thread ready");
          fflush(stdout);
          while (!call->effects->cancelled(call->host) &&
                 call->effects->remaining_ms(call->host) != 0) usleep(1000);
          return NULL;
        }
        int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
          pthread_t thread;
          if (pthread_create(&thread, NULL, query, (void*)call) != 0) return 1;
          pthread_join(thread, NULL);
          return 0;
        }
    ''')
    with worker(tmp_path) as (p, data, control):
        send_call(data, tmp_path, deadline=300 if stop == "deadline" else None)
        assert p.stdout.readline() == b"thread ready\n"
        if stop == "cancel":
            send_message(control, WorkerMessageT(
                callId=1, valueType=WorkerPayload.WorkerCancel, value=WorkerCancelT(),
            ))
        result = receive(data)
        assert result.valueType == WorkerPayload.WorkerFailure
        assert result.value.failure.kind == (
            FailureKind.timeout if stop == "deadline" else FailureKind.cancelled
        )


def test_post_return_send_keeps_opaque_bytes_and_original_view(tmp_path):
    library(tmp_path, '''
        #include <tsugumi/card_abi.h>
        #include <pthread.h>
        #include <stdio.h>
        #include <unistd.h>
        static void* display(void* host) {
          while (access("send-now", F_OK) != 0) usleep(1000);
          static const uint8_t bytes[] = {0, 255, 128, 'z'};
          extern const tsugumi_effects* effects;
          return (void*)(intptr_t)effects->send(host, "undeclared", bytes, sizeof(bytes));
        }
        const tsugumi_effects* effects;
        int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
          static const uint8_t wire[] = {RESULT_BYTES};
          static pthread_t thread;
          if (!effects) {
            effects = call->effects;
            if (pthread_create(&thread, NULL, display, call->host) != 0) return 1;
          } else {
            void* status;
            pthread_join(thread, &status);
            if (status) return 2;
          }
          *result = (tsugumi_bytes){wire, sizeof(wire)};
          return 0;
        }
    ''')
    with worker(tmp_path) as (_, data, _):
        send_call(data, tmp_path)
        assert receive(data).valueType == WorkerPayload.WorkerResult
        (tmp_path / "send-now").touch()
        message = receive(data)
        assert message.callId == 0
        assert message.valueType == WorkerPayload.WorkerDisplay
        assert message.value.view == b"undeclared"
        assert bytes(message.value.bytes) == b"\x00\xff\x80z"
        send_call(data, tmp_path, 2)
        assert receive(data).valueType == WorkerPayload.WorkerResult


NUM = 'table Num { value:ulong; } table Outputs { out:Num (required); } root_type Outputs;'
DIRECTORY = ('attribute "tsugumi"; table Directory { value:string; } '
             'table Outputs { doc:Directory (required, tsugumi: "path"); } '
             'root_type Outputs;')


def card_package(body, *, declarations=""):
    """Build a tiny native Card without an SDK dependency."""
    import json

    def literal(value):
        return json.dumps(value).replace("${", "\\${")

    sources = {
        "card_abi.h": (ROOT / "protocol/include/tsugumi/card_abi.h").read_text(),
        "card.fbs": (ROOT / "protocol/card.fbs").read_text(),
        "card_enums.fbs": (ROOT / "protocol/card_enums.fbs").read_text(),
        "card.cc": _CARD_PREAMBLE + "\n" + declarations +
        "\nint run(const tsugumi_call& call, const C::CardInput& in, Result& out) {\n" +
        body + "\nreturn 0;\n}\n" + _CARD_ENTRY,
    }
    bindings = "\n".join(
        f'{name.split(".")[0]} = pkgs.writeText "{name}" {literal(source)};'
        for name, source in sources.items() if name != "card.cc"
    )
    bindings += f'\nsource = pkgs.writeText "card.cc" {literal(sources["card.cc"])};'
    return f"""
let
{bindings}
in pkgs.runCommandCC "abi9-native-card" {{
  nativeBuildInputs = [ pkgs.flatbuffers ];
  buildInputs = [ pkgs.flatbuffers ];
}} ''
  cp ${{card_abi}} card_abi.h
  cp ${{card}} card.fbs
  cp ${{card_enums}} card_enums.fbs
  cp ${{source}} card.cc
  flatc --cpp --scoped-enums card.fbs card_enums.fbs
  mkdir -p "$out/lib"
  $CXX -std=c++20 -fPIC -shared -pthread card.cc -o "$out/lib/main.so"
''
"""


def native_card(body, *, inputs=(), outputs=NUM, declarations="", views="[]", **settings):
    return {
        "type": "code", "inputs": [{"name": name} for name in inputs],
        **settings, "_outputs": outputs,
        "_nix": "{ pkgs, inputs, params }: {\n  run = " +
        card_package(body, declarations=declarations) +
        f";\n  views = {views};\n}}",
    }


_CARD_PREAMBLE = r"""
#include "card_abi.h"
#include "card_generated.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace C = Tsugumi::Card;
namespace D = Tsugumi::Data;
namespace fs = std::filesystem;
namespace fb = flatbuffers;

struct Result {
  fb::FlatBufferBuilder& message;
  std::vector<fb::Offset<C::CardPort>> ports;
  fb::Offset<C::CardFailure> failure;

  void add(const char* name, D::PortKind kind, fb::FlatBufferBuilder& payload,
           fb::uoffset_t table) {
    payload.Finish(fb::Offset<fb::Table>(table));
    message.ForceVectorAlignment(payload.GetSize(), sizeof(uint8_t), FLATBUFFERS_MAX_ALIGNMENT);
    const auto bytes = message.CreateVector(payload.GetBufferPointer(), payload.GetSize());
    ports.push_back(C::CreateCardPort(message, message.CreateString(name), kind, bytes));
  }

  void number(uint64_t value, const char* port = "out") {
    fb::FlatBufferBuilder payload;
    const auto start = payload.StartTable();
    payload.AddElement<uint64_t>(4, value, 0);
    add(port, D::PortKind::FlatBuffer, payload, payload.EndTable(start));
  }

  // A Path output: a file or directory under the working directory, or an
  // input path returned unchanged.
  void path(const std::string& value, const char* port = "doc") {
    ports.push_back(C::CreateCardPort(message, message.CreateString(port),
                                      D::PortKind::Path, 0,
                                      message.CreateString(value)));
  }

  void fail(D::FailureKind kind, const char* error) {
    failure = C::CreateCardFailure(message, kind, message.CreateString(error));
  }
};

std::string input_path(const C::CardInput& in, std::string_view name = "doc") {
  for (const auto* port : *in.inputs()) {
    if (port->name()->string_view() != name) continue;
    return port->path() ? port->path()->str() : std::string();
  }
  return {};
}
"""

_CARD_ENTRY = r"""
extern "C" int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
  if (call->abi_version != TSUGUMI_ABI_VERSION) return 1;
  static fb::FlatBufferBuilder builder;
  builder.Clear();
  Result output{builder, {}, {}};
  const auto& input = *fb::GetRoot<C::CardInput>(call->input.data);
  const int code = run(*call, input, output);
  if (code != 0) return code;
  builder.Finish(C::CreateCardResult(builder, builder.CreateVector(output.ports), output.failure));
  *result = {builder.GetBufferPointer(), builder.GetSize()};
  return 0;
}
"""
