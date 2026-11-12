"""Typed C# Cards use the actual worker ABI, on the host or shared VM."""
from __future__ import annotations

import json
import os
import time

import pytest

from conftest import Daemon, real_environment
from projects import put, run_and_wait, start_run, wait_run
from test_language_cards import write_project as write_language_project
from tsugumi_client import path, text
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
NUGET_NIX = NIX.replace("inherit sdk;", 'inherit sdk; packages = [{ name = "Newtonsoft.Json"; version = "13.0.3"; }]; nugetDeps = ./nuget-deps.json;')
NUGET_DEPS = [{"pname":"Newtonsoft.Json", "version":"13.0.3", "hash":"sha256-hy/BieY4qxBWVVsDqqOPaLy1QobiIapkbrESm6v2PHc="}]
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
auto run(Card::Input&) -> Card::Output {
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
  private Context? previous;
  private readonly string identity = Guid.NewGuid().ToString();
  private int runs;
  public ValueTask OnRunStartAsync(Context context) { runs++; return ValueTask.CompletedTask; }
  public async ValueTask RunAsync(Context context) {
    if (previous != null) {
      try { previous.Log("late callback"); throw new Exception("stale context remained live"); }
      catch (ObjectDisposedException) { }
    }
    previous = context;
    try { context.DisplayText("\\uD800"); throw new Exception("invalid UTF-16 was accepted"); }
    catch (System.Text.EncoderFallbackException) { }
    var input = context.Inputs.Numbers;
    if (input.Label != "typed 日本語") throw new Exception("input string was corrupted");
    if (context.Parameters.Mode == "fail") throw new Failure("validation_failed", "requested failure");
    if (context.Parameters.Mode is "wait" or "callback") {
      using var registration = context.Parameters.Mode == "callback" ? context.CancellationToken.Register(() => throw new InvalidOperationException("cancel callback failure")) : default;
      context.Log("waiting for cancellation");
      await Task.Delay(60000, context.CancellationToken);
    }
    await Task.Delay(1, context.CancellationToken);
    var values = input.Values.Select(x => x * (context.Parameters.Factor ?? throw new Failure("validation_failed", "factor is required"))).ToList();
    var roundTrip = Newtonsoft.Json.JsonConvert.DeserializeObject<double[]>(Newtonsoft.Json.JsonConvert.SerializeObject(values));
    if (roundTrip == null || !values.SequenceEqual(roundTrip)) throw new Exception("NuGet reflection round trip failed");
    context.Log($"worker {identity} runs {runs}");
    context.DisplayText("intermediate");
    context.DisplayMarkdown("**ready**");
    context.DisplayScalar(values.Sum(), "sum");
    context.DisplayImage(Convert.FromBase64String("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=="), alt: "one pixel");
    context.DisplayPoints([0], [1], [2], [0xff0000]);
    context.DisplayLineChart(new Series("a", "values", [1, 2, 3], values.ToArray()));
    context.DisplayModule("plotly", "{\\"data\\":[],\\"layout\\":{}}");
    context.DisplayTable(["sum", "worker", "runs"], [[values.Sum().ToString(System.Globalization.CultureInfo.InvariantCulture), identity, runs.ToString()]]);
    context.Outputs.Numbers(new outputs.numbers.sample.NumbersT { Values = values, Label = input.Label });
  }
}
'''
PYTHON = '''from inputs.numbers.sample.Numbers import Numbers
import outputs.report.sample.Location as Location
from pathlib import Path

def main(ctx):
    value = ctx.input("numbers", Numbers)
    values = [value.Values(i) for i in range(value.ValuesLength())]
    assert value.Label().decode() == "typed 日本語"
    target = Path(ctx.output_directory) / "report"
    target.mkdir()
    (target / "values.txt").write_text(",".join(str(x) for x in values))
    b = ctx.builder()
    name = b.CreateString(str(target))
    Location.Start(b); Location.AddValue(b, name)
    ctx.output("report", b, Location.End(b))
'''
PY_NIX = '''{ pkgs, inputs, params }:
let sdk = inputs.tsugumi.sdk { languages = [ "python" ]; };
in { run = inputs.tsugumi.pythonCard { inherit sdk; src = ./src; }; }
'''

RECEIPT = REPORT.replace('report:Location', 'receipt:Location').replace('"directory"', '"file"')
FILE_CARD = """using System; using System.IO; using System.Threading.Tasks; using Tsugumi;
public sealed class Card : ICard {
 public ValueTask RunAsync(Context context) {
  if (context.InputKind("report") != PortKind.Directory) throw new Exception("wrong port kind");
  string data = File.ReadAllText(Path.Combine(context.Inputs.Report.Value, "values.txt"));
  string output = Path.Combine(context.OutputDirectory, "receipt.txt");
  File.WriteAllText(output, data);
  context.Outputs.Receipt(new outputs.receipt.sample.LocationT { Value = output });
  return ValueTask.CompletedTask;
 }
}
"""

@pytest.fixture
def project(tmp_path):
    root = tmp_path / "csharp"
    write_project(root, {
        "make": {"card": {"language":"cpp", "inputs":[{"name":"start"}]}, "nix":CPP_NIX, "outputs":NUMBERS, "src":{"card.cc":CPP}},
        "scale": {"card": {"language":"csharp", "inputs":[{"name":"numbers"}], "params":[
            {"name":"factor", "type":"double", "data":2}, {"name":"mode", "type":"string", "data":"ok"}]},
            "nix":NUGET_NIX, "outputs":NUMBERS, "src":{"Card.cs":"#nullable enable\n"+CSHARP}},
        "report": {"card": {"language":"python", "inputs":[{"name":"numbers"}], "retainArtifact":True, "artifactOutput":"report"}, "nix":PY_NIX, "outputs":REPORT, "src":{"main.py":PYTHON}},
        "receipt": {"card": {"language":"csharp", "inputs":[{"name":"report"}], "retainArtifact":True, "artifactOutput":"receipt"}, "nix":NIX, "outputs":RECEIPT, "src":{"Card.cs":FILE_CARD}},
    }, [("start","out","make","start"),("make","numbers","scale","numbers"),("scale","numbers","report","numbers"),("report","report","receipt","report")])
    put(root / "definitions/scale/nuget-deps.json", json.dumps(NUGET_DEPS))
    env = real_environment(root, "tsugumi-test-csharp") if os.environ.get("TSUGUMI_TEST_CONTAINER") == "1" else Daemon(root).start()
    c = env.client()
    if isinstance(env, Daemon): c.hello()
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
    assert status.state in (RunState.failed, RunState.cancelled)
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
