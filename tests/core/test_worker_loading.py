"""The native ABI using real Card libraries and framed socketpairs."""

import contextlib
import os
from pathlib import Path
import select
import shutil
import socket
import struct
import subprocess
import sys
import textwrap

import flatbuffers
import pytest

from harness.native_card import RESULT_WRITER

from Tsugumi.Card.CardFailure import CardFailureT
from Tsugumi.Card.CardPort import CardPortT
from Tsugumi.Card.CardResult import CardResultT
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Worker.EnvVar import EnvVarT
from Tsugumi.Worker.WorkerCall import WorkerCallT
from Tsugumi.Worker.WorkerCancel import WorkerCancelT
from Tsugumi.Worker.WorkerDisplay import WorkerDisplayT
from Tsugumi.Worker.WorkerMessage import WorkerMessage, WorkerMessageT
from Tsugumi.Worker.WorkerPayload import WorkerPayload
from Tsugumi.Worker.WorkerStart import WorkerStartT

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("TSUGUMI_BUILD_DIR", ROOT / "build"))
WORKER = Path(os.environ.get("TSUGUMI_WORKER", BUILD / "workerd/tsugumi-worker"))
PARAMS = bytes.fromhex("080000000400040004000000")


def packed(message, *, identifier=None):
    b = flatbuffers.Builder(256)
    b.Finish(message.Pack(b), file_identifier=identifier)
    return bytes(b.Output())


def c_bytes(value):
    return ", ".join(str(byte) for byte in value)


def library(tmp_path, source, name="main", *, result=None, include_abi=True):
    if not shutil.which("cc"):
        pytest.skip("needs a C compiler")
    lib = tmp_path / "lib"
    lib.mkdir(exist_ok=True)
    c = tmp_path / (name + ".c")
    result = packed(CardResultT(outputs=[])) if result is None else result
    preamble = "#include <tsugumi/card_abi.h>\n" + RESULT_WRITER if include_abi else ""
    c.write_text(preamble + textwrap.dedent(source).replace("RESULT_BYTES", c_bytes(result)))
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


def send_call(data, tmp_path, call_id=1, *, deadline=None, inputs=(), outputs=(), artifact_directory=None, grants=()):
    call = WorkerCallT(inputs=list(inputs), outputs=list(outputs),
                       artifactDirectory=artifact_directory, grants=list(grants), deadlineMs=deadline)
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


def test_worker_passes_inputs_and_schema_unopened(tmp_path):
    # Not a FlatBuffer: the worker hands input bytes to the Card's SDK as they are.
    port = CardPortT(name="reading", schema="producer-schema", bytes=list(PARAMS))
    library(tmp_path, f'''
        #include <tsugumi/card_abi.h>
        #include <stdio.h>
        #include <string.h>
        __attribute__((constructor)) static void loaded(void) {{ puts("loaded"); }}
        int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {{
          static const uint8_t wire[] = {{RESULT_BYTES}};
          if (call->abi_version != 0 || call->input.size < sizeof(wire)) return 4;
          const uint8_t* b = call->input.data;
          uint32_t offset, size;
          uint16_t field;
          memcpy(&offset, b, 4);
          b += offset;
          memcpy(&offset, b, 4);
          memcpy(&field, b - offset + 4, 2);
          b += field;
          memcpy(&offset, b, 4);
          b += offset + 4;
          memcpy(&offset, b, 4);
          b += offset;
          memcpy(&offset, b, 4);
          memcpy(&field, b - offset + 6, 2);
          const uint8_t* schema = b + field;
          memcpy(&offset, schema, 4);
          if (strcmp((const char*)schema + offset + 4, "producer-schema")) return 6;
          memcpy(&offset, b, 4);
          memcpy(&field, b - offset + 8, 2);
          b += field;
          memcpy(&offset, b, 4);
          b += offset;
          memcpy(&size, b, 4);
          static const uint8_t wanted[] = {{{c_bytes(PARAMS)}}};
          if (size != sizeof(wanted) || memcmp(b + 4, wanted, size)) return 5;
          return write_result(result, wire, sizeof(wire));
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
          if (call->abi_version != TSUGUMI_ABI_VERSION) return 1;
          return write_result(result, wire, sizeof(wire));
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
    ''', include_abi=False)
    with worker(tmp_path) as (_, data, _):
        for call_id in (1, 2):
            send_call(data, tmp_path, call_id)
            result = receive(data)
            assert result.callId == call_id
            assert result.valueType == WorkerPayload.WorkerFailure
            assert result.value.failure.kind == FailureKind.worker_failed
            assert b"returned 8" in result.value.failure.diagnostic


