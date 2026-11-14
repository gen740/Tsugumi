"""Typed C# Cards use the actual worker ABI, on the host or shared VM."""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import time

import pytest

from conftest import ROOT, Daemon, real_environment
from projects import put, run_and_wait, start_run, wait_run
from test_language_cards import write_project as write_language_project
from tsugumi_client import path, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState


def write_project(root, cards, connections):
    write_language_project(root, cards, connections)
    if os.environ.get("TSUGUMI_TEST_HOST_SANDBOX") == "1":
        for definition in root.glob("definitions/*/_card.json"):
            body = json.loads(definition.read_text())
            if "language" in body:
                body["sandbox"] = True
                definition.write_text(json.dumps(body))

NIX = '''{ pkgs, inputs, params }:
let sdk = inputs.tsugumi.sdk { languages = [ "csharp" ]; };
in { run = inputs.tsugumi.csharpCard { inherit sdk; src = ./src; }; }
'''
def nuget_nix(hash):
    """A Card with its own NuGet lock; `hash` is a Nix expression."""
    return NIX.replace("inherit sdk;", f"inherit sdk; lock = ./packages.lock.json; hash = {hash};")
# What `dotnet restore` with RestorePackagesWithLockFile writes for a net10.0
# project with <PackageReference Include="Newtonsoft.Json" Version="13.0.3" />
# (the procedure in docs/CSharp.md). Json.NET has no dependencies on net6.0+.
LOCK = {"version": 1, "dependencies": {"net10.0": {"Newtonsoft.Json": {
    "type": "Direct", "requested": "[13.0.3, )", "resolved": "13.0.3",
    "contentHash": "HrC5BXdl00IP9zeV+0Z848QWPAoCr9P3bDEZguI+gkLcBKAOxix/tLEAAHC+UvDNPv4a2d18lOReHMOagPa+zQ=="}}}}
# Another version under an otherwise unchanged lock. Its contentHash is never
# read: the fixed-output fetch is rejected before NuGet runs.
OTHER_LOCK = {"version": 1, "dependencies": {"net10.0": {"Newtonsoft.Json": {
    "type": "Direct", "requested": "[13.0.1, )", "resolved": "13.0.1", "contentHash": "not reached"}}}}
GOT = re.compile(r"got:\s*(sha256-[A-Za-z0-9+/]+=*)")
CPP_NIX = NIX.replace('"csharp"', '"cpp"').replace('csharpCard', 'cppCard')
NUMBERS = '''namespace sample;
table Numbers { values:[double]; label:string; }
table Outputs { numbers:Numbers (required); }
root_type Outputs;
'''
REPORT = '''attribute "tsugumi"; namespace sample;
table Location { value:string; }
table Outputs { report:Location (required, tsugumi:"directory"); }
root_type Outputs;
'''
CPP = '''#include <card.hh>
auto run(const Card::Inputs& inputs) -> Card::Output {
  Card::Output out;
  auto& value = out.numbers.emplace();
  value.values = {1.0, 2.0, 3.0}; value.label = "typed 日本語";
  return out;
}
'''
CSHARP = '''using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Tsugumi;
public sealed class Card : ICard {
  private Report? previous;
  private readonly string identity = Guid.NewGuid().ToString();
  private int runs;
  public ValueTask<Outputs> OnRunStartAsync(Inputs inputs) {
        runs++; return ValueTask.FromResult(new Outputs()); }
  public async ValueTask<Outputs> RunAsync(Inputs inputs, Report report, Run run) {
        var result = new Outputs();
    if (previous != null) {
      try { previous.Log("late callback"); throw new Exception("stale context remained live"); }
      catch (ObjectDisposedException) { }
    }
    previous = report;
    try { report.Text("\\uD800"); throw new Exception("invalid UTF-16 was accepted"); }
    catch (System.Text.EncoderFallbackException) { }
    var received = inputs.Numbers;
    if (received.Label != "typed 日本語") throw new Exception("input string was corrupted");
    if (inputs.Params.Mode == "fail") throw new Failure("validation_failed", "requested failure");
    if (inputs.Params.Mode is "wait" or "callback") {
      using var registration = inputs.Params.Mode == "callback" ? run.CancellationToken.Register(() => throw new InvalidOperationException("cancel callback failure")) : default;
      report.Log("waiting for cancellation");
      await Task.Delay(60000, run.CancellationToken);
    }
    await Task.Delay(1, run.CancellationToken);
    var values = received.Values.Select(x => x * (inputs.Params.Factor ?? throw new Failure("validation_failed", "factor is required"))).ToList();
    var roundTrip = Newtonsoft.Json.JsonConvert.DeserializeObject<double[]>(Newtonsoft.Json.JsonConvert.SerializeObject(values));
    if (roundTrip == null || !values.SequenceEqual(roundTrip)) throw new Exception("NuGet reflection round trip failed");
    report.Log($"worker {identity} runs {runs}");
    report.Text("intermediate");
    report.Markdown("**ready**");
    report.Scalar(values.Sum(), "sum");
    report.Image(Convert.FromBase64String("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=="), alt: "one pixel");
    report.Points([0], [1], [2], [0xff0000]);
    report.LineChart(new Series("a", "values", [1, 2, 3], values.ToArray()));
    report.Module("plotly", "{\\"data\\":[],\\"layout\\":{}}");
    report.Table(["sum", "worker", "runs"], [[values.Sum().ToString(System.Globalization.CultureInfo.InvariantCulture), identity, runs.ToString()]]);
    result.Numbers = new outputs.numbers.sample.NumbersT { Values = values, Label = received.Label };

        return result;
}
}
'''
PYTHON = '''import tsugumi
from inputs.numbers.sample.Numbers import Numbers
import outputs.report.sample.Location as Location
from pathlib import Path

def main(inputs, report, run):
    value = inputs.input("numbers", Numbers)
    values = [value.Values(i) for i in range(value.ValuesLength())]
    assert value.Label().decode() == "typed 日本語"
    target = Path(run.output_directory) / "report"
    target.mkdir()
    (target / "values.txt").write_text(",".join(str(x) for x in values))
    b = tsugumi.builder()
    name = b.CreateString(str(target))
    Location.Start(b); Location.AddValue(b, name)
    return {"report": tsugumi.message(b, Location.End(b))}
'''
PY_NIX = '''{ pkgs, inputs, params }:
let sdk = inputs.tsugumi.sdk { languages = [ "python" ]; };
in { run = inputs.tsugumi.pythonCard { inherit sdk; src = ./src; }; }
'''

