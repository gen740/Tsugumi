"""R and Julia Cards (rCard, juliaCard) and the display of bash Cards: params,
messages and File outputs cross between languages through the port types, and
each display kind reaches the Card's record. They run as processes of the
worker, so a failure of the script is a failure of the Card. With
TSUGUMI_TEST_CONTAINER=1 they run in the shared VM's sandbox."""

from __future__ import annotations

import json
import os

import pytest

from conftest import Daemon, real_environment
from projects import put, run_and_wait
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState

NUMBERS = """attribute "tsugumi";
namespace t;
table Numbers { values:[double]; label:string; big:ulong; }
table Outputs { numbers:Numbers (required); }
root_type Outputs;
"""
REPORT = """attribute "tsugumi";
namespace t;
table Report { value:string; }
table Outputs { report:Report (required, tsugumi: "file"); }
root_type Outputs;
"""
IN_VM = os.environ.get("TSUGUMI_TEST_CONTAINER") == "1"
NO_OUTPUTS = "namespace t;\ntable Outputs { }\nroot_type Outputs;\n"


def write_project(root, cards: dict, connections: list):
    """cards: {id: {"card": _card.json, "nix": default.nix, "outputs": fbs, "src": {name: text}}}."""
    project = {"name": "languages", "pkgsPath": "tsugumi:default", "databaseConnections": [], "canvas": {
        "cards": [{"id": "start"}] + [{"id": i} for i in cards],
        "connections": [{"from": {"card": a, "port": p}, "to": {"card": b, "port": q}} for a, p, b, q in connections]}}
    put(root / "tsugumi.json", json.dumps(project))
    put(root / "layout.json", "{}")
    put(root / "definitions/start/_card.json", json.dumps({"template": "trigger"}))
    for id_, c in cards.items():
        d = root / "definitions" / id_
        put(d / "_card.json", json.dumps({**c["card"], **({} if IN_VM else {"sandbox": False})}))
        put(d / "default.nix", c["nix"])
        put(d / "_outputs.fbs", c.get("outputs", NO_OUTPUTS))
        for name, source in c.get("src", {}).items():
            put(d / "src" / name, source)


R_NIX = "{ pkgs, inputs, params }: { run = inputs.tsugumi.rCard { src = ./src; }; }\n"
JULIA_NIX = "{ pkgs, inputs, params }: { run = inputs.tsugumi.juliaCard { src = ./src; }; }\n"

R_MAKE = r"""
main <- function(ctx) {
  stopifnot(ctx$params$count == 3)
  values <- seq_len(ctx$params$count) * ctx$params$scale
  ctx$output("numbers", list(values = values, label = "三つの値 \"quoted\"", big = ctx$params$big))
  ctx$display_table(data.frame(i = seq_along(values), value = values))
}
"""

JULIA_DOUBLE = r"""
function main(ctx)
    m = input(ctx, "numbers")
    @assert m["label"] == "三つの値 \"quoted\""
    @assert m["big"] == 9007199254740993
    values = m["values"]
    doubled = 2 .* values
    @assert doubled == [3.0, 6.0, 9.0]
    output(ctx, "numbers", Dict("values" => doubled, "label" => m["label"], "big" => m["big"]))
    display_vegalite(ctx, Dict(
        "\$schema" => "https://vega.github.io/schema/vega-lite/v6.json",
        "data" => Dict("values" => [Dict("x" => i, "y" => v) for (i, v) in enumerate(doubled)]),
        "mark" => "line",
        "encoding" => Dict("x" => Dict("field" => "x", "type" => "quantitative"),
                           "y" => Dict("field" => "y", "type" => "quantitative"))))
end
"""

R_REPORT = r"""
main <- function(ctx) {
  m <- ctx$input("numbers")
  stopifnot(identical(m$values, c(3, 6, 9)), identical(m$big, "9007199254740993"))
  file <- file.path(ctx$output_directory, "report.txt")
  writeLines(paste("sum", sum(m$values)), file)
  ctx$output("report", list(value = file))
  ctx$plot(plot(m$values, main = "三つの値"), alt = "the values")
}
"""


