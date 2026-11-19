"""Typed values reach the bundled Bash display Card through the real worker and public RPC."""

import json
import math
import re
from pathlib import Path
import shlex

import pytest

from harness.real_cards import PYTHON_NIX, nix_string
from harness.scenario import Project, run, wait_run
from harness.values import encode_message, reference_json
from tsugumi_client import path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.RunState import RunState


DEFINITION = Path(__file__).resolve().parents[2] / "demos/en/marimo/definitions/result"


def display_graph(project, schema, source_script=None, *, payload=None, display_format="text", markdown_field="value"):
    graph = Project(project)
    if payload is None:
        graph.card("source", {"language": "bash"}, outputs=schema, sources={
            "default.nix": "{ pkgs, inputs, params }: { run = " + nix_string(source_script) + "; }",
        })
    else:
        # A binary producer lets non-finite values bypass Bash output JSON parsing.
        graph.card("source", {"language": "python", "params": [
            {"name": "payload", "type": "string", "data": payload.hex()},
        ]}, outputs=schema, sources={
            "default.nix": PYTHON_NIX % "",
            "src/main.py": """from tsugumi import contract

def main(inputs, report):
    return {"out": bytes.fromhex(contract.params_object(inputs).payload)}
""",
        })
    definition = json.loads((DEFINITION / "_card.json").read_text())
    for param in definition["params"]:
        param["data"] = {"format": display_format, "markdown_field": markdown_field}[param["name"]]
    graph.card("display", definition, outputs=(DEFINITION / "_outputs.fbs").read_text(),
               sources={"default.nix": (DEFINITION / "default.nix").read_text()})
    return graph.connect("source", "out", "display", "in")


def display_run(client):
    run_id = run(client, "source")
    status = wait_run(client, run_id)
    stdout = text(client.call("LogRangeRequest", runId=run_id, card=path("display"),
                              invocation=1, stream=1).text)
    return run_id, status, stdout


def recorded_display(client, run_id):
    record = client.call("RunRecordRequest", runId=run_id, card=path("display"))
    return record.record.display.value


def emit(value):
    return 'printf "%s" ' + shlex.quote('{"out":' + value + '}') + ' > "$TSUGUMI_OUTPUT"'


TEXT_SCHEMA = "table Message { note:string; value:string; other:double; } table Outputs { out:Message; } root_type Outputs;"


def test_display_renders_the_selected_markdown_field_on_each_run(client, project, tmp_path):
    markdown = "**日本語**\n\n$x^2$"
    encoded = '{"note":' + json.dumps(markdown) + ',"value":"other","other":inf}'
    payload = encode_message(tmp_path / "encoded", TEXT_SCHEMA, encoded, root_type="Message")
    display_graph(project, TEXT_SCHEMA, payload=payload, display_format="markdown", markdown_field="note").save(client)
    for cached in (False, True):
        run_id, status, logged = display_run(client)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        if cached:
            record = client.call("RunInvocationsRequest", runId=run_id, card=path("display")).records[0]
            assert record.buildCached
        shown = recorded_display(client, run_id)
        assert shown.markdown
        assert text(shown.text) == markdown
        values = json.loads(logged.replace('"other": inf', '"other": Infinity'))
        assert values["note"] == markdown and values["other"] == math.inf


@pytest.mark.parametrize("field", ["note", "other"], ids=["missing", "not-a-string"])
def test_display_rejects_a_missing_or_non_string_markdown_field(client, project, field):
    display_graph(project, TEXT_SCHEMA, emit('{"value":"other"}'),
                  display_format="markdown", markdown_field=field).save(client)
    run_id, status, logged = display_run(client)
    assert status.state == RunState.failed
    assert json.loads(logged)["value"] == "other"
    reply = client.call("RunRecordRequest", runId=run_id, card=path("display"))
    assert reply.status.state == CardState.failed
    assert reply.record.display is None


def test_display_marks_large_unicode_previews(client, project):
    script = 'jq -n \'{out:{value:("\\uD83D\\uDE00" * 270000)}}\' > "$TSUGUMI_OUTPUT"'
    display_graph(project, TEXT_SCHEMA, script).save(client)
    run_id, status, logged = display_run(client)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    shown = text(recorded_display(client, run_id).text)
    prefix, separator, notice = shown.rpartition("\n\n")
    assert prefix and separator and notice
    assert logged and prefix[:100] == logged[:100]
    assert len(shown.encode()) <= 1024 * 1024
    assert "\ufffd" not in shown


