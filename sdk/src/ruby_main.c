/* MRI lives on the worker's invocation thread. Only this C translation unit
 * calls Ruby: rb_protect catches longjmp before returning to the C++ worker. */
#include <dlfcn.h>
#include <ruby.h>
#include <ruby/encoding.h>
#include <stdlib.h>
#include <string.h>

#include "tsugumi/sdk/abi.h"

const char* tsugumi_json_decode(const char*, const char*, const uint8_t*, size_t);
const char* tsugumi_json_error(void);
int tsugumi_json_emit(const tsugumi_call*, const char*, const char*);

static const tsugumi_call* current = NULL;
static VALUE sdk = Qnil, owner = Qnil;
static int ready = 0;

static void require_call(void) {
  if (!current || rb_thread_current() != owner)
    rb_raise(rb_eRuntimeError, "Card invocation is available only on the invocation thread during a call");
}

static VALUE string(const char* text) { return rb_utf8_str_new_cstr(text ? text : ""); }

static VALUE host_info(VALUE self) {
  require_call();
  VALUE h = rb_hash_new();
#define INFO(name) rb_hash_aset(h, string(#name), string(current->name))
  INFO(run_id);
  INFO(card);
  INFO(working_directory);
  INFO(temporary_directory);
  INFO(output_directory);
#undef INFO
  rb_hash_aset(h, string("invocation"), ULL2NUM(current->invocation));
  return h;
}

static VALUE host_inputs(VALUE self) {
  require_call();
  VALUE all = rb_ary_new();
  for (size_t i = 0; i < current->input_count; ++i) {
    const tsugumi_value* v = &current->inputs[i];
    const char* json = tsugumi_json_decode("inputs", v->name, v->data, v->size);
    if (!json) rb_raise(rb_eRuntimeError, "%s", tsugumi_json_error());
    VALUE entry = rb_ary_new_from_args(3, string(v->name), UINT2NUM(v->kind), string(json));
    rb_ary_push(all, entry);
  }
  return all;
}

static VALUE host_params(VALUE self) {
  require_call();
  const char* json = tsugumi_json_decode("params", "", current->params, current->params_size);
  if (!json) rb_raise(rb_eRuntimeError, "%s", tsugumi_json_error());
  return string(json);
}

static VALUE host_output(VALUE self, VALUE port, VALUE json) {
  require_call();
  const char* name = StringValueCStr(port);
  const char* value = StringValueCStr(json);
  if (tsugumi_json_emit(current, name, value)) rb_raise(rb_eArgError, "%s", tsugumi_json_error());
  return Qnil;
}