@pytest.mark.parametrize("kind", [FailureKind.validation_failed, 60000])
def test_encoded_failure_preserves_source(tmp_path, kind):
    library(tmp_path, '''
        #include <tsugumi/card_abi.h>
        int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
          static const uint8_t wire[] = {RESULT_BYTES};
          return write_result(result, wire, sizeof(wire));
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


def test_an_undecodable_result_fails_cleanly(tmp_path):
    """The worker only decodes the result; output names, required outputs and
    grants are checked on the host (accept_outputs, worker_execution_test.cc)."""
    library(tmp_path, '''
        #include <tsugumi/card_abi.h>
        int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
          static const uint8_t wire[] = {RESULT_BYTES};
          return write_result(result, wire, sizeof(wire));
        }
    ''', result=b"invalid")
    with worker(tmp_path) as (_, data, _):
        send_call(data, tmp_path)
        message = receive(data)
        assert message.valueType == WorkerPayload.WorkerFailure
        assert message.value.failure.kind == FailureKind.output_invalid


def test_resident_worker_releases_results(tmp_path):
    library(tmp_path, r"""
        #include <sys/resource.h>
        static long initial_peak;
        int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
          static const uint8_t wire[] = {RESULT_BYTES};
          static unsigned calls;
          struct rusage usage;
          if (getrusage(RUSAGE_SELF, &usage) != 0) return 2;
          long peak = usage.ru_maxrss;
          #ifndef __APPLE__
          peak *= 1024;
          #endif
          if (calls == 0) initial_peak = peak;
          if (peak - initial_peak > 64 * 1024 * 1024) return 3;

          // Commit one byte on each page. Without free the process grows
          // by 4 MiB on every invocation.
          const size_t allocation_size = 4 * 1024 * 1024;
          uint8_t* data = malloc(allocation_size);
          if (!data) return 4;
          volatile uint8_t* pages = data;
          for (size_t i = 0; i < allocation_size; i += 4096) pages[i] = 0;
          memcpy(data, wire, sizeof(wire));
          *result = (tsugumi_bytes){data, sizeof(wire)};
          return ++calls % 2 == 0 ? 0 : 5;
        }
    """)
    with worker(tmp_path) as (_, data, _):
        for call_id in range(1, 49):
            send_call(data, tmp_path, call_id)
            reply = receive(data)
            if call_id % 2:
                assert reply.valueType == WorkerPayload.WorkerFailure
                assert reply.value.failure.diagnostic == b"tsugumi_main returned 5"
            else:
                assert reply.valueType == WorkerPayload.WorkerResult


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
        send_call(data, tmp_path, deadline=2000 if stop == "deadline" else None)
        ready, _, _ = select.select([p.stdout, data], [], [], 5)
        if p.stdout not in ready and data in ready:
            early = receive(data)
            pytest.fail(f"worker returned before starting the Card thread: {early.value!r}")
        assert p.stdout in ready, "Card thread did not start"
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
          return write_result(result, wire, sizeof(wire));
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


def test_recv_after_return_and_reverse_credit(tmp_path):
    library(tmp_path, r"""
        #include <tsugumi/card_abi.h>
        #include <pthread.h>
        #include <string.h>
        static const tsugumi_effects* effects;
        static void* receive_value(void* host) {
          tsugumi_bytes data;
          if (effects->recv(host, "chat", &data, 3000) != 0) return (void*)1;
          if (data.size != 4 || memcmp(data.data, "\x00\xff\x80z", 4)) return (void*)2;
          return (void*)(intptr_t)effects->send(host, "echo", data.data, data.size);
        }
        int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
          static const uint8_t wire[] = {RESULT_BYTES};
          static pthread_t thread;
          if (!effects) {
            effects = call->effects;
            if (pthread_create(&thread, NULL, receive_value, call->host) != 0) return 1;
          } else {
            void* status;
            pthread_join(thread, &status);
            if (status) return 2;
          }
          return write_result(result, wire, sizeof(wire));
        }
    """)
    with worker(tmp_path) as (_, data, control):
        send_call(data, tmp_path)
        assert receive(data).valueType == WorkerPayload.WorkerResult
        payload = b"\x00\xff\x80z"
        send_message(control, WorkerMessageT(callId=0, valueType=WorkerPayload.WorkerDisplay,
                                            value=WorkerDisplayT(view="chat", bytes=payload)))
        frames = [receive(data), receive(data)]
        by_kind = {frame.valueType: frame.value for frame in frames}
        credit = by_kind[WorkerPayload.WorkerDisplayCredit]
        assert credit.view == b"chat" and credit.bytes == len(payload)
        echo = by_kind[WorkerPayload.WorkerDisplay]
        assert echo.view == b"echo" and bytes(echo.bytes) == payload
        send_call(data, tmp_path, 2)
        assert receive(data).valueType == WorkerPayload.WorkerResult


NUM = 'table Num { value:ulong; } table Outputs { out:Num (required); } root_type Outputs;'
DIRECTORY = 'table Document {} table Outputs { doc:Document (required); } root_type Outputs;'


def card_package(body, *, declarations=""):
    """Build a tiny native Card without an SDK dependency."""
    import json

    def literal(value):
        return json.dumps(value).replace("${", "\\${")

    sources = {
        "card_abi.h": (ROOT / "protocol/include/tsugumi/card_abi.h").read_text(),
        "card.fbs": (ROOT / "protocol/card.fbs").read_text(),
        "artifact.fbs": (ROOT / "protocol/artifact.fbs").read_text(),
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
in pkgs.runCommandCC "native-card" {{
  nativeBuildInputs = [ pkgs.flatbuffers ];
  buildInputs = [ pkgs.flatbuffers ];
}} ''
  cp ${{card_abi}} card_abi.h
  cp ${{card}} card.fbs
  cp ${{card_enums}} card_enums.fbs
  cp ${{artifact}} artifact.fbs
  cp ${{source}} card.cc
  flatc --cpp --scoped-enums card.fbs card_enums.fbs artifact.fbs
  mkdir -p "$out/lib"
  $CXX -std=c++20 -fPIC -shared -pthread card.cc -o "$out/lib/main.so"
''
"""


def native_card(body, *, inputs=(), outputs=NUM, declarations="", views="[]", **settings):
    return {
        "type": "code", "inputs": [{"name": name} for name in inputs],
        **settings, "_outputs": outputs,
        "_nix": "{ params, inputs }: let pkgs = import <nixpkgs> { }; in {\n  run = " +
        card_package(body, declarations=declarations) +
        f";\n  views = {views};\n}}",
    }


_CARD_PREAMBLE = r"""
#include <cstdlib>
#include <cstring>
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

  void add(const char* name, fb::FlatBufferBuilder& payload,
           fb::uoffset_t table) {
    payload.Finish(fb::Offset<fb::Table>(table));
    message.ForceVectorAlignment(payload.GetSize(), sizeof(uint8_t), FLATBUFFERS_MAX_ALIGNMENT);
    const auto bytes = message.CreateVector(payload.GetBufferPointer(), payload.GetSize());
    ports.push_back(C::CreateCardPort(message, message.CreateString(name), 0, bytes));
  }

  void number(uint64_t value, const char* port = "out") {
    fb::FlatBufferBuilder payload;
    const auto start = payload.StartTable();
    payload.AddElement<uint64_t>(4, value, 0);
    add(port, payload, payload.EndTable(start));
  }

  void artifact(const tsugumi_artifact_id& id, const char* port = "doc") {
    const tsugumi::ArtifactId fixed(fb::span<const uint8_t, 16>(id.bytes, 16));
    ports.push_back(C::CreateCardPort(message, message.CreateString(port), 0, 0,
                                     message.CreateVectorOfStructs(&fixed, 1)));
  }

  void fail(D::FailureKind kind, const char* error) {
    failure = C::CreateCardFailure(message, kind, message.CreateString(error));
  }
};

std::string create_artifact(const tsugumi_call& call, tsugumi_artifact_id& id) {
  tsugumi_bytes path;
  if (call.effects->artifact_create(call.host, &id, &path) != 0)
    throw std::runtime_error("artifact_create failed");
  return {reinterpret_cast<const char*>(path.data), path.size};
}

std::string open_artifact(const tsugumi_call& call, const C::CardInput& in,
                          std::string_view name = "doc") {
  for (const auto* port : *in.inputs()) {
    if (port->name()->string_view() != name || !port->artifacts() ||
        port->artifacts()->size() == 0) continue;
    tsugumi_artifact_id id;
    std::copy_n(port->artifacts()->Get(0)->bytes()->data(), 16, id.bytes);
    tsugumi_bytes path;
    if (call.effects->artifact_open(call.host, &id, &path) == 0)
      return {reinterpret_cast<const char*>(path.data), path.size};
  }
  return {};
}

"""

_CARD_ENTRY = r"""
extern "C" int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
  if (call->abi_version != TSUGUMI_ABI_VERSION) return 1;
  fb::FlatBufferBuilder builder;
  Result output{builder, {}, {}};
  const auto& input = *fb::GetRoot<C::CardInput>(call->input.data);
  const int code = run(*call, input, output);
  if (code != 0) return code;
  builder.Finish(C::CreateCardResult(builder, builder.CreateVector(output.ports), output.failure));
  auto* owned = static_cast<uint8_t*>(std::malloc(builder.GetSize()));
  if (!owned) return 1;
  std::memcpy(owned, builder.GetBufferPointer(), builder.GetSize());
  *result = {owned, builder.GetSize()};
  return 0;
}
"""
