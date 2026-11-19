"""Package-controlled names do not authorize code execution before a call."""

import select

from test_worker_loading import library, receive, send_call, worker
from Tsugumi.Worker.WorkerPayload import WorkerPayload


def test_only_main_library_is_loaded(tmp_path):
    """An arbitrary runtime.so is ignored, both in standby and during the invocation."""
    library(tmp_path, r'''
#include <tsugumi/sdk/abi.h>
#include <stdio.h>
#include <stdlib.h>
__attribute__((constructor)) static void constructor(void) {
  puts("runtime constructor"); fflush(stdout);
  const char* marker = getenv("LOAD_MARKER");
  if (marker) { FILE* f = fopen(marker, "w"); fputs(getenv("TSUGUMI_RUN_ID"), f); fclose(f); }
}
int tsugumi_main(const tsugumi_call* c) { return 1; }
''', "runtime")
    library(tmp_path, r'''
#include <tsugumi/sdk/abi.h>
int tsugumi_main(const tsugumi_call* c) {
  if (c->phase == TSUGUMI_LOAD) return tsugumi_display_text(c, c->run_id, 0);
  if (c->phase == TSUGUMI_RUN) return 0;
  return 0;
}
''')
    with worker(tmp_path) as (p, data, _):
        assert not select.select([p.stdout], [], [], 0.3)[0], "package code ran in standby"
        assert not (tmp_path / "loaded").exists()
        send_call(data, tmp_path)
        shown = receive(data)
        assert shown.valueType == WorkerPayload.WorkerDisplay
        assert shown.value.display.runId == b"recorded-run"
        assert shown.value.display.value.text == b"recorded-run"
        assert receive(data).valueType == WorkerPayload.WorkerResult
        assert not (tmp_path / "loaded").exists()
        assert not select.select([p.stdout], [], [], 0.1)[0]
        send_call(data, tmp_path, 2)
        assert receive(data).valueType == WorkerPayload.WorkerResult
        assert not select.select([p.stdout], [], [], 0.1)[0]


def test_source_unsets_survive_later_calls(tmp_path):
    """Runtime defaults and call variables unset by source loading stay absent on reuse."""
    library(tmp_path, r'''
#include <tsugumi/sdk/abi.h>
#include <stdlib.h>
int tsugumi_main(const tsugumi_call* c) {
  if (c->phase == TSUGUMI_PREPARE) setenv("SDK_DEFAULT", "default", 1);
  if (c->phase == TSUGUMI_LOAD) {
    unsetenv("SDK_DEFAULT"); unsetenv("ONLY_FIRST_CALL");
  }
  if (c->phase != TSUGUMI_RUN) return 0;
  if (getenv("SDK_DEFAULT") || getenv("ONLY_FIRST_CALL")) return 1;
  return 0;
}
''')
    with worker(tmp_path) as (_, data, _):
        for call_id in (1, 2):
            send_call(data, tmp_path, call_id)
            assert receive(data).valueType == WorkerPayload.WorkerResult
