"""R and Julia Cards (rCard, juliaCard) and the display of bash Cards: params,
messages and File outputs cross between languages through the port types, and
each display kind reaches the Card's record. Their interpreters live in the
ABI worker; exceptions fail the Card. A Card keeps no interpreter state unless
its settings say `reuseWorker: true`: by default every invocation starts a new
interpreter. Linux CI runs them under bubblewrap, as in the shared VM.
TSUGUMI_TEST_CONTAINER=1 also exercises the app's VM on macOS."""

from __future__ import annotations

import json
import os
import sys

import pytest

from conftest import Daemon, real_environment
from projects import put, run_and_wait
from tsugumi_client import new_id, path, text
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
    project = {"name": "languages", "pkgsPath": "tsugumi:default", "canvas": {
        "cards": [{"id": "start"}] + [{"id": i} for i in cards],
        "connections": [{"from": {"card": a, "port": p}, "to": {"card": b, "port": q}} for a, p, b, q in connections]}}
    put(root / "tsugumi.json", json.dumps(project))
    put(root / "layout.json", "{}")
    put(root / "definitions/start/_card.json", json.dumps({"template": "trigger"}))
    for id_, c in cards.items():
        d = root / "definitions" / id_
        settings = c["card"].copy()
        if not IN_VM and sys.platform != "linux":
            settings["sandbox"] = False
        put(d / "_card.json", json.dumps(settings))
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


@pytest.mark.parametrize("language", ["r", "julia"])
def test_an_interpreter_error_fails_the_card(project, language):
    root, connect = project
    nix, name, source = (R_NIX, "main.R", 'main <- function(ctx) stop("on purpose")\n') if language == "r" else (JULIA_NIX, "main.jl", 'function main(ctx)\n error("on purpose")\nend')
    write_project(root, {"boom": {
        "card": {"language": language, "inputs": [{"name": "start"}]}, "nix": nix,
        "src": {name: source}}}, [("start", "out", "boom", "start")])
    c = connect()
    run_id, status = run_and_wait(c, "start", timeout=1800)
    assert status.state == RunState.failed
    assert "on purpose" in text(c.call("LogRangeRequest", runId=run_id, card=path("boom"), stream=2, limitBytes=65536).text)


LIFECYCLE = {
    "r": (R_NIX, "main.R", r'''
runs <- 0L
calls <- 0L
on_run_start <- function(ctx) { runs <<- runs + 1L }
main <- function(ctx) {
  calls <<- calls + 1L
  stopifnot(is.null(ctx$input("absent")), !ctx$cancelled(), dir.exists(tempdir()))
  ctx$display_text(sprintf("%d %d", runs, calls))
}
'''),
    "julia": (JULIA_NIX, "main.jl", r'''
using Statistics, LinearAlgebra
runs = 0
calls = 0
function on_run_start(ctx)
    global runs += 1
end
function main(ctx)
    global calls += 1
    @assert input(ctx, "absent") === nothing && !cancelled(ctx)
    @assert mean([1, 2]) == 1.5 && norm([3, 4]) == 5
    display_text(ctx, "$runs $calls")
end
'''),
}


@pytest.mark.parametrize("language", ["r", "julia"])
@pytest.mark.parametrize("reuse", [True, False])
def test_interpreter_state_and_run_hook(project, language, reuse):
    root, connect = project
    nix, name, source = LIFECYCLE[language]
    write_project(root, {"count": {"card": {"language": language, "reuseWorker": reuse},
                                  "nix": nix, "src": {name: source}}}, [])
    c = connect()
    for n in (1, 2):
        run_id, status = run_and_wait(c, "start", timeout=1800)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        shown = c.call("RunRecordRequest", runId=run_id, card=path("count")).record.display
        expected = n if reuse else 1
        assert text(shown.value.text) == f"{expected} {expected}"
        inv = c.call("RunInvocationsRequest", runId=run_id, card=path("count")).records[0]
        assert bool(inv.workerReused) == (reuse and n == 2)


