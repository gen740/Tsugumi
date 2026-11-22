"""Worker review regressions at real input/output and scratch boundaries."""

import hashlib
from pathlib import Path

from harness.real_cards import log
from projects import FILE_FBS, card, conn, run_and_wait, write
from test_worker_loading import native_card
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState


def test_dense_file_larger_than_32_mib_reaches_downstream(core_factory, tmp_path):
    """A file with MNIST's byte count crosses both worker boundaries."""
    size = 47040016
    digest = hashlib.sha256(b"x" * size).hexdigest()
    make = native_card(f'''
        std::ofstream file("dense", std::ios::binary);
        for (size_t remaining = {size}; remaining;) {{
          const std::string block(std::min<size_t>(remaining, 65536), 'x');
          file.write(block.data(), block.size());
          remaining -= block.size();
        }}
        if (!file) return 21;
        out.path("dense");
    ''', outputs=FILE_FBS, recordIO=True, retainArtifact=True)
    read = native_card(f'''
        const auto file = input_path(in);
        if (fs::file_size(file) != {size}) return 22;
        out.path(file);
    ''', inputs=["doc"], outputs=FILE_FBS, recordIO=True, retainArtifact=True)
    root = tmp_path / "p"
    write(root, {"make": make, "read": read}, [card("make", "make"), card("read", "read")],
          [conn("make", "doc", "read", "doc")])
    c = core_factory(project_root=root).connect()
    run_id, st = run_and_wait(c, "make")
    assert st.state == RunState.succeeded, log(c, run_id, "read", 2)
    for name in ("make", "read"):
        output, = c.call("RunRecordRequest", runId=run_id, card=path(name)).record.outputs
        saved = Path(text(output.filePath))
        assert saved.stat().st_size == size
        with saved.open("rb") as file:
            assert hashlib.file_digest(file, "sha256").hexdigest() == digest


def test_mode_zero_scratch_does_not_fail_a_successful_call(core_factory, tmp_path):
    """A mode-000 nested directory is cleared between calls."""
    definition = native_card(r'''
        const auto temp = fs::path(std::getenv("TMPDIR")) / "private";
        fs::create_directories(temp / "nested");
        std::ofstream(temp / "nested/data") << "private";
        if (chmod((temp / "nested").c_str(), 0) || chmod(temp.c_str(), 0)) return 1;
        out.number(1);
    ''', reuseWorker=True, recordIO=True)
    write(tmp_path / "p", {"work": definition}, [card("work", "work")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    for _ in range(2):
        run_id, st = run_and_wait(c, "work")
        assert st.state == RunState.succeeded, log(c, run_id, "work", 2)


def test_sparse_output_and_staged_input_keep_their_holes(core_factory, tmp_path):
    """A sparse File stays sparse through transfer, recording and later input staging."""
    make = native_card(r'''
        const int fd = open("sparse", O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0 || ftruncate(fd, 4ll << 30) ||
            pwrite(fd, "data", 4, 2ll << 30) != 4) return 1;
        close(fd);
        out.path("sparse");
    ''', outputs=FILE_FBS, retainArtifact=True)
    read = native_card(r'''
        const auto staged = input_path(in);
        struct stat info;
        if (stat(staged.c_str(), &info) || info.st_blocks * 512 >= 1024 * 1024) return 23;
        out.path(staged);
    ''', inputs=["doc"], outputs=FILE_FBS, recordIO=True, retainArtifact=True, reuseWorker=True)
    root = tmp_path / "p"
    write(root, {"make": make, "read": read}, [card("make", "make"), card("read", "read")],
          [conn("make", "doc", "read", "doc")])
    scratch = tmp_path / "scratch"
    c = core_factory(project_root=root, args=["--run-scratch-dir", str(scratch)]).connect()
    run_id, st = run_and_wait(c, "make")
    assert st.state == RunState.succeeded, log(c, run_id, "read", 2)
    for name in ("make", "read"):
        output, = c.call("RunRecordRequest", runId=run_id, card=path(name)).record.outputs
        saved = Path(text(output.filePath))
        assert saved.stat().st_size == 4 * 1024**3
        assert saved.stat().st_blocks * 512 < 1024**2
        with saved.open("rb") as f:
            assert f.read(4) == b"\0" * 4
            f.seek(2 * 1024**3)
            assert f.read(4) == b"data"
    staged = scratch / "runs" / run_id / "work/make/1/out/sparse"
    assert staged.stat().st_size == 4 * 1024**3
    assert staged.stat().st_blocks * 512 < 1024**2
