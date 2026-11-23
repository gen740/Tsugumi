"""Invocation boundaries on Linux bubblewrap and macOS Seatbelt."""

import json
import os
from pathlib import Path

import pytest

from harness.observations import decode_scalar, read_output
from harness.real_cards import log
from projects import FILE_FBS, card, conn, run_and_wait, start_run, wait_for_log, wait_run, write_nix_project as write
from test_worker_loading import DIRECTORY, native_card
from tsugumi_client import new_id, path, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState


def test_only_wired_files_are_visible_and_worker_reuse_survives(core_factory, tmp_path):
    root = tmp_path / "p"
    make = native_card('std::ofstream("data") << "wired"; out.path("data");',
                       outputs=FILE_FBS, recordIO=True, retainArtifact=True)
    write(root, {"make": make}, [card("secret", "make")], [])
    c = core_factory(project_root=root).connect()
    secret_run, state = run_and_wait(c, "secret")
    assert state.state == RunState.succeeded, log(c, secret_run, "secret", 2)
    record = c.call("RunRecordRequest", runId=secret_run, card=path("secret")).record
    secret = text(record.outputs[0].filePath)
    assert Path(secret).is_file()
    reader = native_card(f'''
        if (fs::exists("/proc/self/fd/900") || std::ifstream({json.dumps(secret)})) return 1;
        const auto file = input_path(in);
        std::string content;
        if (!(std::ifstream(file) >> content) || content != "wired") return 2;
        if (std::ofstream(file, std::ios::app)) return 3;
        out.path(file);
    ''', inputs=["doc"], outputs=FILE_FBS, reuseWorker=True, retainArtifact=True)
    write(root, {"make": make, "read": reader},
          [card("secret", "make"), card("source", "make"), card("reader", "read")],
          [conn("source", "doc", "reader", "doc")])
    c.call("ProjectReloadRequest", baseRev=c.call("ProjectInspectRequest").rev,
           opId=new_id(), origin="test")
    for reused in (False, True):
        run_id, state = run_and_wait(c, "source")
        assert state.state == RunState.succeeded, log(c, run_id, "reader", 2)
        inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("reader")).records
        assert inv.workerReused == reused
        forwarded = c.call("RunRecordRequest", runId=run_id, card=path("reader")).record
        assert not forwarded.outputs[0].fileMissing
        assert Path(text(forwarded.outputs[0].filePath)).read_text() == "wired"