# What an interpreter could carry from one invocation to the next: a global,
# an option, the position of the random number generator (seeded while the
# source loads), an environment variable and the working directory.
CARRIED = {
    "r": (R_NIX, "main.R", r'''
set.seed(1)
calls <- 0L
main <- function(ctx) {
  calls <<- calls + 1L
  seen <- getOption("tsugumi.carried", 0L)
  options(tsugumi.carried = seen + 1L)
  env <- Sys.getenv("TSUGUMI_CARRIED")
  Sys.setenv(TSUGUMI_CARRIED = "set")
  moved <- identical(basename(getwd()), "elsewhere")
  dir.create("elsewhere", showWarnings = FALSE)
  setwd("elsewhere")
  ctx$display_text(sprintf("calls=%d options=%d env=%s moved=%s random=%.9f",
                           calls, seen, env, tolower(moved), runif(1)))
}
'''),
    "julia": (JULIA_NIX, "main.jl", r'''
using Random
Random.seed!(1)
calls = 0
carried = Dict{String,Int}()
function main(ctx)
    global calls += 1
    seen = get(carried, "n", 0)
    carried["n"] = seen + 1
    env = get(ENV, "TSUGUMI_CARRIED", "")
    ENV["TSUGUMI_CARRIED"] = "set"
    moved = basename(pwd()) == "elsewhere"
    mkpath("elsewhere")
    cd("elsewhere")
    display_text(ctx, "calls=$calls options=$seen env=$env moved=$moved random=$(rand())")
end
'''),
}


def shown_text(c, run_id, card):
    return text(c.call("RunRecordRequest", runId=run_id, card=path(card)).record.display.value.text)


def reused(c, run_id, card):
    return bool(c.call("RunInvocationsRequest", runId=run_id, card=path(card)).records[0].workerReused)


def reload(c):
    snap = c.call("ProjectInspectRequest")
    c.call("ProjectReloadRequest", baseRev=snap.rev, opId=new_id(), origin="test")


@pytest.mark.parametrize("language", ["r", "julia"])
def test_a_card_keeps_no_interpreter_state_by_default(project, language):
    # No reuseWorker in the settings: the second run must not be able to tell
    # that there was a first one.
    root, connect = project
    nix, name, source = CARRIED[language]
    write_project(root, {"probe": {"card": {"language": language}, "nix": nix, "src": {name: source}}}, [])
    c = connect()
    seen = []
    for _ in range(2):
        run_id, status = run_and_wait(c, "start", timeout=1800)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        seen.append(shown_text(c, run_id, "probe"))
        assert not reused(c, run_id, "probe")
    assert seen[0].startswith("calls=1 options=0 env= moved=false random="), seen
    assert seen[0] == seen[1], seen


@pytest.mark.parametrize("language", ["r", "julia"])
def test_reuse_worker_carries_globals_options_and_the_random_position(project, language):
    # What carries with reuseWorker: true is pinned here so that a change is
    # deliberate. The worker resets the environment and the working directory
    # for every call, so those two never carry.
    root, connect = project
    nix, name, source = CARRIED[language]
    write_project(root, {"probe": {"card": {"language": language, "reuseWorker": True},
                                  "nix": nix, "src": {name: source}}}, [])
    c = connect()
    seen = []
    for n in (1, 2):
        run_id, status = run_and_wait(c, "start", timeout=1800)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        seen.append(shown_text(c, run_id, "probe"))
        assert reused(c, run_id, "probe") == (n == 2)
    first, second = (dict(item.split("=", 1) for item in shown.split(" ")) for shown in seen)
    assert (first["calls"], first["options"], first["env"], first["moved"]) == ("1", "0", "", "false"), seen
    assert (second["calls"], second["options"], second["env"], second["moved"]) == ("2", "1", "", "false"), seen
    assert first["random"] != second["random"], seen


VERSIONED = {
    "r": (R_NIX, "main.R", 'calls <- 0L\nmain <- function(ctx) {\n  calls <<- calls + 1L\n'
                           '  ctx$display_text(sprintf("VERSION %d", calls))\n}\n'),
    "julia": (JULIA_NIX, "main.jl", 'calls = 0\nfunction main(ctx)\n    global calls += 1\n'
                                    '    display_text(ctx, "VERSION $calls")\nend\n'),
}


@pytest.mark.parametrize("language", ["r", "julia"])
def test_disabling_reuse_drops_the_previous_interpreter(project, language):
    root, connect = project
    nix, name, source = VERSIONED[language]
    settings = {"language": language, "reuseWorker": True}
    write_project(root, {"count": {"card": settings, "nix": nix,
                                  "src": {name: source.replace("VERSION", "calls")}}}, [])
    c = connect()

    def expect(calls, again):
        run_id, status = run_and_wait(c, "start", timeout=1800)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        assert shown_text(c, run_id, "count") == f"calls {calls}"
        assert reused(c, run_id, "count") == again

    expect(1, False)
    expect(2, True)
    settings.pop("reuseWorker")
    put(root / "definitions/count/_card.json", json.dumps(settings))
    reload(c)
    expect(1, False)


