"""Worker review regressions at real input/output and scratch boundaries."""

import hashlib
from pathlib import Path

from harness.real_cards import bash, log
from projects import FILE_FBS, card, conn, run_and_wait, write
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState


def test_dense_file_larger_than_32_mib_reaches_downstream(core_factory, tmp_path):
    """A File with MNIST's byte count crosses worker output and next-worker input staging."""
    size = 47040016
    digest = hashlib.sha256(b"x" * size).hexdigest()
    make = bash(
        f'head -c {size} /dev/zero | tr "\\000" x > "$TSUGUMI_OUTPUT_DIR/dense"; '
        f'test "$(wc -c < "$TSUGUMI_OUTPUT_DIR/dense")" -eq {size} || exit 21; '
        'printf \'{"doc":{"value":"%s/dense"}}\' "$TSUGUMI_OUTPUT_DIR" > "$TSUGUMI_OUTPUT"',
        outputs=FILE_FBS, recordIO=True, retainArtifact=True)
    read = bash(
        f'test "$(wc -c < "$FILE_DOC")" -eq {size} || exit 22; '
        f'test "$(sha256sum "$FILE_DOC" | cut -d " " -f 1)" = {digest} || exit 23; '
        'printf \'{"doc":{"value":"%s"}}\' "$FILE_DOC" > "$TSUGUMI_OUTPUT"',
        inputs=["doc"], outputs=FILE_FBS, recordIO=True, retainArtifact=True)
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
    """A mode-000 nested directory is cleared without turning a successful result into a cleanup failure."""
    definition = bash('mkdir -p "$TSUGUMI_TMP_DIR/private/nested"; '
                      'echo private > "$TSUGUMI_TMP_DIR/private/nested/data"; '
                      'chmod 000 "$TSUGUMI_TMP_DIR/private/nested" "$TSUGUMI_TMP_DIR/private"; '
                      'printf \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"',
                      reuseWorker=True, recordIO=True)
    write(tmp_path / "p", {"work": definition}, [card("work", "work")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    for _ in range(2):
        run_id, st = run_and_wait(c, "work")
        assert st.state == RunState.succeeded, log(c, run_id, "work", 2)


def test_sparse_output_and_staged_input_keep_their_holes(core_factory, tmp_path):
    """A sparse File remains sparse through output transfer, recording and the downstream input copy."""
    source = r'''
#include <tsugumi/sdk/abi.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
int tsugumi_main(const tsugumi_call* c) {
  if (c->phase != TSUGUMI_RUN) return 0;
  char path[4096], json[8192];
  snprintf(path, sizeof path, "%s/sparse", c->output_directory);
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0 || ftruncate(fd, (off_t)4 * 1024 * 1024 * 1024) ||
      pwrite(fd, "data", 4, (off_t)2 * 1024 * 1024 * 1024) != 4) return 1;
  close(fd);
  snprintf(json, sizeof json, "{\"value\":\"%s\"}", path);
  return c->effects->output_json(c->host, "doc", json);
}
'''
    make = {
        "type": "code", "language": "cpp", "retainArtifact": True, "_outputs": FILE_FBS,
        "_nix": '''{ pkgs, inputs, params }: {
  run = pkgs.runCommandCC "sparse-output" {} ''
    mkdir -p $out/lib
    $CC -shared -fPIC -I${inputs.tsugumi.runtime}/include ${./src/main.c} -o $out/lib/main.so
  '';
}
''',
    }
    read = bash('du -k "$FILE_DOC" | { read -r allocated ignored; test "$allocated" -lt 1024; } || exit 23; '
                'printf \'{"text":"%s"}\' "$FILE_DOC" > "$TSUGUMI_REPORT"; '
                'printf \'{"doc":{"value":"%s"}}\' "$FILE_DOC" > "$TSUGUMI_OUTPUT"',
                inputs=["doc"], outputs=FILE_FBS, recordIO=True, retainArtifact=True, reuseWorker=True)
    root = tmp_path / "p"
    write(root, {"make": make, "read": read}, [card("make", "make"), card("read", "read")],
          [conn("make", "doc", "read", "doc")], {"definitions/make/src/main.c": source})
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