STRUCTURED_SCHEMA = """
namespace t;
enum Color : byte { Red, Green = 3 }
enum Wide : ulong { Low, High = 9223372036854775807 }
struct Inner { a:[int:3]; c:Color; }
struct Point { x:double; y:float; ok:bool; inner:Inner; many:[Inner:2]; }
table Leaf { name:string; v:long = -7; }
table Nested { n:int; s:string; }
union Either { Leaf, Nested }
table Message {
  flag:bool; on:bool = true; i8:byte = -5; u8:ubyte = 200; i16:short; u16:ushort = 65535; i32:int; u32:uint;
  i64:long = -9000000000000000000; u64:ulong = 18446744073709551615; f:float = 1.5; d:double = 2.25; zero:double;
  absent:int = null; set:int = null; absent_d:double = null; set_d:double = null;
  color:Color = Green; other:Color; unnamed:Color; wide:Wide; dep:int (deprecated);
  text:string; empty_text:string; point:Point; leaf:Leaf; default_leaf:Leaf; missing:Leaf;
  either:Either; none:Either;
  doubles:[double]; floats:[float]; finite_doubles:[double]; finite_floats:[float]; non_finite:[double]; bytes:[ubyte]; bools:[bool]; colors:[Color]; texts:[string];
  leaves:[Leaf]; points:[Point]; nothing:[int]; no_tables:[Leaf];
}
table Outputs { out:Message; }
root_type Outputs;
"""

STRUCTURED_VALUE = {
    "flag": True, "on": False, "i8": -128, "u32": 4294967295, "d": -2.5, "f": 0.25, "set": 0, "set_d": 0.5,
    "other": "Red", "unnamed": 7, "wide": "High",
    "text": '日本語 "quoted" \\ \n\t \u0001 /', "empty_text": "",
    "point": {"x": 0.5, "y": -0.125, "ok": True, "inner": {"a": [1, 2, 3], "c": "Green"},
              "many": [{"a": [4, 5, 6], "c": "Red"}, {"a": [7, 8, 9], "c": 7}]},
    "leaf": {"name": "葉", "v": 1}, "default_leaf": {},
    "either_type": "Nested", "either": {"n": 1, "s": "x"},
    "doubles": [0.5, 1.0, -1000000.0, 0.001, -0.0], "floats": [0.5, 1.0, -0.125],
    "bytes": [0, 255], "bools": [True, False], "colors": ["Green", "Red", 9],
    "texts": ["x", "", "語"],
    "leaves": [{"name": "a"}, {}], "points": [], "nothing": [], "no_tables": [],
}


FINITE_DOUBLES = [0.30000000000000004, 123456.78901234567, -1.5e-7, 5e-324, 1.7976931348623157e308, 1e16, 9999999999999998, 0.0001, 3]
FINITE_FLOATS = [0.1, 16777216, 1.17549435e-38, 3.4028235e38, 1e-7]


def test_display_preserves_structured_values_and_defaults(client, project, tmp_path):
    """Typed values survive worker display and an independent FlatBuffers round trip."""
    value = dict(STRUCTURED_VALUE, finite_doubles=FINITE_DOUBLES, finite_floats=FINITE_FLOATS)
    finite_payload = encode_message(tmp_path / "finite", STRUCTURED_SCHEMA, value, root_type="t.Message")
    encoded = json.dumps(value, ensure_ascii=False)[:-1] + ', "non_finite": [nan, inf, -inf]}'
    payload = encode_message(tmp_path / "encoded", STRUCTURED_SCHEMA, encoded, root_type="t.Message")
    display_graph(project, STRUCTURED_SCHEMA, payload=payload).save(client)
    run_id, status, logged = display_run(client)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    shown = text(recorded_display(client, run_id).text)
    assert logged == shown
    values = re.sub(r"\b(?:nan|inf)\b", lambda token: {"nan": "NaN", "inf": "Infinity"}[token[0]], shown)
    actual = json.loads(values, strict=False)
    nan, positive, negative = actual.pop("non_finite")
    assert math.isnan(nan) and positive == math.inf and negative == -math.inf
    expected = reference_json(tmp_path / "reference", STRUCTURED_SCHEMA, finite_payload, root_type="t.Message")
    # The resolved schema presents the deprecated source field as a scalar.
    assert actual.pop("dep") == 0
    # flatc's JSON printer and the display can round differently. Re-encode
    # only these vectors and compare their binary values at the schema widths.
    vectors = ("finite_doubles", "finite_floats")
    floats = {key: actual.pop(key) for key in vectors}
    for key in vectors:
        expected.pop(key)
    assert actual == expected
    encode = lambda folder, value: encode_message(tmp_path / folder, STRUCTURED_SCHEMA, value, root_type="t.Message")
    assert encode("read-back", floats) == encode("boundaries", {key: value[key] for key in vectors})
