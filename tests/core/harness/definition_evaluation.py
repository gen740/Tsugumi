"""Observations for definition correctness tests."""

import json

from harness.daemon import ROOT
from harness.scenario import run, wait_run
from tsugumi_client import new_id, path, text
from Tsugumi.Data.RunState import RunState


EMPTY_OUTPUTS = "table Outputs {} root_type Outputs;"


def reload_project(client):
    return client.call("ProjectReloadRequest", baseRev=client.call("ProjectInspectRequest").rev,
                       opId=new_id(), origin="user")


def edit_json(file, edit):
    value = json.loads(file.read_text())
    edit(value)
    file.write_text(json.dumps(value))


def completed_definition(client, card="work", *, start=None):
    """Return a fresh run's output, actual definition identity and invocation."""
    run_id = run(client, start or card)
    status = wait_run(client, run_id)
    card_path = path(*card.split("/"))

    def log(stream):
        return text(client.call("LogRangeRequest", runId=run_id, card=card_path,
                                stream=stream, limitBytes=262144).text)

    assert status.state == RunState.succeeded, (status.failure and text(status.failure.diagnostic), log(2))
    invocation, = client.call("RunInvocationsRequest", runId=run_id, card=card_path).records
    number = text(invocation.environmentId)
    environments = client.call("RunEnvironmentsRequest", runId=run_id).environments
    environment = next(e for e in environments if text(e.id) == number)
    # An environment's ID is a number within its run. What a run was built from
    # is the recorded derivation, manifest and system.
    identity = (text(environment.derivation), text(environment.manifest), text(environment.system))
    return log(1), identity, invocation


def _result_literal(outputs):
    import flatbuffers
    from Tsugumi.Card.CardPort import CardPortT
    from Tsugumi.Card.CardResult import CardResultT

    builder = flatbuffers.Builder(128)
    result = CardResultT(outputs=[CardPortT(name=name, bytes=list(value))
                                  for name, value in outputs])
    builder.Finish(result.Pack(builder))
    return ",".join(map(str, builder.Output()))


def synthetic_sources(outputs=()):
    """A minimal Card in C, independent of any kit SDK: it prints VALUE
    (from value.h) and returns `outputs` [(name, bytes)]."""
    from harness.native_card import RESULT_WRITER

    return {
        "card_abi.h": (ROOT / "protocol/include/tsugumi/card_abi.h").read_text(),
        "card.c": ('#include <stdio.h>\n#include "card_abi.h"\n#include "value.h"\n'
                   + RESULT_WRITER +
                   "static const uint8_t result_bytes[] = {" + _result_literal(outputs) + "};\n"
                   "int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {\n"
                   "  (void)call;\n  fputs(VALUE \"\\n\", stdout);\n  fflush(stdout);\n"
                   "  return write_result(call, result, result_bytes, sizeof(result_bytes));\n}\n"),
    }


def synthetic_run(value=None, *, header=None, name="synthetic-card"):
    """Compile a kit-independent C Card using its confirmed sources."""
    header = header or ('cp ${pkgs.writeText "value.h" ("#define VALUE " + builtins.toJSON ('
                        + value + ') + "\\n")} value.h')
    return ('(pkgs.runCommandCC "' + name + "\" { } ''\n"
            '  mkdir -p "$out/lib"\n'
            "  " + header + "\n"
            "  cp ${./card_abi.h} card_abi.h\n"
            "  cp ${./card.c} card.c\n"
            '  $CC -shared -fPIC -o "$out/lib/main.so" card.c\n'
            "'')")
