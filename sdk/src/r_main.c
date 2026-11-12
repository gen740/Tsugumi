/* R's runtime is loaded once on the worker's main thread. Errors are caught
 * inside R before returning across the ABI; no R longjmp crosses host code. */
#define R_NO_REMAP
#include "native_host.h"
#include <Rembedded.h>
#include <Rinterface.h>
#include <Rinternals.h>
// R_ext/Parse.h requires SEXP from Rinternals.h.
#include <R_ext/Parse.h>
#include <R_ext/Rdynload.h>
#include <ctype.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

static SEXP string_or_null(const char* value) {
  return value ? Rf_ScalarString(Rf_mkCharCE(value, CE_UTF8)) : R_NilValue;
}
/* jsonlite's bigint_as_char covers signed 64-bit values, but unsigned values
 * above INT64_MAX otherwise become rounded doubles. Quote only integer tokens
 * beyond R's exact double range; strings and floating-point tokens stay intact.
 */
static SEXP json_or_null(const char* json) {
  if (!json) return R_NilValue;
  char* encoded = malloc(strlen(json) * 2 + 1);
  if (!encoded) Rf_error("cannot allocate input JSON");
  char* out = encoded;
  const char* p = json;
  while (*p) {
    if (*p == '"') {
      *out++ = *p++;
      while (*p) {
        char c = *p++;
        *out++ = c;
        if (c == '\\' && *p)
          *out++ = *p++;
        else if (c == '"')
          break;
      }
    } else if (*p == '-' || isdigit((unsigned char)*p)) {
      const char* start = p;
      while (*p && strchr("-+0123456789.eE", *p)) ++p;
      const char* digits = start + (*start == '-');
      size_t count = (size_t)(p - digits);
      int integer = 1;
      for (const char* q = digits; q < p; ++q)
        if (!isdigit((unsigned char)*q)) integer = 0;
      int quote = integer &&
                  (count > 16 || (count == 16 &&
                                  strncmp(digits, "9007199254740991", 16) > 0));
      if (quote) *out++ = '"';
      memcpy(out, start, (size_t)(p - start));
      out += p - start;
      if (quote) *out++ = '"';
    } else
      *out++ = *p++;
  }
  *out = '\0';
  SEXP result = PROTECT(string_or_null(encoded));
  free(encoded);
  UNPROTECT(1);
  return result;
}
static SEXP host_input(SEXP port) {
  return json_or_null(
      tsugumi_native_input(Rf_translateCharUTF8(STRING_ELT(port, 0))));
}
static SEXP host_kind(SEXP port) {
  return Rf_ScalarInteger(
      tsugumi_native_input_kind(Rf_translateCharUTF8(STRING_ELT(port, 0))));
}
static SEXP host_invocation(void) {
  return Rf_ScalarReal((double)tsugumi_native_invocation());
}
static SEXP host_params(void) { return json_or_null(tsugumi_native_params()); }
static SEXP host_info(SEXP field) {
  return string_or_null(tsugumi_native_info(Rf_asInteger(field)));
}
static SEXP host_output(SEXP port, SEXP json) {
  return Rf_ScalarLogical(
      tsugumi_native_output(Rf_translateCharUTF8(STRING_ELT(port, 0)),
                            Rf_translateCharUTF8(STRING_ELT(json, 0))) == 0);
}
static SEXP host_display(SEXP json) {
  return Rf_ScalarLogical(
      tsugumi_native_display(Rf_translateCharUTF8(STRING_ELT(json, 0))) == 0);
}
static SEXP host_cancelled(void) {
  return Rf_ScalarLogical(tsugumi_native_cancelled());
}
static SEXP host_remaining(void) {
  return Rf_ScalarReal((double)tsugumi_native_remaining_ms());
}
static SEXP host_fail(SEXP message) {
  tsugumi_native_fail(Rf_translateCharUTF8(STRING_ELT(message, 0)));
  return R_NilValue;
}
/* R's registration API deliberately erases each .Call function signature;
 * the arity below is how R dispatches it back to the matching signature. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
static const R_CallMethodDef methods[] = {
    {"tsugumi_input", (DL_FUNC)&host_input, 1},
    {"tsugumi_params", (DL_FUNC)&host_params, 0},
    {"tsugumi_info", (DL_FUNC)&host_info, 1},
    {"tsugumi_input_kind", (DL_FUNC)&host_kind, 1},
    {"tsugumi_invocation", (DL_FUNC)&host_invocation, 0},
    {"tsugumi_output", (DL_FUNC)&host_output, 2},
    {"tsugumi_display", (DL_FUNC)&host_display, 1},
    {"tsugumi_cancelled", (DL_FUNC)&host_cancelled, 0},
    {"tsugumi_remaining", (DL_FUNC)&host_remaining, 0},
    {"tsugumi_fail", (DL_FUNC)&host_fail, 1},
    {NULL, NULL, 0}};

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static int eval(const char* code) {
  ParseStatus status;
  SEXP text = PROTECT(Rf_mkString(code));
  SEXP parsed = PROTECT(R_ParseVector(text, -1, &status, R_NilValue));
  int error = status != PARSE_OK;
  if (!error)
    for (R_xlen_t i = 0; i < XLENGTH(parsed) && !error; ++i)
      R_tryEval(VECTOR_ELT(parsed, i), R_GlobalEnv, &error);
  UNPROTECT(2);
  return error;
}

int tsugumi_load(void) {
  /* R packages resolve their R symbols through the global loader scope. */
  if (!dlopen(TSUGUMI_LIBR, RTLD_NOW | RTLD_GLOBAL)) {
    fprintf(stderr, "%s\n", dlerror());
    return 1;
  }
  setenv("R_HOME", TSUGUMI_R_HOME, 1);
  setenv("R_LIBS_SITE", TSUGUMI_R_LIBS, 1);
  setenv("FONTCONFIG_FILE", TSUGUMI_R_FONTS, 1);
  R_SignalHandlers = 0;
  char* argv[] = {"tsugumi",      "--silent",       "--no-save",
                  "--no-restore", "--no-site-file", "--no-init-file"};
  /* R's session tempdir survives calls but belongs to the worker, rather
   * than leaking in the host's /tmp when a worker is cancelled. */
  const char* old_tmp = getenv("TMPDIR");
  char* saved_tmp = old_tmp ? strdup(old_tmp) : NULL;
  if (old_tmp && !saved_tmp) return 1;
  const char* home = getenv("HOME");
  if (home) setenv("TMPDIR", home, 1);
  int initialized = Rf_initEmbeddedR(6, argv);
  if (saved_tmp) setenv("TMPDIR", saved_tmp, 1);
  else unsetenv("TMPDIR");
  free(saved_tmp);
  if (initialized < 0) return 1;
  R_registerRoutines(R_getEmbeddingDllInfo(), NULL, methods, NULL, NULL);
  return eval("source(\"" TSUGUMI_R_SDK
              "\", keep.source=FALSE); source(\"" TSUGUMI_R_SOURCE
              "/main.R\", keep.source=FALSE)");
}

static int invoke(const tsugumi_call* call, int hook) {
  if (call->abi_version != TSUGUMI_ABI_VERSION) return 1;
  current_call = call;
  SEXP arg = PROTECT(Rf_ScalarLogical(hook));
  SEXP expr = PROTECT(Rf_lang2(Rf_install(".tsugumi_invoke"), arg));
  int error = 0;
  SEXP result = R_tryEval(expr, R_GlobalEnv, &error);
  int ok = !error && Rf_asLogical(result) == TRUE;
  UNPROTECT(2);
  current_call = NULL;
  return ok ? 0 : 1;
}
int tsugumi_main(const tsugumi_call* call) { return invoke(call, 0); }
int tsugumi_run_start(const tsugumi_call* call) { return invoke(call, 1); }
