"""Exercise artifact grants in the native worker sandbox."""

import sys

import pytest

from harness.observations import decode_scalar, read_output
from projects import card, conn, run_and_wait, write_nix_project
from test_worker_loading import DIRECTORY, native_card
from Tsugumi.Data.RunState import RunState


def producer():
    return native_card("""
        tsugumi_artifact_id id;
        const auto directory = fs::path(create_artifact(call, id));
        std::ofstream(directory / "report.txt") << "artifact";
        out.artifact(id);
    """, outputs=DIRECTORY)


def run_with_granted_artifact(core_factory, project, sink):
    write_nix_project(project, {"source": producer(), "sink": sink},
                      [card("source", "source"), card("sink", "sink")],
                      [conn("source", "doc", "sink", "doc")])
    client = core_factory(project_root=project).connect()
    run_id, status = run_and_wait(client, "source")
    assert status.state == RunState.succeeded, status.failure and status.failure.diagnostic
    return decode_scalar(read_output(client, run_id, card="sink"), "uint64")


@pytest.mark.environment
@pytest.mark.skipif(sys.platform != "darwin", reason="uses macOS Seatbelt")
def test_granted_artifact_is_read_only_in_seatbelt(core_factory, project):
    sink = native_card("""
        tsugumi_artifact_id unknown{};
        tsugumi_bytes unused;
        if (call.effects->artifact_open(call.host, &unknown, &unused) >= 0) return 1;
        const auto directory = open_artifact(call, in);
        if (directory.empty()) return 2;
        const auto member = fs::path(directory) / "report.txt";
        std::string contents;
        std::ifstream(member) >> contents;
        if (contents != "artifact") return 3;
        if (std::ofstream(member, std::ios::app)) return 4;
        if (std::ofstream(fs::path(directory) / "ungranted-write")) return 5;
        out.number(contents.size());
    """, inputs=["doc"], recordIO=True)
    assert run_with_granted_artifact(core_factory, project, sink) == len("artifact")


@pytest.mark.environment
@pytest.mark.skipif(sys.platform != "linux", reason="requires the Linux mount namespace")
def test_opened_artifact_is_a_read_only_mount(core_factory, project):
    sink = native_card("""
        const auto mounts = [] {
          std::ifstream info("/proc/self/mountinfo");
          return std::string(std::istreambuf_iterator<char>(info),
                             std::istreambuf_iterator<char>());
        };
        const auto before = mounts();
        if (before.empty()) return 4;
        tsugumi_artifact_id unknown{};
        tsugumi_bytes unused;
        if (call.effects->artifact_open(call.host, &unknown, &unused) >= 0) return 1;
        if (mounts() != before) return 5;
        const auto started = std::chrono::steady_clock::now();
        const auto directory = open_artifact(call, in);
        const auto delay = std::chrono::steady_clock::now() - started;
        struct statvfs mount{};
        if (directory.empty() || statvfs(directory.c_str(), &mount) != 0 ||
            !(mount.f_flag & ST_RDONLY)) return 2;
        const auto mounted = mounts();
        if (open_artifact(call, in) != directory || mounts() != mounted) return 7;
        if (before.find(directory) != std::string::npos ||
            mounted.find(directory) == std::string::npos) return 6;
        const auto member = fs::path(directory) / "report.txt";
        std::string contents;
        std::ifstream(member) >> contents;
        if (contents != "artifact") return 3;
        if (std::ofstream(member, std::ios::app)) return 8;
        out.number(std::chrono::duration_cast<std::chrono::microseconds>(delay).count());
    """, inputs=["doc"], recordIO=True, declarations="#include <sys/statvfs.h>")

    elapsed = run_with_granted_artifact(core_factory, project, sink)
    print(f"Linux artifact_open: {elapsed} us")