RECEIPT = REPORT.replace('report:Location', 'receipt:Location').replace('"directory"', '"file"')
FILE_CARD = '''using System; using System.IO; using System.Threading.Tasks; using Tsugumi;
public sealed class Card : ICard {
 public ValueTask<Outputs> RunAsync(Inputs inputs, Report report, Run run) {
        var result = new Outputs();
  if (inputs.InputKind("report") != PortKind.Directory) throw new Exception("wrong port kind");
  string data = File.ReadAllText(Path.Combine(inputs.Report.Value, "values.txt"));
  string output = Path.Combine(run.OutputDirectory, "receipt.txt");
  File.WriteAllText(output, data);
  result.Receipt = new outputs.receipt.sample.LocationT { Value = output };
  return ValueTask.FromResult(result);
 }
}
'''

# These methods compile because ICard has default implementations, but none
# implements a supported entry point. Keep their rejection actionable.
INVALID_ENTRIES = [
    ("skipped-report", "public ValueTask<Outputs> RunAsync(Inputs inputs, Run run) => ValueTask.FromResult(new Outputs());",
     ["Card.RunAsync(Tsugumi.Inputs inputs, Tsugumi.Run run)", "parameter 2: expected Tsugumi.Report, found Tsugumi.Run"]),
    ("static", "public static ValueTask<Outputs> RunAsync(Inputs inputs, Run run) => ValueTask.FromResult(new Outputs());",
     ["public static", "must be an instance method"]),
    ("private", "private ValueTask<Outputs> RunAsync(Inputs inputs, Run run) => ValueTask.FromResult(new Outputs());",
     ["private instance", "must be public"]),
    ("task", "public Task<Outputs> RunAsync(Inputs inputs, Run run) => Task.FromResult(new Outputs());",
     ["Task<Tsugumi.Outputs>", "return type: expected System.Threading.Tasks.ValueTask<Tsugumi.Outputs>"]),
    ("typed-output", "public ValueTask<Other.Outputs> RunAsync(Inputs inputs, Run run) => ValueTask.FromResult(new Other.Outputs());",
     ["ValueTask<Other.Outputs>", "return type: expected System.Threading.Tasks.ValueTask<Tsugumi.Outputs>"]),
    ("foreign-input", "public ValueTask<Outputs> RunAsync(Other.Inputs inputs) => ValueTask.FromResult(new Outputs());",
     ["Other.Inputs inputs", "parameter 1: expected Tsugumi.Inputs, found Other.Inputs"]),
    ("missing", "", ["no RunAsync methods"]),
]


