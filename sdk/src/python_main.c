/* runtime.so for Python Cards: embeds the interpreter and exposes the Card ABI as
 * the `_tsugumi_host` module; `tsugumi._invoke()` runs `card.main.main(inputs[, report[, run]])`.
 * By default each invocation gets a new worker and interpreter; with
 * reuseWorker: true the interpreter and module state persist between calls. */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dlfcn.h>
#include <wchar.h>

#include "tsugumi/sdk/abi.h"

#ifdef __APPLE__
#include <crt_externs.h>
#define environ (*_NSGetEnviron()) /* a shared library has no `environ` to link to */
#else
extern char** environ;
#endif

/* A background Python thread never receives the invocation pointer. */
static _Thread_local const tsugumi_call* g_call = NULL;
static int require_call(void) {
  if (g_call) return 1;
  PyErr_SetString(PyExc_RuntimeError, "Card call has expired or belongs to another thread");
  return 0;
}
static int check_display(int status) {
  if (!status) return 1;
  PyErr_SetString(PyExc_ValueError, "display was rejected by the worker");
  return 0;
}

static PyObject* host_inputs(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  PyObject* list = PyList_New(0);
  for (size_t i = 0; i < g_call->input_count; ++i) {
    const tsugumi_value* v = &g_call->inputs[i];
    PyObject* item = Py_BuildValue("(sIy#)", v->name, v->kind, (const char*)v->data, (Py_ssize_t)v->size);
    PyList_Append(list, item);
    Py_DECREF(item);
  }
  return list;
}

static PyObject* host_params(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  return PyBytes_FromStringAndSize((const char*)g_call->params, (Py_ssize_t)g_call->params_size);
}

static PyObject* host_info(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  return Py_BuildValue("{s:s,s:s,s:K,s:s,s:s,s:s}", "run_id", g_call->run_id, "card", g_call->card, "invocation",
                       (unsigned long long)g_call->invocation, "working_directory", g_call->working_directory,
                       "temporary_directory", g_call->temporary_directory, "output_directory",
                       g_call->output_directory);
}

/* The process environment as (name, value) bytes: os.environ is a copy from
 * when the interpreter started. */
static PyObject* host_environ(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  PyObject* list = PyList_New(0);
  for (char** e = environ; *e; ++e) {
    const char* eq = strchr(*e, '=');
    if (!eq) continue;
    PyObject* item = Py_BuildValue("(y#y)", *e, (Py_ssize_t)(eq - *e), eq + 1);
    PyList_Append(list, item);
    Py_DECREF(item);
  }
  return list;
}

static PyObject* host_emit_output(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  const char* port;
  Py_buffer buf;
  if (!PyArg_ParseTuple(args, "sy*", &port, &buf)) return NULL;
  int rc = g_call->emit_output(g_call->host, port, (const uint8_t*)buf.buf, (size_t)buf.len);
  PyBuffer_Release(&buf);
  return PyBool_FromLong(rc == 0);
}

static PyObject* host_display_text(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  const char* text;
  if (!PyArg_ParseTuple(args, "s", &text)) return NULL;
  if (!check_display(g_call->display_text(g_call->host, text))) return NULL;
  Py_RETURN_NONE;
}

static PyObject* host_display_markdown(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  const char* text;
  if (!PyArg_ParseTuple(args, "s", &text)) return NULL;
  if (!check_display(g_call->display_markdown(g_call->host, text))) return NULL;
  Py_RETURN_NONE;
}

static PyObject* host_display_scalar(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  double value;
  const char* label;
  if (!PyArg_ParseTuple(args, "ds", &value, &label)) return NULL;
  if (!check_display(g_call->display_scalar(g_call->host, value, label))) return NULL;
  Py_RETURN_NONE;
}