static VALUE host_text(VALUE self, VALUE value) {
  require_call();
  current->display_text(current->host, StringValueCStr(value));
  return Qnil;
}
static VALUE host_markdown(VALUE self, VALUE value) {
  require_call();
  current->display_markdown(current->host, StringValueCStr(value));
  return Qnil;
}
static VALUE host_scalar(VALUE self, VALUE value, VALUE label) {
  require_call();
  current->display_scalar(current->host, NUM2DBL(value), StringValueCStr(label));
  return Qnil;
}
static VALUE host_module(VALUE self, VALUE name, VALUE json) {
  require_call();
  current->display_module(current->host, StringValueCStr(name), StringValueCStr(json), NULL, 0);
  return Qnil;
}
static VALUE host_image(VALUE self, VALUE media, VALUE bytes, VALUE alt) {
  require_call();
  StringValue(bytes);
  current->display_image(current->host, StringValueCStr(media), (const uint8_t*)RSTRING_PTR(bytes),
                         (size_t)RSTRING_LEN(bytes), StringValueCStr(alt));
  return Qnil;
}
static VALUE host_table(VALUE self, VALUE columns, VALUE rows) {
  require_call();
  Check_Type(columns, T_ARRAY);
  Check_Type(rows, T_ARRAY);
  long n = RARRAY_LEN(columns), count = RARRAY_LEN(rows);
  if (n > 10000 || count > 100000 || (n && count > 1000000 / n))
    rb_raise(rb_eArgError, "display table is too large");
  /* Validate before allocating C arrays: conversions may raise. */
  for (long j = 0; j < n; ++j) {
    VALUE s = rb_ary_entry(columns, j);
    Check_Type(s, T_STRING);
    StringValueCStr(s);
  }
  for (long i = 0; i < count; ++i) {
    VALUE row = rb_ary_entry(rows, i);
    Check_Type(row, T_ARRAY);
    if (RARRAY_LEN(row) != n) rb_raise(rb_eArgError, "table row width differs from columns");
    for (long j = 0; j < n; ++j) {
      VALUE s = rb_ary_entry(row, j);
      Check_Type(s, T_STRING);
      StringValueCStr(s);
    }
  }
  const char** names = calloc((size_t)n + 1, sizeof(char*));
  const char** cells = calloc((size_t)n * (size_t)count + 1, sizeof(char*));
  if (!names || !cells) {
    free(names);
    free(cells);
    rb_memerror();
  }
  for (long j = 0; j < n; ++j) names[j] = RSTRING_PTR(rb_ary_entry(columns, j));
  for (long i = 0; i < count; ++i)
    for (long j = 0; j < n; ++j) cells[i * n + j] = RSTRING_PTR(rb_ary_entry(rb_ary_entry(rows, i), j));
  current->display_table(current->host, names, (size_t)n, cells, (size_t)count);
  free(names);
  free(cells);
  return Qnil;
}
/* rows: [[id, label, x, y], ...] with x and y packed as native doubles. The
 * numbers are copied out, so alignment and the GC do not matter to the host. */
static VALUE host_line_chart(VALUE self, VALUE rows) {
  require_call();
  Check_Type(rows, T_ARRAY);
  long n = RARRAY_LEN(rows);
  if (n > 10000) rb_raise(rb_eArgError, "display line chart has too many series");
  size_t total = 0;
  for (long i = 0; i < n; ++i) {
    VALUE row = rb_ary_entry(rows, i);
    Check_Type(row, T_ARRAY);
    if (RARRAY_LEN(row) != 4) rb_raise(rb_eArgError, "a series is id, label, x and y");
    for (long j = 0; j < 4; ++j) Check_Type(rb_ary_entry(row, j), T_STRING);
    VALUE id = rb_ary_entry(row, 0), label = rb_ary_entry(row, 1);
    StringValueCStr(id);
    StringValueCStr(label);
    long x = RSTRING_LEN(rb_ary_entry(row, 2)), y = RSTRING_LEN(rb_ary_entry(row, 3));
    if (x != y || x % (long)sizeof(double)) rb_raise(rb_eArgError, "series x and y differ in length");
    total += (size_t)x / sizeof(double);
  }
  tsugumi_series* series = calloc((size_t)n + 1, sizeof(tsugumi_series));
  double* numbers = calloc(2 * total + 1, sizeof(double));
  if (!series || !numbers) {
    free(series);
    free(numbers);
    rb_memerror();
  }
  double* next = numbers;
  for (long i = 0; i < n; ++i) {
    VALUE row = rb_ary_entry(rows, i);
    size_t count = (size_t)RSTRING_LEN(rb_ary_entry(row, 2)) / sizeof(double);
    series[i].id = RSTRING_PTR(rb_ary_entry(row, 0));
    series[i].label = RSTRING_PTR(rb_ary_entry(row, 1));
    series[i].count = count;
    series[i].x = next;
    if (count) memcpy(next, RSTRING_PTR(rb_ary_entry(row, 2)), count * sizeof(double));
    next += count;
    series[i].y = next;
    if (count) memcpy(next, RSTRING_PTR(rb_ary_entry(row, 3)), count * sizeof(double));
    next += count;
  }
  current->display_line_chart(current->host, series, (size_t)n);
  free(series);
  free(numbers);
  return Qnil;
}
/* x, y, z packed as native floats; colors packed as uint32, or nil. */
static VALUE host_points(VALUE self, VALUE x, VALUE y, VALUE z, VALUE colors) {
  require_call();
  Check_Type(x, T_STRING);
  Check_Type(y, T_STRING);
  Check_Type(z, T_STRING);
  if (!NIL_P(colors)) Check_Type(colors, T_STRING);
  long bytes = RSTRING_LEN(x);
  if (bytes % (long)sizeof(float) || RSTRING_LEN(y) != bytes || RSTRING_LEN(z) != bytes ||
      (!NIL_P(colors) && RSTRING_LEN(colors) != bytes))
    rb_raise(rb_eArgError, "points need x, y, z (and colors) of one length");
  size_t count = (size_t)bytes / sizeof(float);
  float* p = calloc(3 * count + 1, sizeof(float));
  uint32_t* c = NIL_P(colors) ? NULL : calloc(count + 1, sizeof(uint32_t));
  if (!p || (!NIL_P(colors) && !c)) {
    free(p);
    free(c);
    rb_memerror();
  }
  if (count) {
    memcpy(p, RSTRING_PTR(x), (size_t)bytes);
    memcpy(p + count, RSTRING_PTR(y), (size_t)bytes);
    memcpy(p + 2 * count, RSTRING_PTR(z), (size_t)bytes);
    if (c) memcpy(c, RSTRING_PTR(colors), (size_t)bytes);
  }
  current->display_points(current->host, p, p + count, p + 2 * count, c, count);
  free(p);
  free(c);
  return Qnil;
}
static VALUE host_log(VALUE self, VALUE message) {
  require_call();
  current->log(current->host, StringValueCStr(message));
  return Qnil;
}
static VALUE host_cancelled(VALUE self) {
  require_call();
  return current->cancelled(current->host) ? Qtrue : Qfalse;
}
static VALUE host_remaining(VALUE self) {
  require_call();
  return LL2NUM(current->remaining_ms(current->host));
}
static VALUE host_fail(VALUE self, VALUE kind, VALUE message) {
  require_call();
  current->fail(current->host, StringValueCStr(kind), StringValueCStr(message));
  return Qnil;
}