@pytest.mark.parametrize("language", ["r", "julia"])
def test_a_changed_source_replaces_the_resident_interpreter(project, language):
    root, connect = project
    nix, name, source = VERSIONED[language]
    write_project(root, {"count": {"card": {"language": language, "reuseWorker": True},
                                  "nix": nix, "src": {name: source.replace("VERSION", "first")}}}, [])
    c = connect()
    for expected, again in (("first 1", False), ("first 2", True)):
        run_id, status = run_and_wait(c, "start", timeout=1800)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        assert shown_text(c, run_id, "count") == expected and reused(c, run_id, "count") == again
    put(root / "definitions" / "count" / "src" / name, source.replace("VERSION", "second"))
    reload(c)
    run_id, status = run_and_wait(c, "start", timeout=1800)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    # The new source runs in a new interpreter: nothing of the old one is left.
    assert shown_text(c, run_id, "count") == "second 1" and not reused(c, run_id, "count")


def test_changed_r_packages_replace_the_resident_interpreter(project):
    root, connect = project
    _, name, source = VERSIONED["r"]
    write_project(root, {"count": {"card": {"language": "r", "reuseWorker": True}, "nix": R_NIX,
                                  "src": {name: source.replace("VERSION", "calls")}}}, [])
    c = connect()
    for expected in ("calls 1", "calls 2"):
        run_id, status = run_and_wait(c, "start", timeout=1800)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        assert shown_text(c, run_id, "count") == expected
    put(root / "definitions" / "count" / "default.nix",
        "{ pkgs, inputs, params }: { run = inputs.tsugumi.rCard { src = ./src; packages = ps: [ ps.digest ]; }; }\n")
    reload(c)
    run_id, status = run_and_wait(c, "start", timeout=1800)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    assert shown_text(c, run_id, "count") == "calls 1" and not reused(c, run_id, "count")


# The second call of a resident interpreter ends badly in one of four ways.
BAD_SECOND_CALL = {
    "r": (R_NIX, "main.R", r'''
calls <- 0L
main <- function(ctx) {
  calls <<- calls + 1L
  ctx$display_text(sprintf("call %d", calls))
  if (calls != 2L) return(invisible(NULL))
  how <- "HOW"
  if (how == "fail") stop("on purpose")
  if (how == "exit") quit(save = "no", status = 3)
  if (how == "crash") tools::pskill(Sys.getpid(), tools::SIGKILL)
  while (!ctx$cancelled()) Sys.sleep(0.01)
}
'''),
    "julia": (JULIA_NIX, "main.jl", r'''
calls = 0
function main(ctx)
    global calls += 1
    display_text(ctx, "call $calls")
    calls == 2 || return
    how = "HOW"
    how == "fail" && error("on purpose")
    how == "exit" && exit(3)
    how == "crash" && ccall(:kill, Cint, (Cint, Cint), getpid(), 9)
    while !cancelled(ctx)
        sleep(0.01)
    end
end
'''),
}


@pytest.mark.parametrize("language", ["r", "julia"])
@pytest.mark.parametrize("how", ["fail", "cancel", "exit", "crash"])
def test_a_resident_interpreter_is_not_reused_after_a_bad_call(project, language, how):
    # An error, a cancellation, the interpreter leaving by itself (quit/exit)
    # and the process being killed all end the worker: the next run starts a
    # new interpreter, and the daemon goes on serving.
    from projects import start_run, wait_run
    from Tsugumi.RPC.NotificationPayload import NotificationPayload
    root, connect = project
    nix, name, source = BAD_SECOND_CALL[language]
    write_project(root, {"count": {"card": {"language": language, "reuseWorker": True},
                                  "nix": nix, "src": {name: source.replace("HOW", how)}}}, [])
    c = connect()
    run_id, status = run_and_wait(c, "start", timeout=1800)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    assert shown_text(c, run_id, "count") == "call 1"
    if how == "cancel":
        opened = c.open()
        c.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId), cards=[path("count")])
        run_id = start_run(c, "start")
        c.wait_notification(lambda n: n.valueType == NotificationPayload.DisplayUpdated
                            and text(n.value.display.value.text) == "call 2", timeout=600)
        c.call("RunCancelRequest", runId=run_id)
        assert wait_run(c, run_id, 30).state == RunState.cancelled
    else:
        run_id, status = run_and_wait(c, "start", timeout=1800)
        assert status.state == RunState.failed
        failure = c.call("RunRecordRequest", runId=run_id, card=path("count")).status.failure
        assert failure is not None and failure.kind != 14  # not a timeout: the call ended at once
    c.call("ProjectInspectRequest")
    run_id, status = run_and_wait(c, "start", timeout=1800)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    assert shown_text(c, run_id, "count") == "call 1" and not reused(c, run_id, "count")