def test_library_initialization_observes_launch_environment_and_logs(core_factory, tmp_path):
    definition = native_card(r'''
        if (!initialized || std::getenv("FROM_MAIN") || std::getenv("TSUGUMI_RUN_ID"))
          return 1;
        out.number(17);
    ''', declarations=r'''
        static bool initialized = false;
        __attribute__((constructor)) static void load() {
          initialized = std::getenv("HOME") && fs::current_path() == fs::canonical(std::getenv("HOME"));
          puts("source started");
          fflush(stdout);
        }
    ''', reuseWorker=True, recordIO=True)
    write(tmp_path / "p", {"show": definition}, [card("show", "show")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    for reused in (False, True):
        run_id, state = run_and_wait(c, "show")
        assert state.state == RunState.succeeded, log(c, run_id, "show", 2)
        assert decode_scalar(read_output(c, run_id, card="show"), "uint64") == 17
        assert ("source started" in log(c, run_id, "show")) != reused
        inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("show")).records
        assert inv.workerReused == reused


def test_library_initialization_is_killed_at_the_invocation_deadline(core_factory, tmp_path):
    definition = native_card('return 42;', declarations=r'''
        __attribute__((constructor)) static void load() {
          puts("source started");
          fflush(stdout);
          sleep(60);
        }
    ''', timeoutSeconds=2)
    write(tmp_path / "p", {"show": definition}, [card("show", "show")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    run_id, state = run_and_wait(c, "show")
    assert state.state == RunState.failed
    inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("show")).records
    assert inv.failure.kind == FailureKind.timeout
    assert "source started" in log(c, run_id, "show")


def test_reused_worker_clears_input_slots_and_keeps_its_work_directory(core_factory, tmp_path):
    make = native_card(r'''
        static unsigned count = 0;
        const char* name = ++count == 1 ? "first.txt" : "second.txt";
        std::ofstream(name) << name;
        out.path(name);
    ''', outputs=FILE_FBS, reuseWorker=True)
    reader = native_card(r'''
        const fs::path staged = input_path(in);
        if (!fs::exists(staged)) return 10;
        if (staged.filename() == "first.txt") {
          std::ofstream("work-marker") << "resident data";
        } else if (staged.filename() != "second.txt" ||
                   fs::exists(staged.parent_path() / "first.txt") ||
                   !fs::exists("work-marker")) return 11;
        printf("staged=%s\n", staged.c_str());
        std::ofstream("current") << "current\n";
        out.path("current");
    ''', inputs=["doc"], outputs=FILE_FBS, reuseWorker=True, recordIO=True, retainArtifact=True)
    root = tmp_path / "p"
    write(root, {"make": make, "read": reader}, [card("make", "make"), card("read", "read")],
          [conn("make", "doc", "read", "doc")])
    c = core_factory(project_root=root).connect()
    for reused in (False, True):
        run_id, state = run_and_wait(c, "make")
        assert state.state == RunState.succeeded, log(c, run_id, "read", 2)
        record = c.call("RunRecordRequest", runId=run_id, card=path("read")).record
        assert record.summary.workerReused == reused
        output = log(c, run_id, "read").strip()
        assert output.startswith("staged=")
        staged = Path(output.removeprefix("staged="))
        assert not staged.exists()
        saved = Path(text(record.outputs[0].filePath))
        assert saved.read_text() == "current\n"
        assert not (saved.parent / "work-marker").exists()


def test_failed_call_with_fifo_preserves_its_failure(core_factory, tmp_path):
    failed = native_card('mkfifo("partial", 0600); return 19;', outputs=FILE_FBS)
    write(tmp_path / "p", {"fail": failed}, [card("fail", "fail")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    run_id, state = run_and_wait(c, "fail")
    assert state.state == RunState.failed
    inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("fail")).records
    assert inv.failure.kind == FailureKind.worker_failed
    assert not c.call("RunRecordRequest", runId=run_id, card=path("fail")).record.outputs
    assert not c.call("ArtifactListRequest", runId=run_id, card=path("fail")).artifacts


def test_cancelled_call_with_fifo_preserves_cancellation(core_factory, tmp_path):
    waiting = native_card(r'''
        if (mkfifo("partial", 0600)) return 1;
        puts("ready");
        fflush(stdout);
        while (!call.effects->cancelled(call.host)) usleep(1000);
    ''', outputs=FILE_FBS)
    write(tmp_path / "p", {"wait": waiting}, [card("wait", "wait")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    run_id = start_run(c, "wait")
    wait_for_log(c, run_id, "wait", 1, lambda entry: "ready" in text(entry.text or b""))
    c.call("RunCancelRequest", runId=run_id)
    assert wait_run(c, run_id).state == RunState.cancelled
    inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("wait")).records
    assert inv.failure.kind == FailureKind.cancelled


@pytest.mark.parametrize("entry", ["directory-link", "fifo"])
def test_successful_call_ignores_undeclared_special_entries(core_factory, tmp_path, entry):
    victim = tmp_path / "private"
    victim.mkdir()
    (victim / "secret").write_text("host-only-secret")
    create = ('if (mkfifo("nested/entry", 0600)) return 1;' if entry == "fifo" else
              f'fs::create_directory_symlink({json.dumps(str(victim))}, "nested/entry");')
    definition = native_card(f'''
        fs::create_directory("nested");
        {create}
        std::ofstream("result") << "safe";
        out.path("result");
    ''', outputs=FILE_FBS, recordIO=True, retainArtifact=True)
    write(tmp_path / "p", {"copy": definition}, [card("copy", "copy")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    run_id, state = run_and_wait(c, "copy")
    assert state.state == RunState.succeeded, log(c, run_id, "copy", 2)
    record = c.call("RunRecordRequest", runId=run_id, card=path("copy")).record
    saved = Path(text(record.outputs[0].filePath))
    assert saved.read_text() == "safe"
    assert not (saved.parent / "nested").exists()


def test_live_child_cannot_copy_a_host_file_by_swapping_output_links(core_factory, tmp_path):
    victim = tmp_path / "host-secret"
    victim.write_text("private-host-content-that-the-card-cannot-read")
    body = r'''
        fs::create_directory("tree");
        std::ofstream("tree/data") << "safe";
        if (std::ifstream(HOST_SECRET)) return 3;
        const auto child = fork();
        if (child < 0) return 4;
        if (!child) {
          for (int i = 0; i < 3000; ++i) {
            if (symlink(HOST_SECRET, "next") || rename("next", "tree/data")) _exit(0);
            usleep(20);
            int fd = open("next", O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd < 0) _exit(1);
            if (write(fd, "safe", 4) != 4) _exit(2);
            close(fd);
            if (rename("next", "tree/data")) _exit(3);
            usleep(20);
          }
          _exit(0);
        }
        out.path("tree");
    '''.replace("HOST_SECRET", json.dumps(str(victim)))
    definition = native_card(body, outputs=DIRECTORY, retainArtifact=True)
    write(tmp_path / "p", {"race": definition}, [card("race", "race")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    for _ in range(8):
        run_id, state = run_and_wait(c, "race")
        if state.state == RunState.failed:
            inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("race")).records
            assert inv.failure.kind == FailureKind.output_invalid, log(c, run_id, "race", 2)
        else:
            assert state.state == RunState.succeeded
            record = c.call("RunRecordRequest", runId=run_id, card=path("race")).record
            snapshot = Path(text(record.outputs[0].filePath))
            assert (snapshot / "data").read_text() == "safe"
            assert all(not entry.is_symlink() for entry in snapshot.rglob("*"))
    assert victim.read_text() == "private-host-content-that-the-card-cannot-read"


def test_resident_work_is_persistent_and_temp_is_per_call(core_factory, tmp_path, monkeypatch):
    bundles = [os.environ.get("NIX_SSL_CERT_FILE", ""), "/etc/ssl/certs/ca-bundle.crt",
               "/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem"]
    bundle = next((Path(name) for name in bundles if name and Path(name).is_file()), None)
    if bundle is None:
        pytest.skip("needs a readable CA bundle for the launcher sentinel")
    sentinel = tmp_path / "launcher-ca-sentinel"
    sentinel.symlink_to(bundle)
    monkeypatch.setenv("SSL_CERT_FILE", str(sentinel))
    monkeypatch.setenv("NIX_SSL_CERT_FILE", str(sentinel))
    definition = native_card(r'''
        const char* home = std::getenv("HOME");
        const char* temporary = std::getenv("TMPDIR");
        if (!home || !temporary || fs::current_path() != fs::canonical(home)) return 1;
        const char* ca = std::getenv("SSL_CERT_FILE");
        const char* nix_ca = std::getenv("NIX_SSL_CERT_FILE");
        if (!ca || !nix_ca || strcmp(ca, nix_ca) || !std::ifstream(ca) ||
            std::string_view(ca).ends_with("launcher-ca-sentinel")) return 2;
        for (const char* name : {"TSUGUMI_RUN_ID", "TSUGUMI_CARD", "TSUGUMI_INVOCATION",
                                 "TSUGUMI_OUTPUT_DIR", "TSUGUMI_WORK_DIR", "TSUGUMI_TMP_DIR"})
          if (std::getenv(name)) return 3;
        const auto marker = fs::path(temporary) / "card-marker";
        if (fs::exists(marker)) return 4;
        std::ofstream(marker) << "temporary";
        unsigned previous = 0;
        std::ifstream("count") >> previous;
        std::ofstream("count") << previous + 1;
        printf("paths=%s|%s\n", home, temporary);
        out.number(previous + 1);
    ''', reuseWorker=True, recordIO=True)
    root = tmp_path / "p"
    write(root, {"work": definition}, [card("work", "work")], [])
    c = core_factory(project_root=root).connect()
    paths = None
    for count in (1, 2):
        run_id, state = run_and_wait(c, "work")
        assert state.state == RunState.succeeded, log(c, run_id, "work", 2)
        assert decode_scalar(read_output(c, run_id), "uint64") == count
        seen = log(c, run_id, "work").strip()
        assert seen.startswith("paths=")
        if paths is not None:
            assert seen == paths
        paths = seen


@pytest.mark.parametrize("case", ["tmp", "linked", "byte-budget", "entry-budget"])
def test_local_output_boundary(core_factory, tmp_path, case):
    outputs = FILE_FBS
    if case == "tmp":
        body = r'''
            const auto file = fs::path(std::getenv("TMPDIR")) / "item";
            std::ofstream(file) << "outside the working directory";
            out.path(file.string());
        '''
    elif case == "linked":
        body = r'''
            std::ofstream("regular") << "inside";
            fs::create_symlink("regular", "linked");
            out.path("linked");
        '''
    elif case == "byte-budget":
        body = r'''
            std::ofstream("large").close();
            fs::resize_file("large", (64ull << 30) + 1);
            out.path("large");
        '''
    else:
        outputs = DIRECTORY
        body = r'''
            fs::create_directory("many");
            for (unsigned i = 0; i < 1000; ++i)
              std::ofstream(fs::path("many") / std::to_string(i)).close();
            out.path("many");
        '''
    definition = native_card(body, outputs=outputs, retainArtifact=True)
    write(tmp_path / "p", {"work": definition}, [card("work", "work")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    run_id, state = run_and_wait(c, "work")
    assert state.state == RunState.failed
    inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("work")).records
    assert inv.failure.kind == FailureKind.output_invalid
    assert not c.call("ArtifactListRequest", runId=run_id, card=path("work")).artifacts