static VALUE initialize(VALUE ignored) {
  VALUE host = rb_define_module("TsugumiHost");
#define METHOD(name, function, count) rb_define_singleton_method(host, name, function, count)
  METHOD("info", host_info, 0);
  METHOD("inputs", host_inputs, 0);
  METHOD("params", host_params, 0);
  METHOD("output", host_output, 2);
  METHOD("display_text", host_text, 1);
  METHOD("display_markdown", host_markdown, 1);
  METHOD("display_scalar", host_scalar, 2);
  METHOD("display_module", host_module, 2);
  METHOD("display_image", host_image, 3);
  METHOD("display_table", host_table, 2);
  METHOD("display_line_chart", host_line_chart, 1);
  METHOD("display_points", host_points, 4);
  METHOD("log", host_log, 1);
  METHOD("cancelled?", host_cancelled, 0);
  METHOD("remaining_ms", host_remaining, 0);
  METHOD("fail", host_fail, 2);
#undef METHOD
  owner = rb_thread_current();
  rb_gc_register_address(&owner);
  rb_gc_register_address(&sdk);
  rb_ary_unshift(rb_gv_get("$LOAD_PATH"), string(TSUGUMI_RUBY_SDK));
  rb_require("rubygems");
  rb_require("tsugumi");
  sdk = rb_const_get(rb_cObject, rb_intern("Tsugumi"));
  return Qtrue;
}

static VALUE print_error(VALUE error) {
  VALUE message = rb_funcall(error, rb_intern("full_message"), 0);
  fprintf(stderr, "%s", StringValueCStr(message));
  return Qnil;
}