@pytest.mark.parametrize("name, method, expected", INVALID_ENTRIES, ids=[entry[0] for entry in INVALID_ENTRIES])
def test_invalid_entry_names_the_candidate_and_rejection(tmp_path, name, method, expected):
    root = tmp_path / name
    source = ("using Tsugumi; using System.Threading.Tasks; public sealed class Card : ICard {" + method
              + "} namespace Other { public class Inputs {} public class Outputs {} }")
    write_project(root, {"bad": {"card": {"language": "csharp", "inputs": [{"name": "start"}]},
                                "nix": NIX, "src": {"Card.cs": source}}}, [("start", "out", "bad", "start")])
    env, c = open_environment(root, "tsugumi-test-csharp-entry")
    try:
        rid, status = run_and_wait(c, "start", timeout=2400)
        assert status.state == RunState.failed
        failure = c.call("RunRecordRequest", runId=rid, card=path("bad")).status.failure
        assert failure.kind == FailureKind.worker_failed
        diagnostic = text(failure.diagnostic)
        assert "Found:" in diagnostic
        for detail in expected:
            assert detail in diagnostic, diagnostic
    finally:
        c.close()
        env.cleanup()


def open_environment(root, name):
    env = real_environment(root, name) if os.environ.get("TSUGUMI_TEST_CONTAINER") == "1" else Daemon(root).start()
    c = env.client()
    if isinstance(env, Daemon): c.hello()
    return env, c


PROBE = '''using System.Threading.Tasks; using Tsugumi;
public sealed class Card : ICard {
  public ValueTask<Outputs> RunAsync(Inputs inputs, Report report) {
        var result = new Outputs();
    report.Text(Newtonsoft.Json.JsonConvert.SerializeObject(new[] { 1, 2 }));
    return ValueTask.FromResult(result);
  }
}
'''


def lock_name(lock):
    """The fixed-output derivation csharpCard names after the lock file."""
    return "tsugumi-csharp-card-nuget-lock-" + hashlib.sha256(json.dumps(lock).encode()).hexdigest()[:16]


def build_lock(root, lock, hash):
    """Runs one C# Card that uses Json.NET from `lock`, pinned by `hash` (a Nix
    expression, or None to leave it out). Returns the run state, what the Card's
    build log and failure said, and its display text."""
    write_project(root, {"probe": {"card": {"language": "csharp", "inputs": [{"name": "start"}]},
        "nix": nuget_nix(hash) if hash else NIX.replace("inherit sdk;", "inherit sdk; lock = ./packages.lock.json;"),
        "src": {"Card.cs": PROBE}}}, [("start", "out", "probe", "start")])
    put(root / "definitions/probe/packages.lock.json", json.dumps(lock))
    env, c = open_environment(root, "tsugumi-test-csharp-lock")
    try:
        rid, status = run_and_wait(c, "start", timeout=2400)
        said = text(c.call("LogRangeRequest", runId=rid, card=path("probe"), stream=4, limitBytes=262144).text)
        if status.failure: said += '''
''' + text(status.failure.diagnostic)
        shown = None
        if status.state == RunState.succeeded:
            display = c.call("RunRecordRequest", runId=rid, card=path("probe")).record.display
            shown = text(display.value.text) if display.valueType == 1 else None
        return status.state, said, shown
    finally:
        c.close(); env.cleanup()


@pytest.fixture(scope="module")
def nuget_hash(tmp_path_factory):
    """The hash of LOCK's packages, found the way docs/CSharp.md teaches: run
    once with pkgs.lib.fakeHash and read `got:` in the Card's build log."""
    state, said, _ = build_lock(tmp_path_factory.mktemp("learn") / "csharp", LOCK, "pkgs.lib.fakeHash")
    assert state == RunState.failed, said
    assert lock_name(LOCK) in said, said  # the hash is named by what it covers
    found = GOT.search(said)
    assert found, said
    print(f"NAR hash of the Newtonsoft.Json 13.0.3 lock's packages: {found[1]}", flush=True)
    return found[1]


