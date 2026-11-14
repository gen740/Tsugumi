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
#define TSUGUMI_PY_PATH ""
#include "python_main.c"
static int cancelled(void* host) { (void)host; return 0; }
static int64_t remaining(void* host) { (void)host; return 123; }
static int text(void* host, const char* text) { (void)host; (void)text; return 1; }
static int scalar(void* host, double value, const char* label) { (void)host; (void)value; (void)label; return 1; }
static PyObject* active(PyObject* self, PyObject* value) {
  (void)self;
  static tsugumi_call call;
  call.cancelled = cancelled; call.remaining_ms = remaining;
  call.display_text = text; call.display_scalar = scalar;
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
host.active(True)
assert host.cancelled() is False and host.remaining_ms() == 123
rejected(lambda: host.display_text('bad'), ValueError)
rejected(lambda: host.display_scalar(float('inf'), 'bad'), ValueError)
errors=[]
def background():
    try:
        for callback in (host.cancelled, host.remaining_ms): rejected(callback, RuntimeError)
    except Exception as error: errors.append(error)
thread=threading.Thread(target=background); thread.start(); thread.join()
assert not errors, errors
host.active(False)
for callback in (host.cancelled, host.remaining_ms): rejected(callback, RuntimeError)
'''
    subprocess.run([sys.executable, '-c', script], cwd=tmp_path, check=True, capture_output=True, text=True)
