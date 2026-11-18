"""Compile and invoke generated C++ role overloads without a worker or socket."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize("arity", [1, 2, 3])
def test_cpp_roles_and_hook_against_the_card_abi(tmp_path, arity):
    flatc = shutil.which(os.environ.get("TSUGUMI_FLATC", "flatc"))
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not flatc or not compiler:
        pytest.skip("C++ compiler and flatc are required")
    include = Path(os.environ.get("TSUGUMI_FLATBUFFERS_INCLUDE", Path(flatc).resolve().parents[1] / "include"))
    if not (include / "flatbuffers/flatbuffers.h").exists():
        pytest.skip("set TSUGUMI_FLATBUFFERS_INCLUDE to the FlatBuffers headers")
    schemas = {
        "_inputs": "namespace inputs.number.sample; table Number { value:ulong; } namespace inputs; table Inputs { number:inputs.number.sample.Number (required); } root_type Inputs;",
        "_outputs": "namespace outputs.result.sample; table Number { value:ulong; } namespace outputs; table Outputs { result:outputs.result.sample.Number; } root_type Outputs;",
        "_params": "namespace params; table Params { factor:ulong = null; } root_type Params;",
    }
    for name, schema in schemas.items():
        path = tmp_path / (name + ".fbs")
        path.write_text(schema)
        subprocess.run([flatc, "--cpp", "--gen-object-api", "--scoped-enums", "-o", str(tmp_path), str(path)], check=True)
    contract = {"inputs": [{"name": "number", "type": "inputs.number.sample.Number", "kind": "flatbuffer", "required": True}],
                "outputs": [{"name": "result", "type": "outputs.result.sample.Number", "kind": "flatbuffer", "required": True}]}
    (tmp_path / "contract.json").write_text(json.dumps(contract))
    subprocess.run([sys.executable, str(ROOT / "sdk/codegen/card_cpp.py"), str(tmp_path / "contract.json"), str(tmp_path)], check=True)
    args = ["const Card::Inputs& inputs", "tsugumi::Report& report", "const tsugumi::Run& run"][:arity]
    extra = ("report.text(\"reported\");" if arity >= 2 else "") + ("assert(run.invocation == 7 && !run.cancelled());" if arity == 3 else "")
    source = r'''
#include <card.hh>
#include <cassert>
#include <type_traits>
extern "C" int tsugumi_card_invoke(const tsugumi_call*, tsugumi_buffer*);
static int hooks = 0, reports = 0;
auto onRunStart(ARGS) -> void { assert(inputs.number.value == 21); ++hooks; EXTRA }
auto run(ARGS) -> Card::Output {
  static_assert(std::is_const_v<std::remove_reference_t<decltype(inputs)>>);
  assert(hooks == 1); EXTRA
  Card::Output result;
  result.result.emplace().value = inputs.number.value * inputs.params.factor.value();
  return result;
}
int main() {
  flatbuffers::FlatBufferBuilder input, params;
  input.Finish(inputs::CreateInputs(input, inputs::number::sample::CreateNumber(input, 21)));
  params.Finish(params::CreateParams(params, 2));
  tsugumi_call call{};
  call.abi_version = TSUGUMI_ABI_VERSION;
  call.run_id = "test"; call.card = "step"; call.invocation = 7;
  call.working_directory = "."; call.temporary_directory = "."; call.output_directory = ".";
  call.inputs = input.GetBufferPointer(); call.inputs_size = input.GetSize();
  call.params = params.GetBufferPointer(); call.params_size = params.GetSize();
  tsugumi_host effects{};
  effects.cancelled = [](void*) { return 0; };
  effects.display_text = [](void*, const char*) { ++reports; return 0; };
  effects.fail = [](void*, const char*, const char*) { assert(false); };
  call.effects = &effects;
  tsugumi_buffer result{};
  call.phase = TSUGUMI_RUN_START;
  assert(tsugumi_card_invoke(&call, &result) == 0 && !result.data);
  call.phase = TSUGUMI_RUN;
  assert(tsugumi_card_invoke(&call, &result) == 0);
  assert(outputs::GetOutputs(result.data)->result()->value() == 42);
  free(result.data);
  assert(reports == REPORTS);
}
'''.replace("ARGS", ", ".join(args)).replace("EXTRA", extra).replace("REPORTS", str(2 if arity >= 2 else 0))
    (tmp_path / "card.cc").write_text(source)
    subprocess.run([compiler, "-std=c++20", "-I" + str(include), "-I" + str(ROOT / "sdk/include"),
                    "-I" + str(tmp_path), "-I" + str(tmp_path / "include"), str(tmp_path / "card.cc"),
                    str(tmp_path / "src/card_adapter.cc"), str(ROOT / "sdk/src/card_runtime.cc"),
                    *(["-Wl,-undefined,dynamic_lookup"] if sys.platform == "darwin" else []), "-o", str(tmp_path / "card")], check=True)
    subprocess.run([str(tmp_path / "card")], check=True)
