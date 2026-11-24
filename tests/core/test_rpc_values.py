"""Structured values round-trip through the ABI, execution and public value RPC."""
import json
import math

from harness.fixtures import mock_plan
from harness.native_card import native_card
from harness.observations import read_output
from harness.scenario import Project, run, wait_run
from harness.values import encode_message, reference_json
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState


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


def test_structured_values_survive_a_native_call_and_history(project, tmp_path, core_factory):
    """The real worker and saved value transport preserve the original bytes."""
    value = dict(STRUCTURED_VALUE, finite_doubles=FINITE_DOUBLES, finite_floats=FINITE_FLOATS)
    encoded = json.dumps(value, ensure_ascii=False)[:-1] + ', "non_finite": [nan, inf, -inf]}'
    payload = encode_message(tmp_path / "encoded", STRUCTURED_SCHEMA, encoded, root_type="t.Message")
    package = native_card(tmp_path / "structured-card", """
int tsugumi_main(const tsugumi_call *call, tsugumi_bytes *result) {
    if (call->abi_version != TSUGUMI_ABI_VERSION) return 1;
    *result = (tsugumi_bytes){result_bytes, sizeof(result_bytes)};
    return 0;
}
""", [("out", payload)])
    client = core_factory(mocked_builds=True).connect()
    Project(project).card("work", {"type": "code", "recordIO": "on", "sandbox": {
        "filesystem": False, "pid": False, "ipc": False, "identity": False,
    }}, outputs=STRUCTURED_SCHEMA, plan=mock_plan([package])).save(client)
    run_id = run(client)
    status = wait_run(client, run_id)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    assert read_output(client, run_id) == payload
    record = client.call("RunRecordRequest", runId=run_id, card=path("work")).record
    output, = record.outputs
    assert text(output.rootType) == "t.Message"
    assert bytes(output.inlineBytes) == payload
    # An independent FlatBuffers encoder provides the expected finite values.
    finite = encode_message(tmp_path / "finite", STRUCTURED_SCHEMA, value, root_type="t.Message")
    expected = reference_json(tmp_path / "expected", STRUCTURED_SCHEMA, finite, root_type="t.Message")
    actual = reference_json(tmp_path / "actual", bytes(output.schemaBfbs), payload, root_type="t.Message", non_finite=True)
    nan, positive, negative = actual.pop("non_finite")
    assert math.isnan(nan) and positive == math.inf and negative == -math.inf
    assert actual.pop("dep", 0) == 0
    expected.pop("non_finite", None)
    assert actual == expected