static int ensure_ruby(void) {
  if (ready) return 1;
  if (!dlopen(TSUGUMI_LIBRUBY, RTLD_NOW | RTLD_GLOBAL)) return 0;
  unsetenv("RUBYOPT");
  unsetenv("RUBYLIB");
  /* A Card with a lock sees exactly its locked bundle; any other Card sees
   * only the gems that come with this Ruby. */
  setenv("GEM_HOME", TSUGUMI_RUBY_GEMS, 1);
  setenv("GEM_PATH", TSUGUMI_RUBY_GEMS, 1);
  if (TSUGUMI_RUBY_BUNDLE[0]) {
    setenv("BUNDLE_GEMFILE", TSUGUMI_RUBY_BUNDLE "/Gemfile", 1);
    setenv("BUNDLE_PATH", TSUGUMI_RUBY_BUNDLE "/installed", 1);
    setenv("BUNDLE_APP_CONFIG", TSUGUMI_RUBY_BUNDLE "/no-config", 1);
    setenv("BUNDLE_FROZEN", "true", 1);
    setenv("BUNDLE_FORCE_RUBY_PLATFORM", "true", 1);
    setenv("BUNDLE_DISABLE_VERSION_CHECK", "true", 1);
    setenv("BUNDLE_VERSION", "system", 1); /* never fetch the lock's Bundler */
  }
  int argc = 1;
  char name[] = "tsugumi-ruby";
  char* argv_data[] = {name, NULL};
  char** argv = argv_data;
  ruby_sysinit(&argc, &argv);
  RUBY_INIT_STACK;
  if (ruby_setup() != 0) return 0;
  /* ruby_options installs MRI's builtin prelude (including Time and Kernel
   * methods used by RubyGems); ruby_exec_node runs an empty program without
   * ruby_cleanup, keeping the interpreter available for worker calls. */
  char option_name[] = "tsugumi-ruby", option_eval[] = "-e", option_code[] = "";
  char* options[] = {option_name, option_eval, option_code, NULL};
  void* program = ruby_options(3, options);
  int exit_status = 0;
  if (!ruby_executable_node(program, &exit_status) || ruby_exec_node(program) != 0) return 0;
  ruby_script("tsugumi-card");
  rb_enc_set_default_external(rb_enc_from_encoding(rb_utf8_encoding()));
  int state = 0;
  VALUE result = rb_protect(initialize, Qnil, &state);
  if (state || result == Qfalse) {
    if (state) {
      int ignored = 0;
      rb_protect(print_error, rb_errinfo(), &ignored);
    }
    rb_set_errinfo(Qnil);
    fprintf(stderr, "cannot load Ruby Card runtime or main.rb\n");
    return 0;
  }
  ready = 1;
  return 1;
}

static VALUE invoke(VALUE name) { return rb_funcall(sdk, SYM2ID(name), 0); }

static int call_ruby(const tsugumi_call* call, const char* method) {
  current = call;
  if (!ensure_ruby()) {
    if (call) call->fail(call->host, "worker_failed", "cannot initialize Ruby Card (see stderr)");
    current = NULL;
    return 1;
  }
  if (!method) {
    current = NULL;
    return 0;
  }
  int state = 0;
  VALUE result = rb_protect(invoke, ID2SYM(rb_intern(method)), &state);
  if (state) {
    rb_set_errinfo(Qnil);
    if (call) call->fail(call->host, "worker_failed", "Ruby host bridge raised an exception");
  }
  current = NULL;
  return state || result == Qfalse;
}

int tsugumi_prepare(void) { return call_ruby(NULL, NULL); }
static VALUE load_card(VALUE ignored) {
  if (TSUGUMI_RUBY_BUNDLE[0]) rb_require("bundler/setup");
  return rb_funcall(sdk, rb_intern("_load"), 1, string(TSUGUMI_RUBY_CARD));
}
int tsugumi_load(const tsugumi_call* call) {
  current = call;
  int state = 0;
  VALUE result = rb_protect(load_card, Qnil, &state);
  if (state) {
    int ignored = 0;
    rb_protect(print_error, rb_errinfo(), &ignored);
    rb_set_errinfo(Qnil);
  }
  current = NULL;
  return state || result == Qfalse;
}
int tsugumi_main(const tsugumi_call* call) { return call_ruby(call, "_invoke"); }
int tsugumi_run_start(const tsugumi_call* call) { return call_ruby(call, "_run_start"); }