LIVE = {
    "r": (R_NIX, "main.R", r'''
main <- function(ctx) {
  ctx$display_text("running")
  while (!ctx$cancelled()) Sys.sleep(0.01)
}
'''),
    "julia": (JULIA_NIX, "main.jl", r'''
function main(ctx)
    display_text(ctx, "running")
    while !cancelled(ctx)
        sleep(0.01)
    end
end
'''),
}


@pytest.mark.parametrize("language", ["r", "julia"])
def test_display_arrives_before_completion_and_cancel_stops_worker(project, language):
    import time
    from projects import start_run, wait_run
    root, connect = project
    nix, name, source = LIVE[language]
    write_project(root, {"live": {"card": {"language": language}, "nix": nix, "src": {name: source}}}, [])
    c = connect()
    from Tsugumi.RPC.NotificationPayload import NotificationPayload
    from test_demos import first_display
    opened = c.open()
    c.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId), cards=[path("live")])
    run_id = start_run(c, "start")
    notification = first_display(c, run_id)
    assert notification.valueType == NotificationPayload.DisplayUpdated
    assert text(notification.value.display.value.text) == "running"
    assert c.call("RunStatusRequest", runId=run_id).state not in (RunState.succeeded, RunState.failed)
    started = time.monotonic()
    c.call("RunCancelRequest", runId=run_id)
    assert wait_run(c, run_id, 10).state == RunState.cancelled
    assert time.monotonic() - started < 5
    c.call("ProjectInspectRequest")  # The daemon remains usable after interpreter cancellation.


@pytest.mark.parametrize("language", ["r", "julia"])
def test_interpreter_deadline_ends_uncooperative_code(project, language):
    root, connect = project
    nix, name, _ = LIVE[language]
    source = ('main <- function(ctx) { while (TRUE) {} }' if language == "r" else
              'function main(ctx)\n while true\n end\nend')
    write_project(root, {"slow": {"card": {"language": language, "timeoutSeconds": 2},
                                 "nix": nix, "src": {name: source}}}, [])
    c = connect()
    run_id, status = run_and_wait(c, "start", timeout=1800)
    assert status.state == RunState.failed
    record = c.call("RunRecordRequest", runId=run_id, card=path("slow"))
    assert record.status.failure.kind == 14  # timeout
    c.call("ProjectInspectRequest")


def test_full_integer_ranges_struct_enum_and_union_cross_interpreters(project):
    root, connect = project
    schema = '''namespace t;
    enum Color:byte { red, green }
    struct Bounds { low:long; high:ulong; }
    table Item { value:string; }
    union Choice { Item }
    table Value { signed:long; unsigned:ulong; bounds:Bounds; color:Color; choice:Choice; absent:Item; text:string; }
    table Outputs { value:Value (required); }
    root_type Outputs;
    '''
    payload = {"signed": -9223372036854775808, "unsigned": 18446744073709551615,
               "bounds": {"low": -9007199254740993, "high": 9007199254740993}, "color": "green",
               "choice_type": "Item", "choice": {"value": "18446744073709551615"},
               "text": 'quoted "18446744073709551615" and \\backslash'}
    raw = json.dumps({"value": payload})
    make = "{ pkgs, inputs, params }: { run = ''cat > \"$TSUGUMI_OUTPUT\" <<'JSON'\n" + raw + "\nJSON\n''; }"
    show = "{ pkgs, inputs, params }: { run = ''jq -n --rawfile m \"$JSON_VALUE\" '{text:$m}' > \"$TSUGUMI_DISPLAY\"''; }"
    write_project(root, {
        "make": {"card": {"language": "bash"}, "nix": make, "outputs": schema},
        "r": {"card": {"language": "r", "inputs": [{"name": "value"}]}, "nix": R_NIX, "outputs": schema,
              "src": {"main.R": 'main <- function(ctx) { ctx$output("value", ctx$input("value")) }'}},
        "julia": {"card": {"language": "julia", "inputs": [{"name": "value"}]}, "nix": JULIA_NIX, "outputs": schema,
                  "src": {"main.jl": 'function main(ctx)\n output(ctx, "value", input(ctx, "value"))\nend'}},
        "show": {"card": {"language": "bash", "inputs": [{"name": "value"}]}, "nix": show},
    }, [("make", "value", "r", "value"), ("r", "value", "julia", "value"), ("julia", "value", "show", "value")])
    c = connect()
    run_id, status = run_and_wait(c, "start", timeout=1800)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    shown = c.call("RunRecordRequest", runId=run_id, card=path("show")).record.display
    assert json.loads(text(shown.value.text)) == payload