/* series: list of (id, label, [x...], [y...]) */
static PyObject* host_display_line_chart(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  PyObject* list;
  if (!PyArg_ParseTuple(args, "O!", &PyList_Type, &list)) return NULL;
  Py_ssize_t n = PyList_Size(list);
  tsugumi_series* series = calloc((size_t)n + 1, sizeof(tsugumi_series));
  double** buffers = calloc((size_t)n * 2 + 1, sizeof(double*));
  int ok = 1;
  for (Py_ssize_t i = 0; i < n && ok; ++i) {
    const char* id;
    const char* label;
    PyObject* xs;
    PyObject* ys;
    if (!PyArg_ParseTuple(PyList_GetItem(list, i), "ssOO", &id, &label, &xs, &ys)) { ok = 0; break; }
    PyObject* xf = PySequence_Fast(xs, "x must be a sequence");
    PyObject* yf = PySequence_Fast(ys, "y must be a sequence");
    if (!xf || !yf) { Py_XDECREF(xf); Py_XDECREF(yf); ok = 0; break; }
    Py_ssize_t count = PySequence_Fast_GET_SIZE(xf);
    if (PySequence_Fast_GET_SIZE(yf) != count) {
      PyErr_SetString(PyExc_ValueError, "x and y differ in length");
      Py_DECREF(xf); Py_DECREF(yf); ok = 0; break;
    }
    double* x = malloc(sizeof(double) * (size_t)(count + 1));
    double* y = malloc(sizeof(double) * (size_t)(count + 1));
    buffers[i * 2] = x;
    buffers[i * 2 + 1] = y;
    for (Py_ssize_t k = 0; k < count; ++k) {
      x[k] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(xf, k));
      y[k] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(yf, k));
    }
    Py_DECREF(xf);
    Py_DECREF(yf);
    if (PyErr_Occurred()) { ok = 0; break; }
    series[i].id = id;
    series[i].label = label;
    series[i].x = x;
    series[i].y = y;
    series[i].count = (size_t)count;
  }
  if (ok) ok = check_display(g_call->display_line_chart(g_call->host, series, (size_t)n));
  for (Py_ssize_t i = 0; i < n * 2; ++i) free(buffers[i]);
  free(buffers);
  free(series);
  if (!ok) return NULL;
  Py_RETURN_NONE;
}

static PyObject* host_display_table(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  PyObject* cols;
  PyObject* rows;
  if (!PyArg_ParseTuple(args, "O!O!", &PyList_Type, &cols, &PyList_Type, &rows)) return NULL;
  Py_ssize_t nc = PyList_Size(cols);
  Py_ssize_t nr = PyList_Size(rows);
  const char** c = calloc((size_t)nc + 1, sizeof(char*));
  const char** cells = calloc((size_t)(nc * nr) + 1, sizeof(char*));
  int ok = 1;
  for (Py_ssize_t i = 0; i < nc && ok; ++i) ok = (c[i] = PyUnicode_AsUTF8(PyList_GetItem(cols, i))) != NULL;
  for (Py_ssize_t r = 0; r < nr && ok; ++r) {
    PyObject* row = PyList_GetItem(rows, r);
    if (!PyList_Check(row) || PyList_Size(row) != nc) {
      PyErr_SetString(PyExc_ValueError, "every row needs one cell per column");
      ok = 0;
      break;
    }
    for (Py_ssize_t k = 0; k < nc && ok; ++k) ok = (cells[r * nc + k] = PyUnicode_AsUTF8(PyList_GetItem(row, k))) != NULL;
  }
  if (ok) ok = check_display(g_call->display_table(g_call->host, c, (size_t)nc, cells, (size_t)nr));
  free(c);
  free(cells);
  if (!ok) return NULL;
  Py_RETURN_NONE;
}

static PyObject* host_display_image(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  const char* media;
  const char* alt;
  Py_buffer data;
  if (!PyArg_ParseTuple(args, "sy*s", &media, &data, &alt)) return NULL;
  const int status = g_call->display_image(g_call->host, media, (const uint8_t*)data.buf, (size_t)data.len, alt);
  PyBuffer_Release(&data);
  if (!check_display(status)) return NULL;
  Py_RETURN_NONE;
}

