"""The Python role boundary and result stream against the real FlatBuffers API.

No daemon or sockets: the host stub records precisely when a callback occurs.
"""
import importlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture
def sdk(tmp_path, monkeypatch):
    pytest.importorskip("flatbuffers")
    shutil.copytree(ROOT / "sdk/python/tsugumi", tmp_path / "tsugumi")
    contract = {"inputs": [{"name": "number", "type": "inputs.number.sample.Number", "kind": "flatbuffer", "required": True}, {"name": "optional", "type": "inputs.number.sample.Number", "kind": "flatbuffer", "required": False}],
                "outputs": [{"name": name, "type": "outputs.result.sample.Number", "kind": "flatbuffer", "required": name == "result"} for name in ("result", "next", "last")]}
    (tmp_path / "_contract.json").write_text(json.dumps(contract))
    for name, schema in {
        "inputs": "namespace inputs.number.sample; table Number { value:ulong; }",
        "outputs": "namespace outputs.result.sample; table Number { value:ulong; }",
        "params": "namespace params; table Params { threshold:ulong = null; samples:[double]; labels:[string]; } root_type Params;",
    }.items():
        path = tmp_path / (name + ".fbs")
        path.write_text(schema)
        subprocess.run([os.environ.get("TSUGUMI_FLATC", "flatc"), "--python", "--gen-object-api", "-o", str(tmp_path), str(path)], check=True)
    (tmp_path / "_tsugumi_host.py").write_text('''
import os
events = []
values = []
parameter_bytes = b""
accept = True
def inputs(): return values
def params(): return parameter_bytes
def info():
    events.append("info")
    return dict(run_id="test", card="step", invocation=7, working_directory="work", temporary_directory="temp", output_directory="outputs")
def environ(): return list(os.environb.items())
def finish_outputs(): return accept
def emit_output(port, value): events.append((port, bytes(value))); return accept
def display_text(text): events.append(("text", text))
def log(text): events.append(("log", text))
def cancelled(): return False
def remaining_ms(): return -1
def fail(kind, message): events.append(("failure", kind, message))
''')
    # Each test uses a distinct generated package and module lifetime.
    names = [n for n in sys.modules if n.split(".")[0] in ("tsugumi", "_tsugumi_host", "inputs", "outputs", "params")]
    saved = {n: sys.modules.pop(n) for n in names}
    monkeypatch.syspath_prepend(str(tmp_path))
    try:
        module = importlib.import_module("tsugumi")
        host = importlib.import_module("_tsugumi_host")
        from inputs.number.sample.Number import NumberT
        from params.Params import ParamsT
        number = NumberT(); number.value = 9007199254740993
        parameters = ParamsT(); parameters.threshold = 21; parameters.samples = [1., 2.]; parameters.labels = [b"a", b"b"]
        host.values = [("number", 3, module.message(number))]
        host.parameter_bytes = module.message(parameters)
        yield module, host
    finally:
        for name in list(sys.modules):
            if name.split(".")[0] in ("tsugumi", "_tsugumi_host", "inputs", "outputs", "params"):
                del sys.modules[name]
        sys.modules.update(saved)


def test_inputs_are_read_only_and_run_is_opt_in(sdk):
    module, host = sdk
    def main(inputs):
        assert inputs.number.value == 9007199254740993
        assert inputs.optional is None and not inputs.has_input("optional")
        assert inputs.params.threshold == 21
        assert len(inputs.params.samples) == 2 and inputs.params.labels[1] == b"b"
        with pytest.raises((TypeError, ValueError)): inputs.params.samples[0] = 3
        with pytest.raises(AttributeError): inputs.number.value = 1
        with pytest.raises(AttributeError): inputs.params.threshold = 0
        with pytest.raises(AttributeError): inputs.number = None
        return {"result": inputs.input_bytes("number")}
    module._outputs(module._call(main))
    assert "info" not in host.events
    assert host.events[0][0] == "result"


def test_generated_objects_builders_and_bytes_are_results(sdk):
    module, host = sdk
    from outputs.result.sample.Number import Number, NumberT
    number = NumberT(); number.value = 42
    b = module.builder(); root = number.Pack(b)
    values = [number, module.message(b, root), b]
    for value in values: module._outputs({"result": value})
    assert [Number.GetRootAs(event[1], 0).Value() for event in host.events] == [42, 42, 42]


def test_stream_is_lazy_ordered_and_keeps_final_result(sdk):
    module, host = sdk
    def main(inputs, report, run):
        assert run.invocation == 7 and run.remaining_ms() == -1
        report.text("before")
        yield {"result": inputs.input_bytes("number")}
        report.log("between")
        yield {"next": inputs.input_bytes("number")}
        return {"last": inputs.input_bytes("number")}
    def body():
        result = module._call(main)
        assert host.events == ["info"]
        module._outputs(result)
    assert module._guard(body)
    assert [e[0] for e in host.events[1:]] == ["text", "result", "log", "next", "last"]


def test_hook_uses_same_roles_and_rejection_fails_invocation(sdk):
    module, host = sdk
    from types import SimpleNamespace
    def hook(inputs, report):
        report.log("start")
        return {"result": inputs.input_bytes("number")}
    module._card_module = SimpleNamespace(on_run_start=hook, main=hook)
    assert module._run_start()
    host.accept = False
    assert not module._invoke()
    assert host.events[-1][0:2] == ("failure", "output_invalid")


def test_run_expires_between_calls_and_during_the_next_call(sdk):
    module, host = sdk
    saved = []
    assert module._guard(lambda: saved.append(module.Run()))
    for method in (saved[0].cancelled, saved[0].remaining_ms):
        with pytest.raises(module.Failure, match="expired"):
            method()
    def next_call():
        for method in (saved[0].cancelled, saved[0].remaining_ms):
            with pytest.raises(module.Failure, match="expired"):
                method()
        current = module.Run()
        assert current.cancelled() is False and current.remaining_ms() == -1
    assert module._guard(next_call)


def test_run_callbacks_reject_background_threads(sdk):
    import threading
    module, host = sdk
    errors = []
    def body():
        run = module.Run()
        def background():
            for method in (run.cancelled, run.remaining_ms):
                try:
                    method()
                except module.Failure as error:
                    errors.append(str(error))
        thread = threading.Thread(target=background)
        thread.start()
        thread.join()
    assert module._guard(body)
    assert len(errors) == 2


def test_required_output_completion_fails_inside_the_language_guard(sdk):
    from types import SimpleNamespace
    module, host = sdk
    host.accept = False
    module._card_module = SimpleNamespace(main=lambda inputs: {})
    assert module._invoke() is False
    assert host.events[-1] == ('failure', 'output_invalid', 'required outputs were not emitted')


def test_python_reads_the_shared_numeric_vectors_without_changing_bits(sdk):
    import math
    import struct
    module, host = sdk
    from params.Params import ParamsT
    vectors = json.loads((ROOT / 'tests/fixtures/number-vectors.json').read_text())
    params = ParamsT()
    params.samples = [float(token) for token in vectors['values']]
    params.labels = vectors['strings']
    host.parameter_bytes = module.message(params)
    inputs = module.Inputs()
    assert [value.decode() for value in inputs.params.labels] == vectors['strings']
    for actual, expected in zip(inputs.params.samples, params.samples, strict=True):
        assert math.isnan(actual) if math.isnan(expected) else struct.pack('<d', actual) == struct.pack('<d', expected)
