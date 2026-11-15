"""Count the full evaluator boundary for actual derivation-input edits."""

import json

import pytest

from harness.fixtures import worker_project
from harness.scenario import run, wait_run
from tsugumi_client import new_id, text
from Tsugumi.Data.RunState import RunState


@pytest.mark.parametrize("component", [
    "source", "lock", "added-file", "removed-file", "nix-param", "contract", "pin-url", "pin-hash",
])
def test_derivation_input_edit_misses_full_evaluation(core_factory, project, tmp_path, component):
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    worker_project(client, project, tmp_path,
                   [{"op": "output", "port": "out", "type": "string", "value": "executed"}],
                   params=[{"name": "message", "type": "string", "nix": True, "data": "first"}])
    directory = project / "definitions/work"
    (directory / "default.nix").write_text('{ pkgs, inputs, params }: { run = "echo first"; }')
    (directory / "lock.json").write_text('{"revision":1}')
    (directory / "optional.txt").write_text("first")

    def reload():
        client.call("ProjectReloadRequest", baseRev=client.call("ProjectInspectRequest").rev,
                    opId=new_id(), origin="test")

    def complete():
        state = wait_run(client, run(client))
        assert state.state == RunState.succeeded, state.failure and text(state.failure.diagnostic)

    reload()
    complete()
    counter = daemon.gates / "full-evaluations"
    assert int(counter.read_text()) == 1
    complete()
    assert int(counter.read_text()) == 1
    if component == "source":
        (directory / "default.nix").write_text('{ pkgs, inputs, params }: { run = "echo second"; }')
    elif component == "lock":
        (directory / "lock.json").write_text('{"revision":2}')
    elif component == "added-file":
        (directory / "added.txt").write_text("new input")
    elif component == "removed-file":
        (directory / "optional.txt").unlink()
    elif component in ("nix-param", "contract"):
        target = directory / "_card.json"
        settings = json.loads(target.read_text())
        settings["params"][0]["data" if component == "nix-param" else "name"] = "second"
        target.write_text(json.dumps(settings))
    else:
        target = project / "tsugumi.json"
        doc = json.loads(target.read_text())
        doc["pkgsPath"] = (
            "https://github.com/NixOS/nixpkgs/archive/" + ("a" if component == "pin-url" else "b") * 40
            + ".tar.gz#sha256-" + ("A" if component == "pin-url" else "B") * 43 + "="
        )
        target.write_text(json.dumps(doc))
    reload()
    complete()
    assert int(counter.read_text()) == 2
    complete()
    assert int(counter.read_text()) == 2


def test_upstream_definition_edit_misses_the_consumer_key(core_factory, project, tmp_path):
    from harness.fixtures import add_worker
    from harness.scenario import Project

    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    graph = Project(project)
    action = [{"op": "output", "port": "out", "type": "string", "value": "executed"}]
    add_worker(graph, "producer", tmp_path / "producer", action)
    add_worker(graph, "work", tmp_path / "consumer", action, inputs=[{"name": "data"}])
    graph.connect("producer", "out", "work", "data").save(client)
    source = project / "definitions/producer/default.nix"
    source.write_text('{ pkgs, inputs, params }: { exports.message = "first"; run = "true"; }')

    def reload():
        client.call("ProjectReloadRequest", baseRev=client.call("ProjectInspectRequest").rev,
                    opId=new_id(), origin="test")

    def complete():
        status = wait_run(client, run(client, "producer"))
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)

    reload()
    complete()
    counter = daemon.gates / "full-evaluations"
    assert int(counter.read_text()) == 2
    complete()
    assert int(counter.read_text()) == 2
    source.write_text(source.read_text().replace('"first"', '"second"'))
    reload()
    complete()
    assert int(counter.read_text()) == 4
    complete()
    assert int(counter.read_text()) == 4


@pytest.mark.parametrize("component", ["reference", "narHash", "overrides"])
def test_flake_product_inputs_miss_the_evaluation_key(core_factory, project, tmp_path, component):
    import hashlib

    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    worker_project(client, project, tmp_path,
                   [{"op": "output", "port": "out", "type": "string", "source": "calls"}],
                   reuseWorker=True)
    (project / "definitions/work/default.nix").write_text(
        '{ pkgs, inputs, params }: { run = "${inputs.sources.fixture.packages.sample}"; }')
    service, = client.call("ProjectInspectRequest").executionServices
    system = text(service.nativeSystem)
    products = daemon.gates / "flakes"
    products.mkdir()
    revision = "a" * 40
    nar_hash = "sha256-" + "A" * 43 + "="

    def install(rev, nar, overrides, product):
        reference = "github:example/fixture/" + rev + "?narHash=" + nar
        source = {"name": "fixture", "type": "flake", "ref": reference, "overrides": overrides}
        request_key = hashlib.sha256((reference + "|" + system + "|" + json.dumps(
            overrides, separators=(",", ":"))).encode()).hexdigest()
        value = {"ref": reference, "rev": rev, "narHash": nar, "packages": {
            "sample": {"name": "sample", "drv": "/nix/store/" + product + "-sample.drv",
                       "out": "/nix/store/" + product + "-sample", "output": "out"},
        }}
        (products / (request_key + ".json")).write_text(json.dumps(value))
        project_file = project / "tsugumi.json"
        document = json.loads(project_file.read_text())
        document["nixSources"] = [source]
        project_file.write_text(json.dumps(document))
        client.call("ProjectReloadRequest", baseRev=client.call("ProjectInspectRequest").rev,
                    opId=new_id(), origin="test")

    def complete():
        status = wait_run(client, run(client))
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)

    install(revision, nar_hash, {}, "0" * 32)
    complete()
    complete()
    counter = daemon.gates / "full-evaluations"
    assert int(counter.read_text()) == 1
    install("b" * 40 if component == "reference" else revision,
            "sha256-" + "B" * 43 + "=" if component == "narHash" else nar_hash,
            {"sample": {"override": {"message": "second"}}} if component == "overrides" else {},
            "1" * 32 if component == "overrides" else "0" * 32)
    complete()
    complete()
    assert int(counter.read_text()) == 2