def test_a_lock_restores_offline_under_its_hash(tmp_path, nuget_hash):
    state, said, shown = build_lock(tmp_path / "pinned", LOCK, f'"{nuget_hash}"')
    assert state == RunState.succeeded and shown == "[1,2]", said
    # A changed lock is a new derivation: the old hash is checked against the
    # new packages rather than reusing the store path fetched for LOCK.
    state, said, _ = build_lock(tmp_path / "stale", OTHER_LOCK, f'"{nuget_hash}"')
    assert state == RunState.failed and lock_name(OTHER_LOCK) in said and "hash mismatch" in said, said


def test_the_demo_pins_what_its_lock_resolves_to(nuget_hash):
    """The bundled demo locks the same Json.NET, so its hash is the one found
    above (both languages)."""
    from pathlib import Path
    for language in ("en", "ja"):
        card = Path(__file__).resolve().parents[2] / "demos" / language / "csharp-calibration/definitions/calibrate"
        assert json.loads((card / "packages.lock.json").read_text()) == LOCK
        assert f'hash = "{nuget_hash}";' in (card / "default.nix").read_text(), f"pin {nuget_hash} in {card}/default.nix"


def test_a_lock_without_hash_says_what_to_pin(tmp_path):
    state, said, _ = build_lock(tmp_path / "unpinned", LOCK, None)
    assert state == RunState.failed
    assert "NAR hash" in said and "pkgs.lib.fakeHash" in said and lock_name(LOCK) in said, said


@pytest.fixture
def project(tmp_path, nuget_hash):
    root = tmp_path / "csharp"
    write_project(root, {
        "make": {"card": {"language":"cpp", "inputs":[{"name":"start"}]}, "nix":CPP_NIX, "outputs":NUMBERS, "src":{"card.cc":CPP}},
        "scale": {"card": {"language":"csharp", "reuseWorker":True, "inputs":[{"name":"numbers"}], "params":[
            {"name":"factor", "type":"double", "data":2}, {"name":"mode", "type":"string", "data":"ok"}]},
            "nix":nuget_nix(f'"{nuget_hash}"'), "outputs":NUMBERS, "src":{"Card.cs":'''#nullable enable
'''+CSHARP}},
        "report": {"card": {"language":"python", "inputs":[{"name":"numbers"}], "retainArtifact":True, "artifactOutput":"report"}, "nix":PY_NIX, "outputs":REPORT, "src":{"main.py":PYTHON}},
        "receipt": {"card": {"language":"csharp", "inputs":[{"name":"report"}], "retainArtifact":True, "artifactOutput":"receipt"}, "nix":NIX, "outputs":RECEIPT, "src":{"Card.cs":FILE_CARD}},
    }, [("start","out","make","start"),("make","numbers","scale","numbers"),("scale","numbers","report","numbers"),("report","report","receipt","report")])
    put(root / "definitions/scale/packages.lock.json", json.dumps(LOCK))
    env, c = open_environment(root, "tsugumi-test-csharp")
    yield root, env, c
    c.close(); env.cleanup()


def log(c, rid, card="scale"):
    return text(c.call("LogRangeRequest", runId=rid, card=path(card), stream=2, limitBytes=65536).text)


def run(c):
    rid, status = run_and_wait(c, "start", timeout=2400)
    if status.state != RunState.succeeded:
        print(text(c.call("LogRangeRequest", runId=rid, stream=4, limitBytes=262144).text), flush=True)
    assert status.state == RunState.succeeded, (status.failure and text(status.failure.diagnostic), log(c,rid,"make"), log(c,rid), log(c,rid,"report"), text(c.call("LogRangeRequest", runId=rid, stream=4, limitBytes=262144).text))
    return rid


def shown(c, rid):
    display = c.call("RunRecordRequest", runId=rid, card=path("scale")).record.display
    assert display.valueType == 4
    return [text(cell) for cell in display.value.rows[0].cells]


def test_typed_cpp_csharp_python_and_resident_lifetime(project):
    root, env, c = project
    first = run(c)
    second = run(c)
    a, b = shown(c, first), shown(c, second)
    assert a[0] == b[0] == "12"
    assert a[1] == b[1] and a[2] == "1" and b[2] == "2"
    artifacts = c.call("ArtifactListRequest", runId=first).artifacts
    report = next(item for item in artifacts if text(item.output)=="report")
    response = c.call("ArtifactReadRequest", artifactId=text(report.artifactId), member="values.txt")
    assert bytes(response.bytes) == b"2.0,4.0,6.0"

    receipt = next(item for item in artifacts if text(item.output)=="receipt")
    assert bytes(c.call("ArtifactReadRequest", artifactId=text(receipt.artifactId)).bytes) == b"2.0,4.0,6.0"


