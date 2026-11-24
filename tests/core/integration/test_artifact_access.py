"""Exercise the project artifact tree under Seatbelt and bubblewrap."""

import json
import stat
import sys

import pytest

from harness.observations import decode_scalar, read_output
from harness.real_cards import log
from projects import card, conn, run_and_wait, write_nix_project
from test_worker_loading import DIRECTORY, native_card
from tsugumi_client import new_id, path
from Tsugumi.Data.RunState import RunState


@pytest.mark.environment
@pytest.mark.skipif(sys.platform not in ("darwin", "linux"), reason="requires Seatbelt or bubblewrap")
def test_artifact_holders_are_searchable_but_not_listable(core_factory, project):
    stored_root = project.resolve() / ".tsugumi/artifacts"
    sandbox_root = stored_root if sys.platform == "darwin" else "/tsugumi-artifacts"
    check_holders = r"""
        static bool protected_holder(const fs::path& path) {
          struct stat info{};
          if (stat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) ||
              (info.st_mode & 0444) != 0) return false;
          if (auto* listing = opendir(path.c_str())) {
            closedir(listing);
            return false;
          }
          if (errno != EACCES && errno != EPERM) return false;
          return chmod(path.c_str(), 0700) != 0;
        }
    """
    declarations = (
        "#include <cerrno>\n#include <dirent.h>\n"
        f"static const fs::path artifact_root = {json.dumps(str(sandbox_root))};\n"
        + check_holders
    )
    source = native_card(r"""
        if (!protected_holder(artifact_root)) return 1;
        tsugumi_artifact_id id;
        const auto directory = fs::path(create_artifact(call, id));
        std::ofstream(directory / "report.txt") << "artifact";
        out.artifact(id);
    """, declarations=declarations, outputs=DIRECTORY)
    sink = native_card(r"""
        tsugumi_artifact_id unknown{};
        tsugumi_bytes unused;
        if (call.effects->artifact_open(call.host, &unknown, &unused) >= 0) return 1;
        const auto directory = fs::path(open_artifact(call, in));
        if (directory.empty()) return 2;
        const auto relative = directory.lexically_relative(artifact_root);
        if (std::distance(relative.begin(), relative.end()) != 4 ||
            *relative.begin() == ".." || directory.filename().string().size() != 32)
          return 3;
        for (auto holder = directory.parent_path(); ; holder = holder.parent_path()) {
          if (!protected_holder(holder)) return 4;
          if (holder == artifact_root) break;
        }
        const auto member = directory / "report.txt";
        std::string contents;
        std::ifstream(member) >> contents;
        if (contents != "artifact") return 5;
        if (std::ofstream(member, std::ios::app) ||
            std::ofstream(directory / "extra") ||
            chmod(directory.c_str(), 0700) == 0 ||
            chmod(member.c_str(), 0600) == 0) return 6;
        const char* granted_project = std::getenv("TSUGUMI_GRANT_PROJECT");
        if (!granted_project) return 7;
        const auto alias = fs::path(granted_project) / ".tsugumi/artifacts" / relative;
        if (!fs::is_directory(alias) || chmod(alias.c_str(), 0700) == 0) return 8;
        out.number(contents.size());
    """, declarations=declarations, inputs=["doc"], recordIO="on", reuseWorker=True,
        grants={"hostPaths": [{"name": "project", "path": str(project), "write": True}]})
    write_nix_project(project, {"source": source, "sink": sink},
                      [card("source", "source"), card("sink", "sink")],
                      [conn("source", "doc", "sink", "doc")])
    client = core_factory(project_root=project).connect()
    for reused in (False, True):
        run_id, status = run_and_wait(client, "source")
        assert status.state == RunState.succeeded, (
            vars(status.failure) if status.failure else None,
            log(client, run_id, "source"), log(client, run_id, "sink")
        )
        assert decode_scalar(read_output(client, run_id, card="sink"), "uint64") == len("artifact")
        inv, = client.call("RunInvocationsRequest", runId=run_id, card=path("sink")).records
        assert inv.workerReused == reused
        run_root = stored_root / run_id
        for holder in (stored_root, run_root, run_root / "source", run_root / "source/@1"):
            assert stat.S_IMODE(holder.stat().st_mode) == 0o311

    removed = client.call("RunHistoryDeleteRequest", opId=new_id(),
                          restrictToRunIds=True, runIds=[run_id])
    assert removed.deletedRuns == 1
    assert not run_root.exists()


@pytest.mark.environment
@pytest.mark.skipif(sys.platform not in ("darwin", "linux"), reason="requires Seatbelt or bubblewrap")
def test_input_card_artifact_opens_at_its_store_path(core_factory, project):
    artifact_id = "a1" * 16
    source = {
        "type": "input",
        "_outputs": DIRECTORY,
        "_nix": r"""
          { params, inputs }:
          let pkgs = import <nixpkgs> { };
          in {
            run = pkgs.runCommand "artifact-input" { } ''
              root=$out/share/tsugumi/input
              mkdir -p "$root/ports" "$root/artifacts/ARTIFACT_ID"
              printf 'opaque' > "$root/ports/doc"
              printf '%s\n' ARTIFACT_ID > "$root/ports/doc.artifacts"
              printf 'store' > "$root/artifacts/ARTIFACT_ID/report.txt"
            '';
          }
        """.replace("ARTIFACT_ID", artifact_id),
    }
    sink = native_card(r"""
        const auto directory = fs::path(open_artifact(call, in));
        if (!directory.string().starts_with("/nix/store/") ||
            !directory.string().ends_with("/share/tsugumi/input/artifacts/" ARTIFACT_ID))
          return 1;
        std::string contents;
        std::ifstream(directory / "report.txt") >> contents;
        if (contents != "store") return 2;
        out.number(contents.size());
    """.replace("ARTIFACT_ID", json.dumps(artifact_id)), inputs=["doc"], recordIO="on")
    write_nix_project(project, {"source": source, "sink": sink},
                      [card("source", "source"), card("sink", "sink")],
                      [conn("source", "doc", "sink", "doc")])
    client = core_factory(project_root=project).connect()
    run_id, status = run_and_wait(client, "source")
    assert status.state == RunState.succeeded, vars(status.failure) if status.failure else None
    assert decode_scalar(read_output(client, run_id, card="sink"), "uint64") == len("store")
    assert not (project / ".tsugumi/artifacts" / run_id).exists()
