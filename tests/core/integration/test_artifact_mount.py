"""Verify the Linux worker mounts only granted directories when they are opened."""

import sys

import pytest

from harness.observations import decode_scalar, read_output
from harness.scenario import Project, run, wait_run
from test_worker_loading import DIRECTORY, native_card
from Tsugumi.Data.RunState import RunState


@pytest.mark.environment
@pytest.mark.skipif(sys.platform != "linux", reason="requires the Linux mount namespace")
def test_opened_artifact_is_a_read_only_mount(core_factory, project):
    source = native_card("""
        tsugumi_artifact_id id;
        const auto directory = fs::path(create_artifact(call, id));
        std::ofstream(directory / "report.txt") << "artifact";
        out.artifact(id);
    """, outputs=DIRECTORY)
    sink = native_card("""
        tsugumi_artifact_id unknown{};
        tsugumi_bytes unused;
        if (call.effects->artifact_open(call.host, &unknown, &unused) >= 0) return 1;
        const auto started = std::chrono::steady_clock::now();
        const auto directory = open_artifact(call, in);
        const auto delay = std::chrono::steady_clock::now() - started;
        struct statvfs mount{};
        if (directory.empty() || statvfs(directory.c_str(), &mount) != 0 ||
            !(mount.f_flag & ST_RDONLY)) return 2;
        std::string contents;
        std::ifstream(fs::path(directory) / "report.txt") >> contents;
        if (contents != "artifact") return 3;
        out.number(std::chrono::duration_cast<std::chrono::microseconds>(delay).count());
    """, inputs=["doc"], recordIO=True, declarations="#include <sys/statvfs.h>")

    client = core_factory().connect()
    graph = Project(project).card("source", source).card("sink", sink)
    graph.connect("source", "doc", "sink", "doc").save(client)
    run_id = run(client, card="source", propagate=True)
    assert wait_run(client, run_id).state == RunState.succeeded
    elapsed = decode_scalar(read_output(client, run_id, card="sink"), "uint64")
    print(f"Linux artifact_open: {elapsed} us")
