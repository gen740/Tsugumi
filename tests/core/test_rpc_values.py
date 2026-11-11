"""Typed values reach the real logger through an external worker and public RPC."""

import json

import pytest

from harness.fixtures import add_worker
from harness.scenario import Project, run, wait_run
from harness.values import encode_message, raw_output, reference_json
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState


FLOAT_SCHEMA = "table M { d:double; f:float; } table Outputs { out:M; } root_type Outputs;"


@pytest.fixture
def shown_value(core_factory, project, tmp_path):
    def show(schema, value, root_type):
        payload = encode_message(tmp_path / "encoded", schema, value, root_type=root_type)
        client = core_factory(mocked_builds=True, mocked_worker=True).connect()
        graph = Project(project)
        add_worker(graph, "source", tmp_path / "package", [raw_output(payload)], outputs=schema)
        graph.card("logger", {"template": "logger"}).connect("source", "out", "logger", "in").save(client)
        run_id = run(client, "source")
        status = wait_run(client, run_id)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        logged = text(client.call("LogRangeRequest", runId=run_id, card=path("logger"),
                                  invocation=1, stream=1).text)
        record = client.call("RunRecordRequest", runId=run_id, card=path("logger"))
        shown = text(record.record.display.value.text)
        assert logged == shown + "\n"
        assert "\n" not in shown
        return shown, payload
    return show


@pytest.mark.parametrize("field,literal,expected", [
    ("d", "0.30000000000000004", "0.30000000000000004"),
    ("d", "123456.78901234567", "123456.78901234567"),
    ("d", "-1.5e-7", "-1.5e-07"),
    ("d", "5e-324", "5e-324"),
    ("d", "1.7976931348623157e308", "1.7976931348623157e+308"),
    ("d", "1e16", "1e+16"),
    ("d", "9999999999999998", "9999999999999998.0"),
    ("d", "0.0001", "0.0001"),
    ("d", "3", "3.0"),
    ("f", "0.1", "0.1"),
    ("f", "16777216", "16777216.0"),
    ("f", "1.17549435e-38", "1.1754944e-38"),
    ("f", "3.4028235e38", "3.4028235e+38"),
    ("f", "1e-7", "1e-07"),
], ids=["double-rounding-tail", "double-many-digits", "double-negative-exponent", "double-min-subnormal",
        "double-max-finite", "double-large-exponent", "double-large-integer", "double-small-decimal",
        "double-whole-number", "float-decimal", "float-max-exact-integer", "float-min-normal",
        "float-max-finite", "float-small-exponent"])
def test_logger_preserves_floating_point_values(shown_value, tmp_path, field, literal, expected):
    """The logger prints floating-point boundaries without losing their original value.

    Given an independently encoded float or double,
    when the external worker sends it through the real logger,
    then its displayed decimal reconstructs exactly the same message bytes.
    """
    shown, original = shown_value(FLOAT_SCHEMA, '{"' + field + '":' + literal + '}', "M")
    expected_fields = {"d": "0.0", "f": "0.0", field: expected}
    assert shown == '{"d": ' + expected_fields["d"] + ',"f": ' + expected_fields["f"] + '}'
    assert encode_message(tmp_path / "read-back", FLOAT_SCHEMA, shown, root_type="M") == original


@pytest.mark.parametrize("literal", ["nan", "inf", "-inf"], ids=["nan", "positive-infinity", "negative-infinity"])
def test_logger_represents_non_finite_message_values(shown_value, literal):
    """The logger preserves non-finite values carried by a typed binary message."""
    shown, _ = shown_value(FLOAT_SCHEMA, '{"d":' + literal + '}', "M")
    assert shown == '{"d": ' + literal + ',"f": 0.0}'


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
  doubles:[double]; floats:[float]; bytes:[ubyte]; bools:[bool]; colors:[Color]; texts:[string];
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


def test_logger_preserves_structured_values_and_defaults(shown_value, tmp_path):
    """Enums, structs, arrays, unions, Unicode and default values survive logging."""
    shown, payload = shown_value(STRUCTURED_SCHEMA, STRUCTURED_VALUE, "t.Message")
    expected = reference_json(tmp_path / "reference", STRUCTURED_SCHEMA, payload, root_type="t.Message")
    actual = json.loads(shown, strict=False)
    assert actual == expected
    assert actual["u64"] == 18446744073709551615
    assert actual["i64"] == -9000000000000000000
    assert actual["default_leaf"]["v"] == -7
    assert actual["point"]["many"][1]["a"] == [7, 8, 9]


@pytest.mark.xfail(strict=True, raises=AssertionError,
                   reason="Output schema conversion turns a uint64-max enum member into -1")
def test_logger_accepts_unsigned_enum_members_above_int64_max(shown_value):
    """An unsigned 64-bit enum keeps its largest member through a real run."""
    schema = """
    enum Wide : ulong { Low, High = 18446744073709551615 }
    table Message { value:Wide; }
    table Outputs { out:Message; }
    root_type Outputs;
    """
    shown, _ = shown_value(schema, {"value": "High"}, "Message")
    assert json.loads(shown) == {"value": "High"}


@pytest.mark.xfail(strict=True, raises=AssertionError,
                   reason="Output schema conversion drops bit_flags and rejects its zero default")
def test_logger_accepts_combined_bit_flags(shown_value, tmp_path):
    """A bit-flags enum preserves combinations, zero and unknown bits through a real run."""
    schema = """
    enum Flags : ubyte (bit_flags) { A, B, C }
    table Message { value:Flags; none:Flags; unknown:Flags; many:[Flags]; }
    table Outputs { out:Message; }
    root_type Outputs;
    """
    shown, payload = shown_value(schema, {"value": "A C", "none": 0, "unknown": 128,
                                         "many": ["B", "A B", 0]}, "Message")
    assert json.loads(shown) == reference_json(tmp_path / "reference", schema, payload, root_type="Message")


@pytest.mark.xfail(strict=True, raises=AssertionError,
                   reason="Output schema conversion duplicates a union vector's type discriminator")
def test_logger_accepts_vectors_of_union_values(shown_value, tmp_path):
    """A vector of different union members keeps every type and value through a real run."""
    schema = """
    table Leaf { name:string; value:int = -7; }
    table Nested { n:int; }
    union Either { Leaf, Nested }
    table Message { values:[Either]; }
    table Outputs { out:Message; }
    root_type Outputs;
    """
    shown, payload = shown_value(schema, {"values_type": ["Leaf", "Nested"],
                                         "values": [{"name": "a"}, {"n": 2}]}, "Message")
    assert json.loads(shown) == reference_json(tmp_path / "reference", schema, payload, root_type="Message")
