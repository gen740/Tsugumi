"""Package-controlled names do not authorize code execution before a call."""

import select

from test_worker_loading import library, receive, send_call, worker
from Tsugumi.Worker.WorkerPayload import WorkerPayload


def test_only_main_library_is_loaded(tmp_path):
    """An arbitrary runtime.so is ignored, both in standby and during the invocation."""
    library(tmp_path, r'''
#include <tsugumi/card_abi.h>
#include <stdio.h>
#include <stdlib.h>
__attribute__((constructor)) static void constructor(void) {
  puts("runtime constructor"); fflush(stdout);
  const char* marker = getenv("LOAD_MARKER");
  if (marker) { FILE* f = fopen(marker, "w"); fputs("unexpected", f); fclose(f); }
}
int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
  return 1;
}
''', "runtime")
    library(tmp_path, r'''
#include <tsugumi/card_abi.h>
int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
  static const uint8_t wire[] = {RESULT_BYTES};
  return write_result(result, wire, sizeof(wire));
}
''')
    with worker(tmp_path) as (p, data, _):
        assert not select.select([p.stdout], [], [], 0.3)[0], "package code ran in standby"
        assert not (tmp_path / "loaded").exists()
        send_call(data, tmp_path)
        assert receive(data).valueType == WorkerPayload.WorkerResult
        assert not (tmp_path / "loaded").exists()
        assert not select.select([p.stdout], [], [], 0.1)[0]
        send_call(data, tmp_path, 2)
        assert receive(data).valueType == WorkerPayload.WorkerResult
        assert not select.select([p.stdout], [], [], 0.1)[0]


def test_source_unsets_survive_later_calls(tmp_path):
    """A launch secret unset by library initialization stay absent on reuse."""
    library(tmp_path, r'''
#include <tsugumi/card_abi.h>
#include <stdlib.h>
__attribute__((constructor)) static void load(void) {
  unsetenv("startup_key");
}
int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
  static const uint8_t wire[] = {RESULT_BYTES};
  if (getenv("startup_key")) return 1;
  return write_result(result, wire, sizeof(wire));
}
''')
    with worker(tmp_path, startup={"startup_key": "test-value"}) as (_, data, _):
        for call_id in (1, 2):
            send_call(data, tmp_path, call_id)
            assert receive(data).valueType == WorkerPayload.WorkerResult