EXACT = """attribute "tsugumi";
namespace t;
table Exact { values:[double]; single:float; }
table Outputs { numbers:Exact (required); }
root_type Outputs;
"""
# More digits than 12 decimals keep, below them, the limits of a double, and a sum that shows every bit.
EXACT_VALUES = [0.042169650342858, 1e-20, 5e-324, 1.7976931348623157e308, 0.1 + 0.2, 123456.78901234567, 3.0]
EXACT_JSON = json.dumps({"numbers": {"values": EXACT_VALUES, "single": 0.1}})

BASH_MAKE = "{ pkgs, inputs, params }: { run = ''\n  cat > \"$TSUGUMI_OUTPUT\" <<'END'\n" + EXACT_JSON + "\nEND\n''; }\n"
# R reads its own literals and as.numeric less exactly than a double on some machines, so it compares what C prints.
R_EXACT = r"""
main <- function(ctx) {
  m <- ctx$input("numbers")
  stopifnot(identical(sprintf("%.17g", m$values), c(""" + ", ".join(f'"{v:.17g}"' for v in EXACT_VALUES) + r""")))
  ctx$output("numbers", m)
}
"""
JULIA_EXACT = r"""
function main(ctx)
    m = input(ctx, "numbers")
    @assert m["values"] == [""" + ", ".join(repr(v) for v in EXACT_VALUES) + r"""]
    @assert m["single"] == 0.1
    output(ctx, "numbers", m)
end
"""
BASH_SHOW = "{ pkgs, inputs, params }: { run = ''\n  jq -n --rawfile m \"$JSON_NUMBERS\" '{text: $m}' > \"$TSUGUMI_DISPLAY\"\n''; }\n"


@pytest.fixture
def project(tmp_path):
    """The project's folder, and a function that opens it after the test wrote it."""
    root = tmp_path / "languages"
    opened = []

    def connect():
        env = real_environment(root, "tsugumi-test-languages") if IN_VM else Daemon(root).start()
        opened.append(env)
        c = env.client()
        if not IN_VM:
            c.hello()
        return c

    yield root, connect
    for env in opened:
        env.cleanup()


def test_values_cross_r_and_julia_and_each_display_arrives(project):
    root, connect = project
    write_project(root, {
        "make": {"card": {"language": "r", "inputs": [{"name": "start"}], "params": [
            {"name": "count", "type": "uint64", "data": 3},
            {"name": "scale", "type": "double", "data": 1.5},
            {"name": "big", "type": "uint64", "data": 9007199254740993}]},
                 "nix": R_NIX, "outputs": NUMBERS, "src": {"main.R": R_MAKE}},
        "double": {"card": {"language": "julia", "inputs": [{"name": "numbers"}]},
                   "nix": JULIA_NIX, "outputs": NUMBERS, "src": {"main.jl": JULIA_DOUBLE}},
        "report": {"card": {"language": "r", "inputs": [{"name": "numbers"}], "filesystem": True,
                            "retainArtifact": True, "artifactOutput": "report"},
                   "nix": R_NIX, "outputs": REPORT, "src": {"main.R": R_REPORT}},
    }, [("start", "out", "make", "start"), ("make", "numbers", "double", "numbers"), ("double", "numbers", "report", "numbers")])
    c = connect()
    run_id, status = run_and_wait(c, "start", timeout=1800)
    log = lambda card: text(c.call("LogRangeRequest", runId=run_id, card=path(card), stream=2, limitBytes=65536).text)
    assert status.state == RunState.succeeded, (status.failure and text(status.failure.diagnostic),
                                                log("make"), log("double"), log("report"))
    table = c.call("RunRecordRequest", runId=run_id, card=path("make")).record.display
    assert table.valueType == 4  # DisplayTable
    assert [text(name) for name in table.value.columns] == ["i", "value"]
    assert [text(cell) for cell in table.value.rows[2].cells] == ["3", "4.5"]
    plot = c.call("RunRecordRequest", runId=run_id, card=path("double")).record.display
    assert plot.valueType == 7 and text(plot.value.name) == "vega-lite"  # DisplayModule
    assert json.loads(text(plot.value.dataJson))["data"]["values"][2] == {"x": 3, "y": 9.0}
    image = c.call("RunRecordRequest", runId=run_id, card=path("report")).record.display
    assert image.valueType == 5 and bytes(image.value.data)[:4] == b"\x89PNG" and text(image.value.alt) == "the values"
    art = next(a for a in c.call("ArtifactListRequest", runId=run_id).artifacts if text(a.output) == "report")
    assert bytes(c.call("ArtifactReadRequest", artifactId=text(art.artifactId)).bytes).decode() == "sum 18\n"