def edit(env, c, **updates):
    from test_demos import edit_card, set_param
    def update(body):
        for name, value in updates.items():
            if name in ("mode", "factor"): set_param(body, name, value)
            else: body[name] = value
    edit_card(env, c, "scale", update)


@pytest.mark.parametrize("mode", ["fail", "wait", "callback"])
def test_failure_cancel_deadline_and_recovery(project, mode):
    root, env, c = project
    run(c)  # Warm the real packages before measuring cancellation/deadline.
    edit(env, c, mode=mode, timeoutSeconds=1 if mode == "wait" else None)
    rid = start_run(c, "start")
    if mode == "callback":
        until = time.monotonic() + 30
        while "waiting for cancellation" not in log(c, rid):
            assert time.monotonic() < until, log(c, rid)
            time.sleep(.02)
        c.call("RunCancelRequest", runId=rid)
    status = wait_run(c, rid, timeout=30)
    assert status.state == (RunState.cancelled if mode == "callback" else RunState.failed)
    card_status = c.call("RunRecordRequest", runId=rid, card=path("scale")).status
    assert card_status.failure.kind == {
        "fail": FailureKind.validation_failed,
        "wait": FailureKind.timeout,
        "callback": FailureKind.cancelled,
    }[mode], log(c, rid)
    if mode == "fail":
        assert "requested failure" in log(c, rid)
    if mode == "callback":
        assert "cancel callback failure" in log(c, rid)
    edit(env, c, mode="ok", timeoutSeconds=None)
    recovered = run(c)
    assert shown(c, recovered)[0] == "12"


def test_no_reuse_creates_fresh_runtime(project):
    root, env, c = project
    edit(env, c, reuseWorker=False)
    a, b = shown(c, run(c)), shown(c, run(c))
    assert a[1] != b[1] and a[2] == b[2] == "1"


def test_creation_scaffold_copies_are_generated():
    """The Web (csharp.json) and Mac (CSharpTemplate.swift) creation menus read
    copies generated from ui/templates/csharp.py; neither is edited by hand."""
    import subprocess
    import sys
    from pathlib import Path
    script = Path(__file__).resolve().parents[2] / "ui/templates/csharp.py"
    result = subprocess.run([sys.executable, str(script), "--check"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr + result.stdout + " (run python3 ui/templates/csharp.py)"


def test_production_creation_scaffold(tmp_path):
    from pathlib import Path
    template = json.loads((Path(__file__).resolve().parents[2] / "ui/templates/csharp.json").read_text())
    root = tmp_path / "scaffold"
    write_project(root, {"hello": {"card":json.loads(template["_card.json"]),
        "nix":template["default.nix"], "outputs":template["_outputs.fbs"],
        "src":{name.removeprefix("src/"):body for name,body in template.items() if name.startswith("src/")}}},
        [("start","out","hello","in")])
    env = real_environment(root, "tsugumi-test-csharp-scaffold") if os.environ.get("TSUGUMI_TEST_CONTAINER") == "1" else Daemon(root).start()
    c = env.client()
    try:
        if isinstance(env, Daemon): c.hello()
        rid, status = run_and_wait(c, "start", timeout=2400)
        assert status.state == RunState.succeeded, (status.failure and text(status.failure.diagnostic), log(c,rid,"hello"))
        shown = c.call("RunRecordRequest", runId=rid, card=path("hello")).record.display
        assert shown.valueType == 1 and text(shown.value.text) == "Hello from C#"
    finally:
        c.close(); env.cleanup()

@pytest.mark.parametrize("language", ["ja", "en"])
def test_bundled_calibration_uses_its_locked_package(tmp_path, language):
    root = tmp_path / "csharp-calibration"
    shutil.copytree(ROOT / "demos" / language / "csharp-calibration", root)
    env, c = open_environment(root, "tsugumi-test-csharp-demo")
    try:
        run_id = run(c)
        items = c.call("ArtifactListRequest", runId=run_id).artifacts
        report = next(item for item in items if text(item.output) == "report")
        file = c.call("ArtifactReadRequest", artifactId=text(report.artifactId))
        assert bytes(file.bytes) == b"sample,value\n1,2\n2,4\n3,6\n"
    finally:
        c.close()
        env.cleanup()
