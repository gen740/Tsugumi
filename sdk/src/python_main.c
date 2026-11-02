/* main.so for Python Cards: embeds the interpreter and exposes the Card ABI as
 * the `_tsugumi_host` module; `tsugumi._invoke()` runs `card.main.main(ctx)`.
 * With reuseWorker the interpreter and module state persist between calls. */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <wchar.h>

#include "tsugumi/sdk/abi.h"

static const tsugumi_call* g_call = NULL;

static PyObject* host_inputs(PyObject* self, PyObject* args) {
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
  return PyBytes_FromStringAndSize((const char*)g_call->params, (Py_ssize_t)g_call->params_size);
}

static PyObject* host_info(PyObject* self, PyObject* args) {
  return Py_BuildValue("{s:s,s:s,s:K,s:s,s:s,s:s}", "run_id", g_call->run_id, "card", g_call->card, "invocation",
                       (unsigned long long)g_call->invocation, "working_directory", g_call->working_directory,
                       "temporary_directory", g_call->temporary_directory, "output_directory",
                       g_call->output_directory);
}

static PyObject* host_emit_output(PyObject* self, PyObject* args) {
  const char* port;
  Py_buffer buf;
  if (!PyArg_ParseTuple(args, "sy*", &port, &buf)) return NULL;
  int rc = g_call->emit_output(g_call->host, port, (const uint8_t*)buf.buf, (size_t)buf.len);
  PyBuffer_Release(&buf);
  return PyBool_FromLong(rc == 0);
}

static PyObject* host_display_text(PyObject* self, PyObject* args) {
  const char* text;
  if (!PyArg_ParseTuple(args, "s", &text)) return NULL;
  g_call->display_text(g_call->host, text);
  Py_RETURN_NONE;
}

static PyObject* host_display_scalar(PyObject* self, PyObject* args) {
  double value;
  const char* label;
  if (!PyArg_ParseTuple(args, "ds", &value, &label)) return NULL;
  g_call->display_scalar(g_call->host, value, label);
  Py_RETURN_NONE;
}

/* series: list of (id, label, [x...], [y...]) */
static PyObject* host_display_line_chart(PyObject* self, PyObject* args) {
  PyObject* list;
  if (!PyArg_ParseTuple(args, "O!", &PyList_Type, &list)) return NULL;
  Py_ssize_t n = PyList_Size(list);
  tsugumi_series* series = calloc((size_t)n + 1, sizeof(tsugumi_series));
  double** buffers = calloc((size_t)n * 2 + 1, sizeof(double*));
  int ok = 1;
  for (Py_ssize_t i = 0; i < n && ok; ++i) {
    const char *id, *label;
    PyObject *xs, *ys;
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
  if (ok) g_call->display_line_chart(g_call->host, series, (size_t)n);
  for (Py_ssize_t i = 0; i < n * 2; ++i) free(buffers[i]);
  free(buffers);
  free(series);
  if (!ok) return NULL;
  Py_RETURN_NONE;
}

static PyObject* host_log(PyObject* self, PyObject* args) {
  const char* msg;
  if (!PyArg_ParseTuple(args, "s", &msg)) return NULL;
  g_call->log(g_call->host, msg);
  Py_RETURN_NONE;
}

static PyObject* host_cancelled(PyObject* self, PyObject* args) {
  return PyBool_FromLong(g_call->cancelled(g_call->host));
}

static PyObject* host_remaining_ms(PyObject* self, PyObject* args) {
  return PyLong_FromLongLong(g_call->remaining_ms(g_call->host));
}

static PyObject* host_fail(PyObject* self, PyObject* args) {
  const char *kind, *msg;
  if (!PyArg_ParseTuple(args, "ss", &kind, &msg)) return NULL;
  g_call->fail(g_call->host, kind, msg);
  Py_RETURN_NONE;
}

static PyMethodDef host_methods[] = {
    {"inputs", host_inputs, METH_NOARGS, NULL},
    {"params", host_params, METH_NOARGS, NULL},
    {"info", host_info, METH_NOARGS, NULL},
    {"emit_output", host_emit_output, METH_VARARGS, NULL},
    {"display_text", host_display_text, METH_VARARGS, NULL},
    {"display_scalar", host_display_scalar, METH_VARARGS, NULL},
    {"display_line_chart", host_display_line_chart, METH_VARARGS, NULL},
    {"log", host_log, METH_VARARGS, NULL},
    {"cancelled", host_cancelled, METH_NOARGS, NULL},
    {"remaining_ms", host_remaining_ms, METH_NOARGS, NULL},
    {"fail", host_fail, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef host_module = {PyModuleDef_HEAD_INIT, "_tsugumi_host", NULL, -1, host_methods};

static PyObject* init_host(void) { return PyModule_Create(&host_module); }

static int ensure_python(void) {
  static int ready = 0;
  if (ready) return 1;
  if (PyImport_AppendInittab("_tsugumi_host", init_host) != 0) return 0;
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

int tsugumi_main(const tsugumi_call* call) {
  g_call = call;
  if (!ensure_python()) {
    call->fail(call->host, "worker_failed", "cannot initialize the embedded Python interpreter");
    return 1;
  }
  PyObject* mod = PyImport_ImportModule("tsugumi");
  PyObject* result = mod ? PyObject_CallMethod(mod, "_invoke", NULL) : NULL;
  int rc = 0;
  if (!result) {
    /* tsugumi._invoke reports structured failures itself; print the traceback. */
    PyErr_Print();
    call->fail(call->host, "worker_failed", "the Python Card raised an exception (see stderr)");
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