def test_a_double_crosses_bash_r_and_julia_unchanged(project):
    root, connect = project
    write_project(root, {
        "make": {"card": {"language": "bash", "inputs": [{"name": "start"}]}, "nix": BASH_MAKE, "outputs": EXACT},
        "r": {"card": {"language": "r", "inputs": [{"name": "numbers"}]},
              "nix": R_NIX, "outputs": EXACT, "src": {"main.R": R_EXACT}},
        "julia": {"card": {"language": "julia", "inputs": [{"name": "numbers"}]},
                  "nix": JULIA_NIX, "outputs": EXACT, "src": {"main.jl": JULIA_EXACT}},
        "show": {"card": {"language": "bash", "inputs": [{"name": "numbers"}]}, "nix": BASH_SHOW},
    }, [("start", "out", "make", "start"), ("make", "numbers", "r", "numbers"),
        ("r", "numbers", "julia", "numbers"), ("julia", "numbers", "show", "numbers")])
    c = connect()
    run_id, status = run_and_wait(c, "start", timeout=1800)
    log = lambda card: text(c.call("LogRangeRequest", runId=run_id, card=path(card), stream=2, limitBytes=65536).text)
    assert status.state == RunState.succeeded, (status.failure and text(status.failure.diagnostic), log("r"), log("julia"))
    raw = text(c.call("RunRecordRequest", runId=run_id, card=path("show")).record.display.value.text)
    assert json.loads(raw) == {"values": EXACT_VALUES, "single": 0.1}
    assert "0.042169650342858," in raw and "1e-20," in raw  # the shortest digits, as a person would read them


@pytest.mark.parametrize("display, check", [
    ('{"markdown": "**bold**"}', lambda d: d.value.markdown and text(d.value.text) == "**bold**"),
    ('{"text": "plain"}', lambda d: not d.value.markdown and text(d.value.text) == "plain"),
])
def test_a_bash_card_shows_what_it_writes(project, display, check):
    root, connect = project
    write_project(root, {"show": {
        "card": {"language": "bash", "inputs": [{"name": "start"}]},
        "nix": "{ pkgs, inputs, params }: { run = ''\n  cat > \"$TSUGUMI_DISPLAY\" <<'END'\n" + display + "\nEND\n''; }\n"}},
        [("start", "out", "show", "start")])
    c = connect()
    run_id, status = run_and_wait(c, "start", timeout=600)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    assert check(c.call("RunRecordRequest", runId=run_id, card=path("show")).record.display)


@pytest.mark.parametrize("display, why", [
    ('{"image": {"path": "missing.png"}}', "cannot read the image"),
    ('{"table": {"columns": ["a"], "rows": [["1", "2"]]}}', "a table row has 2 cells"),
    ("{}", "name one of"),
])
def test_an_invalid_display_fails_the_card_and_says_why(project, display, why):
    root, connect = project
    write_project(root, {"show": {
        "card": {"language": "bash", "inputs": [{"name": "start"}]},
        "nix": "{ pkgs, inputs, params }: { run = ''\n  cat > \"$TSUGUMI_DISPLAY\" <<'END'\n" + display + "\nEND\n''; }\n"}},
        [("start", "out", "show", "start")])
    c = connect()
    _, status = run_and_wait(c, "start", timeout=600)
    assert status.state == RunState.failed and why in text(status.failure.diagnostic)


def test_an_r_error_fails_the_card(project):
    root, connect = project
    write_project(root, {"boom": {
        "card": {"language": "r", "inputs": [{"name": "start"}]}, "nix": R_NIX,
        "src": {"main.R": 'main <- function(ctx) stop("on purpose")\n'}}}, [("start", "out", "boom", "start")])
    c = connect()
    run_id, status = run_and_wait(c, "start", timeout=1800)
    assert status.state == RunState.failed
    assert "on purpose" in text(c.call("LogRangeRequest", runId=run_id, card=path("boom"), stream=2, limitBytes=65536).text)