static PyObject* host_display_points(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  PyObject* xs;
  PyObject* ys;
  PyObject* zs;
  PyObject* cs;
  if (!PyArg_ParseTuple(args, "OOOO", &xs, &ys, &zs, &cs)) return NULL;
  Py_ssize_t n = PySequence_Size(xs);
  if (n < 0 || PySequence_Size(ys) != n || PySequence_Size(zs) != n) {
    PyErr_SetString(PyExc_ValueError, "x, y and z differ in length");
    return NULL;
  }
  int with_colors = cs != Py_None;
  if (with_colors && PySequence_Size(cs) != n) {
    PyErr_SetString(PyExc_ValueError, "colors must be None or one per point");
    return NULL;
  }
  float* p = malloc(sizeof(float) * (size_t)(n * 3 + 1));
  uint32_t* c = with_colors ? malloc(sizeof(uint32_t) * (size_t)(n + 1)) : NULL;
  for (Py_ssize_t i = 0; i < n; ++i) {
    PyObject* a = PySequence_GetItem(xs, i);
    PyObject* b = PySequence_GetItem(ys, i);
    PyObject* d = PySequence_GetItem(zs, i);
    p[i] = (float)PyFloat_AsDouble(a);
    p[n + i] = (float)PyFloat_AsDouble(b);
    p[2 * n + i] = (float)PyFloat_AsDouble(d);
    Py_XDECREF(a); Py_XDECREF(b); Py_XDECREF(d);
    if (c) {
      PyObject* k = PySequence_GetItem(cs, i);
      c[i] = (uint32_t)PyLong_AsUnsignedLong(k);
      Py_XDECREF(k);
    }
  }
  int ok = !PyErr_Occurred();
  if (ok) ok = check_display(g_call->display_points(g_call->host, p, p + n, p + 2 * n, c, (size_t)n));
  free(p);
  free(c);
  if (!ok) return NULL;
  Py_RETURN_NONE;
}

static PyObject* host_display_module(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  const char* name;
  const char* data;
  PyObject* dict;
  if (!PyArg_ParseTuple(args, "ssO!", &name, &data, &PyDict_Type, &dict)) return NULL;
  Py_ssize_t count = PyDict_Size(dict), pos = 0, n = 0;
  tsugumi_blob* blobs = calloc((size_t)count, sizeof(tsugumi_blob));
  Py_buffer* buffers = calloc((size_t)count, sizeof(Py_buffer));
  if (count && (!blobs || !buffers)) { free(blobs); free(buffers); return PyErr_NoMemory(); }
  PyObject *key, *value;
  while (PyDict_Next(dict, &pos, &key, &value)) {
    blobs[n].name = PyUnicode_AsUTF8(key);
    if (!blobs[n].name || PyObject_GetBuffer(value, &buffers[n], PyBUF_CONTIG_RO) != 0) break;
    blobs[n].data = buffers[n].buf;
    blobs[n].size = (size_t)buffers[n].len;
    ++n;
  }
  int status = -1;
  if (!PyErr_Occurred()) status = g_call->display_module(g_call->host, name, data, blobs, (size_t)n);
  for (Py_ssize_t i = 0; i < n; ++i) PyBuffer_Release(&buffers[i]);
  free(blobs); free(buffers);
  if (PyErr_Occurred()) return NULL;
  if (status) { PyErr_SetString(PyExc_ValueError, "invalid module input or unknown module; declare its name in views"); return NULL; }
  Py_RETURN_NONE;
}

static PyObject* host_log(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  const char* msg;
  if (!PyArg_ParseTuple(args, "s", &msg)) return NULL;
  g_call->log(g_call->host, msg);
  Py_RETURN_NONE;
}

static PyObject* host_cancelled(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  return PyBool_FromLong(g_call->cancelled(g_call->host));
}

static PyObject* host_remaining_ms(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  return PyLong_FromLongLong(g_call->remaining_ms(g_call->host));
}

static PyObject* host_fail(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  const char* kind;
  const char* msg;
  if (!PyArg_ParseTuple(args, "ss", &kind, &msg)) return NULL;
  g_call->fail(g_call->host, kind, msg);
  Py_RETURN_NONE;
}

static PyObject* host_finish_outputs(PyObject* self, PyObject* args) {
  if (!require_call()) return NULL;
  return PyBool_FromLong(g_call->finish_outputs(g_call->host) == 0);
}

