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
    def log(stream):
        return text(client.call("LogRangeRequest", runId=run_id, card=path(card),
                                stream=stream, limitBytes=262144).text)

    assert status.state == RunState.succeeded, (status.failure and text(status.failure.diagnostic), log(2))
    invocation, = client.call("RunInvocationsRequest", runId=run_id, card=path(card)).records
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
    result = CardResultT(outputs=[CardPortT(name=name, kind=kind, bytes=list(value))
                                  for name, kind, value in outputs])
    builder.Finish(result.Pack(builder))
    return ",".join(map(str, builder.Output()))


def synthetic_sources(outputs=()):
    """A minimal Card in C, independent of any kit SDK: it prints VALUE
    (from value.h) and returns `outputs` [(name, kind, bytes)]."""
    return {
        "card_abi.h": (ROOT / "protocol/include/tsugumi/card_abi.h").read_text(),
        "card.c": ('#include <stdio.h>\n#include "card_abi.h"\n#include "value.h"\n'
                   "static const uint8_t result_bytes[] = {" + _result_literal(outputs) + "};\n"
                   "int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {\n"
                   "  (void)call;\n  fputs(VALUE \"\\n\", stdout);\n  fflush(stdout);\n"
                   "  *result = (tsugumi_bytes){result_bytes, sizeof(result_bytes)};\n  return 0;\n}\n"),
    }


def synthetic_run(value=None, *, header=None, name="synthetic-card"):
    """A builder written in the Card's own default.nix with the production
    arguments: it compiles the synthetic sources to lib/main.so and places the
    port schemas, a README and a nested file under share/tsugumi/api/ (plus a
    symlink that a reader must not follow). `value` is a Nix string expression;
    `header` is shell that writes value.h instead."""
    header = header or ('cp ${pkgs.writeText "value.h" ("#define VALUE " + builtins.toJSON ('
                        + value + ') + "\\n")} value.h')
    return ('(pkgs.runCommandCC "' + name + "\" { } ''\n"
            '  mkdir -p "$out/lib" "$out/share/tsugumi/api/include"\n'
            "  " + header + "\n"
            "  cp ${./card_abi.h} card_abi.h\n"
            "  cp ${./card.c} card.c\n"
            '  $CC -shared -fPIC -o "$out/lib/main.so" card.c\n'
            '  cp ${schemas}/_inputs.fbs ${schemas}/_outputs.fbs ${schemas}/_params.fbs "$out/share/tsugumi/api/"\n'
            "  printf '# Card API\\n' > \"$out/share/tsugumi/api/README.md\"\n"
            "  printf '#define CARD 1\\n' > \"$out/share/tsugumi/api/include/card.h\"\n"
            "  printf '\\000\\377' > \"$out/share/tsugumi/api/raw\"\n"
            '  ln -s /etc/hosts "$out/share/tsugumi/api/escape"\n'
            "'')")
