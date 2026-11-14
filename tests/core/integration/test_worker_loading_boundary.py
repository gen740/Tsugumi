"""Package-controlled names do not authorize code execution before a call."""

import select

from test_worker_loading import library, receive, send_call, worker
from Tsugumi.Worker.WorkerPayload import WorkerPayload


def test_package_named_runtime_waits_for_its_invocation(tmp_path):
    """An arbitrary runtime.so constructor and initializer wait for the recorded call."""
    library(tmp_path, r'''
#include <tsugumi/sdk/abi.h>
#include <stdio.h>
#include <stdlib.h>
__attribute__((constructor)) static void constructor(void) {
  puts("runtime constructor"); fflush(stdout);
  const char* marker = getenv("LOAD_MARKER");
  if (marker) { FILE* f = fopen(marker, "w"); fputs(getenv("TSUGUMI_RUN_ID"), f); fclose(f); }
}
int tsugumi_prepare(void) { puts("runtime initialized"); fflush(stdout); return 0; }
int tsugumi_load(const tsugumi_call* c) { return c->display_text(c->host, c->run_id); }
int tsugumi_main(const tsugumi_call* c) { return 0; }
''', "runtime")
    with worker(tmp_path) as (p, data, _):
        assert not select.select([p.stdout], [], [], 0.3)[0], "package code ran in standby"
        assert not (tmp_path / "loaded").exists()
        send_call(data, tmp_path)
        shown = receive(data)
        assert shown.valueType == WorkerPayload.WorkerDisplay
        assert shown.value.display.runId == b"recorded-run"
        assert shown.value.display.value.text == b"recorded-run"
        assert receive(data).valueType == WorkerPayload.WorkerResult
        assert (tmp_path / "loaded").read_text() == "recorded-run"
        assert p.stdout.readline() == b"runtime constructor\n"
        assert p.stdout.readline() == b"runtime initialized\n"
        send_call(data, tmp_path, 2)
        assert receive(data).valueType == WorkerPayload.WorkerResult
        assert not select.select([p.stdout], [], [], 0.1)[0]


def test_source_unsets_survive_later_calls(tmp_path):
    """Runtime defaults and call variables unset by source loading stay absent on reuse."""
    library(tmp_path, r'''
#include <tsugumi/sdk/abi.h>
#include <stdlib.h>
int tsugumi_prepare(void) { setenv("SDK_DEFAULT", "default", 1); return 0; }
int tsugumi_load(const tsugumi_call* c) {
  unsetenv("SDK_DEFAULT"); unsetenv("ONLY_FIRST_CALL");
  return 0;
}
int tsugumi_main(const tsugumi_call* c) {
  return getenv("SDK_DEFAULT") || getenv("ONLY_FIRST_CALL") ? 1 : 0;
}
''', "runtime")
    with worker(tmp_path) as (_, data, _):
        for call_id in (1, 2):
            send_call(data, tmp_path, call_id)
            assert receive(data).valueType == WorkerPayload.WorkerResult