def test_nix_r_packages_available_in_resident_interpreter(project):
    root, connect = project
    nix = '{ pkgs, inputs, params }: { run = inputs.tsugumi.rCard { src = ./src; packages = ps: [ ps.digest ]; }; }'
    source = 'main <- function(ctx) { ctx$display_text(digest::digest("pinned", algo="sha256")) }'
    write_project(root, {"package": {"card": {"language": "r"}, "nix": nix, "src": {"main.R": source}}}, [])
    c = connect()
    for _ in range(2):
        run_id, status = run_and_wait(c, "start", timeout=1800)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        shown = text(c.call("RunRecordRequest", runId=run_id, card=path("package")).record.display.value.text)
        assert len(shown) == 64


def test_julia_package_resolution_does_not_enable_ifd(project):
    # nixpkgs' existing withPackages resolves a registry through IFD. Embedding
    # Julia must not bypass the evaluator's policy to make that path work.
    root, connect = project
    nix = '{ pkgs, inputs, params }: { run = inputs.tsugumi.juliaCard { src = ./src; packages = [ "JSON" ]; }; }'
    write_project(root, {"package": {"card": {"language": "julia"}, "nix": nix,
                                    "src": {"main.jl": "function main(ctx)\nend"}}}, [])
    c = connect()
    _, status = run_and_wait(c, "start", timeout=1800)
    assert status.state == RunState.failed
    assert "allow-import-from-derivation" in text(status.failure.diagnostic)


@pytest.mark.parametrize("language", ["r", "julia"])
def test_file_and_directory_inputs_refresh_in_resident_interpreters(project, language):
    root, connect = project
    schema = '''attribute "tsugumi"; namespace t; table Path { value:string; }
    table Outputs { file:Path (required, tsugumi:"file"); directory:Path (required, tsugumi:"directory"); }
    root_type Outputs;'''
    make = '''{ pkgs, inputs, params }: { run = ''
      mkdir -p "$TSUGUMI_OUTPUT_DIR/d"
      echo hello > "$TSUGUMI_OUTPUT_DIR/f"
      echo world > "$TSUGUMI_OUTPUT_DIR/d/value"
      jq -n --arg f "$TSUGUMI_OUTPUT_DIR/f" --arg d "$TSUGUMI_OUTPUT_DIR/d" '{file:{value:$f},directory:{value:$d}}' > "$TSUGUMI_OUTPUT"
    ''; }'''
    r = '''previous <- NULL
    main <- function(ctx) {
      file <- ctx$path("file")
      stopifnot(!identical(previous, file), readLines(file) == "hello",
                readLines(file.path(ctx$path("directory"), "value")) == "world")
      previous <<- file
      ctx$display_text("read both")
    }'''
    julia = '''previous = ""
    function main(ctx)
      file = path(ctx, "file")
      @assert previous != file && strip(read(file, String)) == "hello"
      @assert strip(read(joinpath(path(ctx, "directory"), "value"), String)) == "world"
      global previous = file
      display_text(ctx, "read both")
    end'''
    nix, name, source = (R_NIX, "main.R", r) if language == "r" else (JULIA_NIX, "main.jl", julia)
    write_project(root, {
        "make": {"card": {"language": "bash"}, "nix": make, "outputs": schema},
        "read": {"card": {"language": language, "inputs": [{"name": "file"}, {"name": "directory"}]},
                 "nix": nix, "src": {name: source}},
    }, [("make", port, "read", port) for port in ("file", "directory")])
    c = connect()
    for _ in range(2):
        run_id, status = run_and_wait(c, "start", timeout=1800)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        shown = c.call("RunRecordRequest", runId=run_id, card=path("read")).record.display
        assert text(shown.value.text) == "read both"