static PyMethodDef host_methods[] = {
    {"inputs", host_inputs, METH_NOARGS, NULL},
    {"params", host_params, METH_NOARGS, NULL},
    {"info", host_info, METH_NOARGS, NULL},
    {"environ", host_environ, METH_NOARGS, NULL},
    {"finish_outputs", host_finish_outputs, METH_NOARGS, NULL},
    {"emit_output", host_emit_output, METH_VARARGS, NULL},
    {"display_text", host_display_text, METH_VARARGS, NULL},
    {"display_markdown", host_display_markdown, METH_VARARGS, NULL},
    {"display_scalar", host_display_scalar, METH_VARARGS, NULL},
    {"display_line_chart", host_display_line_chart, METH_VARARGS, NULL},
    {"display_table", host_display_table, METH_VARARGS, NULL},
    {"display_image", host_display_image, METH_VARARGS, NULL},
    {"display_points", host_display_points, METH_VARARGS, NULL},
    {"display_module", host_display_module, METH_VARARGS, NULL},
    {"log", host_log, METH_VARARGS, NULL},
    {"cancelled", host_cancelled, METH_NOARGS, NULL},
    {"remaining_ms", host_remaining_ms, METH_NOARGS, NULL},
    {"fail", host_fail, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef host_module = {
    PyModuleDef_HEAD_INIT, "_tsugumi_host", NULL, -1, host_methods, NULL, NULL, NULL, NULL};

static PyObject* init_host(void) { return PyModule_Create(&host_module); }

static int ensure_python(void) {
  static int ready = 0;
  if (ready) return 1;
#ifdef TSUGUMI_LIBPYTHON
  /* The worker loads runtime.so with RTLD_LOCAL; extension modules need the
   * interpreter's symbols globally visible. */
  if (!dlopen(TSUGUMI_LIBPYTHON, RTLD_NOW | RTLD_GLOBAL)) return 0;
#endif
  if (PyImport_AppendInittab("_tsugumi_host", init_host) != 0) return 0;
  /* UTF-8 mode: text I/O and file names are UTF-8 regardless of the locale. */
  PyPreConfig pre;
  PyPreConfig_InitIsolatedConfig(&pre);
  pre.utf8_mode = 1;
  if (PyStatus_Exception(Py_PreInitialize(&pre))) return 0;
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  PyStatus st = PyConfig_SetBytesString(&config, &config.home, TSUGUMI_PY_HOME);
  if (PyStatus_Exception(st)) return 0;
  config.write_bytecode = 0;
  config.install_signal_handlers = 0;
  st = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(st)) return 0;
  /* sys.path: SDK, generated bindings, the Card and its packages. */
  PyObject* sys_path = PySys_GetObject("path");
  const char* paths = TSUGUMI_PY_PATH;
  const char* p = paths;
  Py_ssize_t pos = 0;
  while (*p) {
    const char* e = strchr(p, ':');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    PyObject* item = PyUnicode_FromStringAndSize(p, (Py_ssize_t)len);
    PyList_Insert(sys_path, pos++, item);
    Py_DECREF(item);
    if (!e) break;
    p = e + 1;
  }
  ready = 1;
  return 1;
}

/* Calls tsugumi.<fn>() with g_call set; returns 0 on success. */
static int call_python(const tsugumi_call* call, const char* fn) {
  if (!ensure_python()) {
    if (call) call->fail(call->host, "worker_failed", "cannot initialize the embedded Python interpreter");
    else fprintf(stderr, "cannot initialize the embedded Python interpreter\n");
    return 1;
  }
  g_call = call;
  PyObject* mod = PyImport_ImportModule("tsugumi");
  PyObject* result = mod ? PyObject_CallMethod(mod, fn, NULL) : NULL;
  int rc = 0;
  if (!result) {
    /* tsugumi reports structured failures itself; print the traceback. */
    PyErr_Print();
    if (call) call->fail(call->host, "worker_failed", "the Python Card raised an exception (see stderr)");
    rc = 1;
  } else if (result == Py_False) {
    rc = 1;
  }
  Py_XDECREF(result);
  Py_XDECREF(mod);
  fflush(stdout);
  fflush(stderr);
  PySys_WriteStdout("%s", "");
  g_call = NULL;
  return rc;
}

/* Preparation imports only the SDK. Card imports belong to the first call. */
int tsugumi_prepare(void) {
  if (!ensure_python()) return 1;
  PyObject* mod = PyImport_ImportModule("tsugumi");
  if (!mod) { PyErr_Print(); return 1; }
  Py_DECREF(mod);
  return 0;
}
int tsugumi_load(const tsugumi_call* call) { return call_python(call, "_load"); }

int tsugumi_run_start(const tsugumi_call* call) { return call_python(call, "_run_start"); }

int tsugumi_main(const tsugumi_call* call) { return call_python(call, "_invoke"); }
