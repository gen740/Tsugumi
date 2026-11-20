"""Exercise the real CPython C bridge without a worker, socket or Nix."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import sysconfig

import pytest

ROOT = Path(__file__).resolve().parents[2]


def test_native_callbacks_reject_expired_and_background_calls(tmp_path):
    compiler = shutil.which(os.environ.get('CC', 'cc'))
    include = Path(sysconfig.get_paths()['include'])
    if not compiler or not (include / 'Python.h').is_file():
        pytest.skip('C compiler and Python development headers are required')
    source = tmp_path / 'host.c'
    source.write_text('''#define TSUGUMI_PY_HOME ""
#define TSUGUMI_LIBPYTHON ""
const char tsugumi_python_path[] = "";
#include "python_main.c"
#include <assert.h>
static int cancelled(void* host) { (void)host; return 0; }
static int64_t remaining(void* host) { (void)host; return 123; }
static int reject_display(void* host, uint32_t kind, const void* payload) {
  (void)host;
  assert(payload && (kind == TSUGUMI_DISPLAY_TEXT || kind == TSUGUMI_DISPLAY_SCALAR));
  return 1;
}
static int failures = 0;
static void fail(void* host, const char* kind, const char* message) {
  (void)host;
  assert(strcmp(kind, "worker_failed") == 0 && strcmp(message, "plain failure") == 0);
  ++failures;
}
static void fail_at(void* host, const char* kind, const char* message,
                    const char* source, uint32_t line, const char* cell) {
  (void)host;
  assert(strcmp(kind, "worker_failed") == 0 && strcmp(message, "cell traceback") == 0);
  assert(strcmp(source, "src/notebook.py") == 0 && line == 8 && strcmp(cell, "calculation") == 0);
  ++failures;
}
static PyObject* active(PyObject* self, PyObject* value) {
  (void)self;
  static const tsugumi_host effects = {
    .cancelled = cancelled,
    .remaining_ms = remaining,
    .display = reject_display,
    .fail = fail,
    .fail_at = fail_at,
  };
  static const tsugumi_call call = {.effects = &effects};
  if (!PyObject_IsTrue(value)) assert(failures == 2);
  g_call = PyObject_IsTrue(value) ? &call : NULL;
  Py_RETURN_NONE;
}
static PyMethodDef extra[] = {{"active", active, METH_O, NULL}, {NULL,NULL,0,NULL}};
PyMODINIT_FUNC PyInit__tsugumi_host(void) {
  PyObject* module = init_host();
  if (module && PyModule_AddFunctions(module, extra)) { Py_DECREF(module); return NULL; }
  return module;
}
''')
    library = tmp_path / ('_tsugumi_host' + sysconfig.get_config_var('EXT_SUFFIX'))
    command = [compiler, '-std=c11', '-shared', '-fPIC', '-I' + str(include), '-I' + str(ROOT / 'sdk/include'),
               '-I' + str(ROOT / 'sdk/src'), str(source), '-o', str(library)]
    if sys.platform == 'darwin':
        command += ['-Wl,-undefined,dynamic_lookup']
    subprocess.run(command, check=True, capture_output=True, text=True)
    script = '''import _tsugumi_host as host
import threading

def rejected(callback, kind):
    try: callback()
    except kind: return
    raise AssertionError('callback was accepted')

for callback in (host.cancelled, host.remaining_ms): rejected(callback, RuntimeError)
rejected(lambda: host.fail('worker_failed', 'expired'), RuntimeError)
host.active(True)
assert host.cancelled() is False and host.remaining_ms() == 123
host.fail('worker_failed', 'plain failure')
host.fail('worker_failed', 'cell traceback', 'src/notebook.py', 8, 'calculation')
rejected(lambda: host.display_text('bad'), ValueError)
rejected(lambda: host.display_scalar(float('inf'), 'bad'), ValueError)
errors=[]
def background():
    try:
        for callback in (host.cancelled, host.remaining_ms): rejected(callback, RuntimeError)
        rejected(lambda: host.fail('worker_failed', 'background'), RuntimeError)
    except Exception as error: errors.append(error)
thread=threading.Thread(target=background); thread.start(); thread.join()
assert not errors, errors
host.active(False)
for callback in (host.cancelled, host.remaining_ms): rejected(callback, RuntimeError)
rejected(lambda: host.fail('worker_failed', 'expired'), RuntimeError)
'''
    subprocess.run([sys.executable, '-c', script], cwd=tmp_path, check=True, capture_output=True, text=True)
