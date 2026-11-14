"""Exercise interpreter role dispatch and incremental results without sockets."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


def executable(name):
    value = shutil.which(os.environ.get("TSUGUMI_" + name.upper(), {"RSCRIPT": "Rscript"}.get(name, name.lower())))
    if not value:
        pytest.skip(name + " is not available")
    return value


def test_r_roles_hook_and_result_closure(tmp_path):
    rscript = executable("RSCRIPT")
    if subprocess.run([rscript, "--vanilla", "-e", 'quit(status=if (requireNamespace("jsonlite", quietly=TRUE)) 0 else 1)'], capture_output=True).returncode:
        pytest.skip("R jsonlite is not available")
    script = tmp_path / "roles.R"
    script.write_text(r'''
events <- list()
.Call <- function(name, ...) {
  args <- list(...)
  switch(name,
    tsugumi_params = '{"factor":2}',
    tsugumi_input = if (args[[1]] == "number") '{"value":21}' else NULL,
    tsugumi_info = c("test", "step", ".", ".", ".")[[args[[1]] + 1]],
    tsugumi_invocation = 7,
    tsugumi_cancelled = FALSE,
    tsugumi_remaining = -1,
    tsugumi_output = { events[[length(events)+1]] <<- args; TRUE },
    tsugumi_display = TRUE,
    tsugumi_finish_outputs = TRUE,
    tsugumi_log = NULL,
    tsugumi_fail = stop(args[[1]]),
    stop("unexpected host call: ", name))
}
source(commandArgs(TRUE)[[1]])
on_run_start <- function(inputs, report, run) {
  stopifnot(inputs$params$factor == 2, run$invocation == 7)
  report$log("start")
  list(hook = list(value = 1))
}
main <- function(inputs, report, run) {
  stopifnot(inputs$number$value == 21, !run$cancelled(), run$remaining_ms() == -1)
  stopifnot(inherits(try(inputs$params$factor <- 3, silent=TRUE), "try-error"))
  if (exists("saved_run", envir=.GlobalEnv)) {
    stopifnot(inherits(try(saved_run$cancelled(), silent=TRUE), "try-error"))
    stopifnot(inherits(try(saved_run$remaining_ms(), silent=TRUE), "try-error"))
  }
  saved_run <<- run
  report$text("ready")
  n <- 0
  function() {
    n <<- n + 1
    if (n == 1) list(result = list(value = 42)) else NULL
  }
}
stopifnot(.tsugumi_invoke(TRUE), .tsugumi_invoke(FALSE))
stopifnot(inherits(try(saved_run$cancelled(), silent=TRUE), "try-error"))
stopifnot(.tsugumi_invoke(FALSE))
stopifnot(events[[1]][[1]] == "hook", events[[2]][[1]] == "result")
stopifnot(jsonlite::fromJSON(events[[2]][[2]])$value == 42)
''')
    subprocess.run([rscript, "--vanilla", str(script), str(ROOT / "sdk/r/tsugumi.R")], check=True)


def test_ruby_roles_stream_and_callback_lifetime(tmp_path):
    script = tmp_path / "roles.rb"
    script.write_text(r'''
module TsugumiHost
  @events = []
  class << self
    attr_reader :events
    def inputs = [['number', 3, '{"value":21}']]
    def params = '{"factor":2}'
    def info = {'run_id'=>'test', 'card'=>'step', 'invocation'=>7,
                'working_directory'=>'.', 'temporary_directory'=>'.', 'output_directory'=>'.'}
    def output(port, value) = @events << [port, JSON.parse(value)]
    def finish_outputs; end
    def display_text(text); end
    def log(text); end
    def cancelled? = false
    def remaining_ms = -1
    def fail(kind, message) = raise(message)
  end
end
require ARGV[0]
class Tsugumi::Card
  def on_run_start(inputs, report, run)
    raise unless inputs.params['factor'] == 2 && run.invocation == 7
    {hook: {value: 1}}
  end
  def main(inputs, report, run)
    raise unless inputs.number['value'] == 21 && !run.cancelled? && run.remaining_ms == -1
    begin
      inputs.number['value'] = 0
      raise 'mutable inputs'
    rescue FrozenError
    end
    $saved_report = report
    Enumerator.new do |stream|
      report.text('ready')
      stream << {result: {value: 42}}
    end
  end
end
Tsugumi.instance_variable_set(:@card, Tsugumi::Card.new)
raise unless Tsugumi._run_start && Tsugumi._invoke
raise unless TsugumiHost.events == [['hook', {'value'=>1}], ['result', {'value'=>42}]]
begin
  $saved_report.log('late')
  raise 'callback outlived invocation'
rescue Tsugumi::Failure
end
''')
    subprocess.run([executable("RUBY"), str(script), str(ROOT / "sdk/ruby/tsugumi.rb")], check=True)


def test_ruby_unjoined_threads_fail_before_required_outputs(tmp_path):
    script = tmp_path / "threads.rb"
    script.write_text(r'''
require ARGV[0]
module TsugumiHost
  @events = []
  class << self
    attr_reader :events
    def info = {}
    def params = '{}'
    def inputs = []
    def finish_outputs
      @events << :finish
      raise ArgumentError, 'required outputs were not emitted'
    end
    def fail(kind, message) = @events << [kind, message]
  end
end
class Tsugumi::Card
  def main(inputs)
    $leftover = Thread.new { sleep 60 }
    nil
  end
end
Tsugumi.instance_variable_set(:@card, Tsugumi::Card.new)
raise if Tsugumi._invoke
raise unless TsugumiHost.events.length == 1
kind, message = TsugumiHost.events.first
raise unless kind == 'worker_failed' && message.include?('join Ruby threads')
raise if $leftover.alive?
# With no leftover thread, required-output validation still runs.
class Tsugumi::Card
  def main(inputs) = nil
end
TsugumiHost.events.clear
raise if Tsugumi._invoke
raise unless TsugumiHost.events.first == :finish
raise unless TsugumiHost.events.last[1].include?('required outputs')
''')
    subprocess.run([executable("RUBY"), str(script), str(ROOT / "sdk/ruby/tsugumi.rb")], check=True)


def test_julia_roles_hook_and_result_iterator(tmp_path):
    compiler = shutil.which(os.environ.get("CC", "cc"))
    if not compiler:
        pytest.skip("C compiler is not available")
    source = tmp_path / "host.c"
    source.write_text(r'''
#include <stdint.h>
#include <stdio.h>
const char* tsugumi_native_params(void) { return "{\"factor\":2}"; }
const char* tsugumi_native_input(const char* port) { (void)port; return "{\"value\":21}"; }
const char* tsugumi_native_info(int field) { (void)field; return "."; }
uint64_t tsugumi_native_invocation(void) { return 7; }
int tsugumi_native_output(const char* port, const char* json) { printf("%s:%s\n", port, json); return 0; }
int tsugumi_native_finish_outputs(void) { return 0; }
int tsugumi_native_display(const char* json) { (void)json; return 0; }
void tsugumi_native_log(const char* text) { (void)text; }
int tsugumi_native_cancelled(void) { return 0; }
int64_t tsugumi_native_remaining_ms(void) { return -1; }
void tsugumi_native_fail(const char* text) { fprintf(stderr, "%s\n", text); }
''')
    library = tmp_path / "host.so"
    subprocess.run([compiler, "-shared", "-fPIC", str(source), "-o", str(library)], check=True)
    script = tmp_path / "roles.jl"
    script.write_text(r'''
include(ARGS[1])
using .Tsugumi
function on_run_start(inputs, report, run)
    @assert inputs.params["factor"] == 2 && run.invocation == 7
    Dict("hook" => Dict("value" => 1))
end
function main(inputs, report, run)
    @assert inputs.number["value"] == 21 && !cancelled(run) && remaining_ms(run) == -1
    inputs.number["value"] = 0
    @assert inputs.number["value"] == 21
    if isdefined(Main, :saved_run)
        for method in (cancelled, remaining_ms)
            rejected = try; method(saved_run); false; catch; true; end
            @assert rejected
        end
    end
    global saved_run = run
    report.text("ready")
    (Dict("result" => Dict("value" => 42)) for _ in 1:1)
end
@assert Tsugumi.invoke(true) && Tsugumi.invoke(false)
for method in (cancelled, remaining_ms)
    rejected = try; method(saved_run); false; catch; true; end
    @assert rejected
end
@assert Tsugumi.invoke(false)
''')
    result = subprocess.run([executable("JULIA"), "--startup-file=no", "--history-file=no", str(script), str(ROOT / "sdk/julia/tsugumi.jl")],
                            env={**os.environ, "TSUGUMI_NATIVE_LIBRARY": str(library), "JULIA_DEPOT_PATH": str(tmp_path / "depot")},
                            text=True, capture_output=True, check=True)
    assert 'hook:{"value":1}' in result.stdout
    assert 'result:{"value":42}' in result.stdout
