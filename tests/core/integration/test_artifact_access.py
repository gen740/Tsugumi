"""Exercise invocation artifact directories under Seatbelt and bubblewrap."""

import json
import stat
import sys
from uuid import UUID

import pytest

from harness.observations import decode_scalar, read_output
from harness.real_cards import log
from projects import card, conn, run_and_wait, write_nix_project
from test_worker_loading import DIRECTORY, native_card
from tsugumi_client import new_id, text
from Tsugumi.Data.RunState import RunState


@pytest.mark.environment
@pytest.mark.skipif(sys.platform not in ("darwin", "linux"), reason="requires Seatbelt or bubblewrap")
def test_invocation_can_open_a_sealed_artifact_by_uuid(core_factory, project):
    stored_root = project.resolve() / ".tsugumi/artifacts"
    declarations = (
        "#include <cerrno>\n#include <dirent.h>\n"
        f"static const fs::path store = {json.dumps(str(stored_root))};\n"
    )
    source = native_card(r"""
        if (auto* listing = opendir(store.c_str())) {
          closedir(listing);
          return 1;
        }
        tsugumi_artifact_id id, ungranted;
        const auto directory = fs::path(create_artifact(call, id));
        const auto private_output = fs::path(create_artifact(call, ungranted));
        fs::create_directories(directory / "nested/empty");
        std::ofstream(directory / "nested/report.txt") << "artifact";
        std::ofstream(private_output / "hidden.txt") << "ungranted";
        out.artifact(id);
    """, declarations=declarations, outputs=DIRECTORY)
    sandboxed_grant = sys.platform == "linux"
    sink = native_card(r"""
        tsugumi_artifact_id ungranted;
        const auto* port = in.inputs()->Get(0);
        const auto* artifact = fb::GetRoot<tsugumi::Artifact>(port->bytes()->data());
        std::copy_n(artifact->id()->bytes()->data(), 16, ungranted.bytes);
        ungranted.bytes[15] ^= 1;
        tsugumi_bytes unused;
        if (call.effects->artifact_open(call.host, &ungranted, &unused) >= 0) return 1;
        const auto directory = fs::path(open_artifact(call, in));
        if (directory.empty() || directory.string().starts_with(store.string())) return 2;
        if (fs::path(open_artifact(call, in)) != directory) return 3;
        if (std::distance(fs::directory_iterator(directory.parent_path()),
                          fs::directory_iterator{}) != 1) return 4;
        if (!fs::is_empty(directory / "nested/empty")) return 5;
        const auto member = directory / "nested/report.txt";
        std::string contents;
        std::ifstream(member) >> contents;
        if (contents != "artifact") return 6;
        if (std::ofstream(member, std::ios::app) ||
            std::ofstream(directory / "extra") ||
            chmod(directory.c_str(), 0700) == 0 ||
            chmod(member.c_str(), 0600) == 0) return 7;
#ifdef __linux__
        if (errno != EROFS) return 8;
#endif
#ifdef __linux__
        const char* granted_project = std::getenv("TSUGUMI_GRANT_PROJECT");
        if (!granted_project) return 9;
        if (auto* listing = opendir((fs::path(granted_project) / ".tsugumi/artifacts").c_str())) {
          while (auto* entry = readdir(listing)) {
            const std::string_view name(entry->d_name);
            if (name != "." && name != "..") {
              closedir(listing);
              return 10;
            }
          }
          closedir(listing);
        }
#endif
        out.number(contents.size());
    """, declarations=declarations, inputs=["doc"], recordIO="on",
        grants={"hostPaths": [{"name": "project", "write": True}]} if sandboxed_grant else {})
    write_nix_project(project, {"source": source, "sink": sink},
                      [card("source", "source"), card("sink", "sink")],
                      [conn("source", "doc", "sink", "doc")],
                      host_paths=("project",) if sandboxed_grant else ())
    daemon = core_factory(project_root=project)
    if sandboxed_grant:
        (daemon.control / "host-path-bindings.json").write_text(json.dumps({
            "project": {"path": str(project.resolve()), "write": True},
        }))
    client = daemon.connect()
    previous = 0
    for _ in range(2):
        run_id, status = run_and_wait(client, "source")
        assert status.state == RunState.succeeded, (
            vars(status.failure) if status.failure else None,
            log(client, run_id, "source"), log(client, run_id, "sink")
        )
        assert decode_scalar(read_output(client, run_id, card="sink"), "uint64") == len("artifact")
        first, second = client.call("ArtifactListRequest", runId=run_id).artifacts
        sequence, counter = map(int, text(first.label).split("."))
        assert sequence > previous and counter == 0
        assert text(second.label) == f"{sequence}.1"
        assert all(UUID(text(a.artifactId)).version == 4 for a in (first, second))
        previous = sequence
        run_root = stored_root / run_id
        saved = run_root / "source/@1/0"
        assert (saved / "nested/report.txt").read_text() == "artifact"
        assert (run_root / "source/@1/1/hidden.txt").read_text() == "ungranted"
        for path in (saved, *saved.rglob("*")):
            assert not stat.S_IMODE(path.stat().st_mode) & 0o222

        removed = client.call("RunHistoryDeleteRequest", opId=new_id(),
                              restrictToRunIds=True, runIds=[run_id])
        assert removed.deletedRuns == 1
        assert not run_root.exists()


@pytest.mark.environment
@pytest.mark.skipif(sys.platform != "linux", reason="macOS seals clones")
def test_without_pid_isolation_outputs_are_renamed(core_factory, project):
    """With PID isolation off nothing is guaranteed about lingering writers,
    so the staged tree is renamed into the store as it is."""
    source = native_card(r"""
        tsugumi_artifact_id id;
        const auto directory = fs::path(create_artifact(call, id));
        std::ofstream(directory / "report.txt") << "renamed";
        struct stat status{};
        if (stat((directory / "report.txt").c_str(), &status) != 0) return 1;
        std::ofstream(directory / "inode") << status.st_ino;
        out.artifact(id);
    """, outputs=DIRECTORY, sandbox={"pid": False})
    write_nix_project(project, {"source": source}, [card("source", "source")], [])
    client = core_factory(project_root=project).connect()
    run_id, status = run_and_wait(client, "source")
    assert status.state == RunState.succeeded, log(client, run_id, "source")
    saved = project.resolve() / ".tsugumi/artifacts" / run_id / "source/@1/0"
    assert (saved / "report.txt").stat().st_ino == int((saved / "inode").read_text())
