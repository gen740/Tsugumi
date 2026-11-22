// tsugumi-worker: runs one Card invocation at a time on behalf of tsugumid.
//
// fd 3: data socket (WorkerCall in, WorkerResult/WorkerFailure/WorkerDisplay
// out) fd 4: control socket (WorkerCancel) stdout/stderr: captured by the core
// as the invocation's logs.
//
// Steps per call: preRun -> the body (`run`) -> postRun. The body is either a
// package (--package: lib/main.so; Card code loads inside its first invocation)
// or a bash script
// (--run). preRun / postRun (--pre / --post) are bash and get no Card inputs;
// the environment and working directory produced by preRun carry over. An
// earlier failure is never cleared by a later step's success. One deadline
// covers all steps, and cancel/deadline are checked before each step starts.
//
// The process environment and cwd are configured at launch. ABI 8 preRun
// overrides apply only to the current call.
// A bash body gets, per input port P:
//   JSON_P          path of a JSON file with the message (any size)
//   FILE_P / DIR_P  the path carried by File / Directory messages
// and writes its outputs as {"port": {...}} JSON to $TSUGUMI_OUTPUT.
// postRun gets TSUGUMI_FAILED (0/1) and TSUGUMI_OUTPUTS (the ports sent).

#include <dlfcn.h>
#include <fcntl.h>
#include <flatbuffers/idl.h>
#include <flatbuffers/reflection.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "tsugumi/message_json.hh"
#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/rpc/frame.hh"
#include "tsugumi/sdk/abi.h"
#include "tsugumi/util/flatbuffer_schema.hh"
#include "tsugumi/util/json.hh"
#include "tsugumi/workerd/frame.hh"

extern char** environ;

namespace W = Tsugumi::Worker;
namespace D = Tsugumi::Data;

namespace {

constexpr uint32_t kMaxFrame = tsugumi::workerd::kMaxWorkerFrameBytes;
constexpr int kData = 3;
constexpr int kControl = 4;

std::mutex g_write_mu;
std::atomic<bool> g_cancel{false};
std::atomic<pid_t> g_child{0};

namespace frame = tsugumi::rpc::frame;

// A bad or truncated frame ends the worker as a closed stream does.
std::optional<std::string> read_frame(int fd) {
  try {
    return frame::Read(fd, kMaxFrame);
  } catch (const frame::Error&) {
    return std::nullopt;
  }
}

// The core notices a broken connection on its side.
void send_frame(flatbuffers::FlatBufferBuilder& b) {
  std::lock_guard<std::mutex> l(g_write_mu);
  try {
    frame::Write(
        kData,
        {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()},
        kMaxFrame);
  } catch (const frame::Error&) {
  }
}

std::string get_env(const std::vector<std::string>& env,
                    const std::string& key) {
  std::string prefix = key + "=";
  for (const auto& e : env)
    if (e.starts_with(prefix)) return e.substr(prefix.size());
  return {};
}

std::string upper(std::string s) {
  for (auto& c : s)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string read_text(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void write_text(const std::string& path, const std::string& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

struct Failure {
  D::FailureKind kind;
  std::string message;
  std::vector<std::pair<std::string, std::string>> args = {};
};

std::optional<D::FailureKind> kind_from_name(const std::string& n) {
  for (auto k : D::EnumValuesFailureKind())
    if (n == D::EnumNameFailureKind(k) && k != D::FailureKind::unspecified)
      return k;
  return std::nullopt;
}

struct Options {
  std::string bash;
  std::string package;  // contains lib/main.so
  std::string pre;      // a file with bash text
  std::string run;      // a file with bash text (a bash body)
  std::string post;     // a file with bash text
  std::string work = ".";
  std::string tmp = ".";
  std::string out;
};

struct Call {
  const W::WorkerCall* msg;
  uint64_t call_id;
  std::chrono::steady_clock::time_point deadline;
  bool has_deadline = false;
  std::vector<std::pair<std::string, std::string>> outputs;
  std::optional<Failure> failure;
  size_t output_bytes = 0;
  uint32_t phase = TSUGUMI_PREPARE;
  std::optional<std::string> inputs_json;
  std::optional<std::string> params_json;
};

Call* g_call = nullptr;

int64_t remaining_ms_impl(void*) {
  if (!g_call || !g_call->has_deadline) return -1;
  // Rounded up: 0 must mean the deadline has passed, as the worker sees it.
  auto left = std::chrono::ceil<std::chrono::milliseconds>(
                  g_call->deadline - std::chrono::steady_clock::now())
                  .count();
  return left < 0 ? 0 : left;
}

void record_failure(D::FailureKind kind, std::string message) {
  if (g_call && !g_call->failure)
    g_call->failure = Failure{kind, message.substr(0, 4096)};
}

// Build the actual result envelope when budgeting: port names, alignment and
// FlatBuffers metadata count towards the same 16 MiB limit as message bytes.
auto result_frame(const Call& call) -> flatbuffers::FlatBufferBuilder {
  flatbuffers::FlatBufferBuilder b;
  std::vector<flatbuffers::Offset<W::WorkerOutput>> outputs;
  for (const auto& [name, bytes] : call.outputs)
    outputs.push_back(W::CreateWorkerOutput(
        b, b.CreateString(name),
        b.CreateVector(reinterpret_cast<const uint8_t*>(bytes.data()),
                       bytes.size())));
  auto result = W::CreateWorkerResult(b, b.CreateVector(outputs));
  W::FinishWorkerMessageBuffer(
      b, W::CreateWorkerMessage(b, call.call_id, W::WorkerPayload::WorkerResult,
                                result.Union()));
  return b;
}

auto append_output(std::string_view port, const uint8_t* data, size_t size)
    -> int {
  if (size > kMaxFrame - g_call->output_bytes) {
    record_failure(D::FailureKind::output_invalid,
                   "combined outputs exceed the 16 MiB result frame limit");
    return 1;
  }
  g_call->output_bytes += size;
  g_call->outputs.emplace_back(
      port, std::string(reinterpret_cast<const char*>(data), size));
  return 0;
}

void validate_required_outputs(Call& call) {
  if (call.failure) return;
  for (const auto* port : *call.msg->outputs()) {
    if (port->optional().value_or(false)) continue;
    if (std::none_of(call.outputs.begin(), call.outputs.end(),
                     [&](const auto& output) {
                       return output.first == port->name()->string_view();
                     })) {
      record_failure(D::FailureKind::output_invalid,
                     "required output " + port->name()->str() + " is missing");
      // The same argument as core's own check of the result gives.
      if (call.failure) call.failure->args = {{"port", port->name()->str()}};
      return;
    }
  }
}

void log_impl(void*, const char* message) {
  std::fprintf(stderr, "%s\n", message);
  std::fflush(stderr);
}

int cancelled_impl(void*) { return g_cancel.load() ? 1 : 0; }

auto fail_at_impl(void*, const char* kind, const char* message,
                  const char* source, uint32_t line, const char* cell) -> void {
  if (!g_call || g_call->failure) return;
  record_failure(kind_from_name(kind).value_or(D::FailureKind::worker_failed),
                 message ? message : "");
  if (source && *source && line > 0) {
    g_call->failure->args = {{"source", source},
                             {"line", std::to_string(line)}};
    if (cell && *cell) g_call->failure->args.emplace_back("cell", cell);
  }
}

void fail_impl(void*, const char* kind, const char* message) {
  auto k = kind_from_name(kind).value_or(D::FailureKind::worker_failed);
  record_failure(k, message ? message : "");
}

// Runs bash (argv after the interpreter); returns exit status. Kills the
// process group on cancel or deadline.
int run_shell(const std::string& bash, const std::vector<std::string>& args,
              const std::vector<std::string>& env, const std::string& cwd,
              Call& call, bool* timed_out) {
  std::vector<char*> envp;
  for (const auto& e : env) envp.push_back(const_cast<char*>(e.c_str()));
  envp.push_back(nullptr);
  std::vector<const char*> argv = {bash.c_str(), "-e", "-o", "pipefail"};
  for (const auto& a : args) argv.push_back(a.c_str());
  argv.push_back(nullptr);
  pid_t pid = ::fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    ::setpgid(0, 0);
    ::signal(SIGPIPE, SIG_DFL);
    if (::chdir(cwd.c_str()) != 0) ::_exit(126);
    ::close(kData);
    ::close(kControl);
    ::execve(bash.c_str(), const_cast<char**>(argv.data()), envp.data());
    ::_exit(127);
  }
  ::setpgid(pid, pid);
  g_child = pid;
  int status = 0;
  bool term = false;
  auto kill_at = std::chrono::steady_clock::time_point::max();
  while (true) {
    pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) break;
    auto now = std::chrono::steady_clock::now();
    if (!term &&
        (g_cancel.load() || (call.has_deadline && now >= call.deadline))) {
      if (!g_cancel.load()) *timed_out = true;
      ::kill(-pid, SIGTERM);
      term = true;
      // A cancelled shell step can ignore TERM and hold the worker.
      kill_at = now + std::chrono::seconds(1);
    } else if (term && now >= kill_at) {
      ::kill(-pid, SIGKILL);
    }
    ::usleep(2000);
  }
  ::kill(-pid, SIGKILL);  // no stray children after the step
  g_child = 0;
  return status;
}

// The pipe preRun's environment comes back through. A reader thread drains
// it while preRun runs, so a large environment never blocks the shell; once
// the step has ended, what it wrote is read and the pipe closed, even if a
// detached process still holds the write end.
class EnvDump {
 public:
  EnvDump() {
    int ends[2];
    if (::pipe(ends) != 0) return;
    reader_ = ends[0];
    writer_ = ends[1];
    ::fcntl(reader_, F_SETFD, FD_CLOEXEC);
    ::fcntl(reader_, F_SETFL, O_NONBLOCK);
    thread_ = std::thread([this] {
      for (;;) {
        pollfd ready{reader_, POLLIN, 0};
        ::poll(&ready, 1, 20);
        if (drain() || done_.load()) break;
      }
      drain();
    });
  }
  EnvDump(const EnvDump&) = delete;
  auto operator=(const EnvDump&) -> EnvDump& = delete;
  ~EnvDump() { finish(); }
  auto writer() const -> int { return writer_; }
  auto finish() -> std::string {
    if (writer_ >= 0) ::close(writer_);
    writer_ = -1;
    done_ = true;
    if (thread_.joinable()) thread_.join();
    if (reader_ >= 0) ::close(reader_);
    reader_ = -1;
    return std::move(data_);
  }

 private:
  // Reads what is available; true at end of file.
  auto drain() -> bool {
    char buffer[65536];
    for (;;) {
      const auto n = ::read(reader_, buffer, sizeof buffer);
      if (n > 0) {
        data_.append(buffer, static_cast<size_t>(n));
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      return n == 0;
    }
  }
  int reader_ = -1, writer_ = -1;
  std::atomic<bool> done_{false};
  std::string data_;
  std::thread thread_;
};

std::vector<std::string> parse_env_dump(const std::string& dump) {
  std::vector<std::string> out;
  size_t s = 0;
  while (s < dump.size()) {
    size_t e = dump.find('\0', s);
    if (e == std::string::npos) e = dump.size();
    std::string kv = dump.substr(s, e - s);
    if (kv.find('=') != std::string::npos &&
        !kv.starts_with("TSUGUMI_ENV_DUMP_FD=") && !kv.starts_with("BASH_") &&
        !kv.starts_with("SHLVL=") && !kv.starts_with("_="))
      out.push_back(kv);
    s = e + 1;
  }
  return out;
}

void set_env(std::vector<std::string>& env, const std::string& key,
             const std::string& value) {
  std::string prefix = key + "=";
  for (auto& e : env)
    if (e.starts_with(prefix)) {
      e = prefix + value;
      return;
    }
  env.push_back(prefix + value);
}

// Parsers by schema and root type, shared by bash and in-memory language
// adapters.
flatbuffers::Parser* parser_for(const flatbuffers::Vector<uint8_t>* bfbs,
                                const std::string& root) {
  static std::map<std::string, std::unique_ptr<flatbuffers::Parser>> cache;
  std::string key =
      root + '\0' +
      std::string(reinterpret_cast<const char*>(bfbs->data()), bfbs->size());
  if (auto it = cache.find(key); it != cache.end()) return it->second.get();
  flatbuffers::IDLOptions o;
  o.strict_json = true;
  o.natural_utf8 = true;
  o.output_default_scalars_in_json = true;
  auto p = std::make_unique<flatbuffers::Parser>(o);
  p->known_attributes_["tsugumi"] = true;
  if (!p->Deserialize(bfbs->data(), bfbs->size()) ||
      !p->SetRootType(root.c_str()))
    return nullptr;
  tsugumi::flatbuffer_schema::restore_reflection_fields(
      *p, *reflection::GetSchema(bfbs->data()), /*restore_deprecated=*/false);
  return (cache[key] = std::move(p)).get();
}

auto params_json_impl(void*) -> const char* {
  if (g_call->params_json) return g_call->params_json->c_str();
  const auto* params = g_call->msg->params();
  auto* parser = parser_for(params->schema_bfbs(), params->root_type()->str());
  std::string json;
  if (!parser || tsugumi::message_json::gen_language_text(
                     *parser, params->bytes()->data(), &json)) {
    record_failure(D::FailureKind::worker_failed, "cannot decode params");
    return nullptr;
  }
  return g_call->params_json.emplace(std::move(json)).c_str();
}

auto input_port(const char* name) -> const W::WorkerInput* {
  for (const auto* port : *g_call->msg->inputs())
    if (port->name()->string_view() == name) return port;
  return nullptr;
}

auto input_impl(void*, const char* name, size_t* size) -> const uint8_t* {
  const auto* port = input_port(name);
  *size = port ? port->value()->bytes()->size() : 0;
  return port ? port->value()->bytes()->data() : nullptr;
}

auto input_kind_impl(void*, const char* name) -> uint32_t {
  const auto* port = input_port(name);
  return port ? static_cast<uint32_t>(port->kind()) : 0;
}

auto inputs_json_impl(void*) -> const char* {
  if (g_call->inputs_json) return g_call->inputs_json->c_str();
  std::string json = "{";
  for (const auto* port : *g_call->msg->inputs()) {
    const auto* value = port->value();
    auto* parser = parser_for(value->schema_bfbs(), value->root_type()->str());
    std::string text;
    if (!parser || tsugumi::message_json::gen_language_text(
                       *parser, value->bytes()->data(), &text)) {
      record_failure(D::FailureKind::worker_failed,
                     "cannot decode input " + port->name()->str());
      return nullptr;
    }
    if (json.size() > 1) json += ',';
    json += tsugumi::schema::text(
        tsugumi::schema::string_value(port->name()->str()));
    json += ':';
    json += text;
  }
  json += '}';
  return g_call->inputs_json.emplace(std::move(json)).c_str();
}

auto writable_output(std::string_view name) -> const D::Port* {
  if (g_call->failure) return nullptr;
  if (g_call->phase != TSUGUMI_RUN) {
    record_failure(D::FailureKind::output_invalid,
                   "outputs may only be sent during run");
    return nullptr;
  }
  for (const auto* port : *g_call->msg->outputs()) {
    if (port->name()->string_view() != name) continue;
    if (std::ranges::none_of(g_call->outputs, [&](const auto& output) {
          return output.first == name;
        }))
      return port;
    record_failure(D::FailureKind::output_invalid,
                   "output " + std::string(name) + " emitted twice");
    return nullptr;
  }
  record_failure(D::FailureKind::output_invalid,
                 "unknown output port " + std::string(name));
  return nullptr;
}

auto output_impl(void*, const char* name, const uint8_t* data, size_t size)
    -> int {
  const auto* port = writable_output(name);
  if (!port) return 1;
  const auto* schema = reflection::GetSchema(port->schema_bfbs()->data());
  if (!data || size > kMaxFrame ||
      !flatbuffers::Verify(*schema, *schema->root_table(), data, size)) {
    record_failure(D::FailureKind::output_invalid,
                   "output " + std::string(name) + " does not match its type");
    return 1;
  }
  return append_output(name, data, size);
}

auto output_json(std::string_view name, std::string_view json) -> int {
  const auto* port = writable_output(name);
  if (!port) return 1;
  auto* parser = parser_for(port->schema_bfbs(), port->root_type()->str());
  if (!parser || !tsugumi::message_json::parse_json(*parser, json)) {
    record_failure(D::FailureKind::output_invalid,
                   "output " + std::string(name) +
                       " JSON does not match its type" +
                       (parser ? ": " + parser->error_ : ""));
    return 1;
  }
  return append_output(name, parser->builder_.GetBufferPointer(),
                       parser->builder_.GetSize());
}

auto output_json_impl(void*, const char* name, const char* json) -> int {
  return output_json(name, json);
}


// Package libraries and language state are initialized during the first call.
void* g_lib = nullptr;
tsugumi_main_fn g_main = nullptr;
bool g_card_loaded = false;
std::string g_load_error;
auto process_env() -> std::vector<std::string> {
  std::vector<std::string> out;
  for (char** e = environ; *e; ++e) out.emplace_back(*e);
  return out;
}

auto env_value(const std::vector<std::string>& env, const std::string& name)
    -> std::optional<std::string> {
  for (const auto& e : env)
    if (e.starts_with(name + "=")) return e.substr(name.size() + 1);
  return std::nullopt;
}

class StepEnvironment {
 public:
  using Value = std::optional<std::string>;

  auto set(const std::string& name, Value value) -> void {
    const char* previous = ::getenv(name.c_str());
    changes_.try_emplace(name, previous ? Value(previous) : std::nullopt,
                         std::move(value));
    apply(name, changes_.at(name).second);
  }

  auto adopt(const std::vector<std::string>& before,
             const std::vector<std::string>& after) -> void {
    for (const auto& entry : before) {
      const auto name = entry.substr(0, entry.find('='));
      if (!env_value(after, name) && ::getenv(name.c_str())) set(name, {});
    }
    for (const auto& entry : after) {
      const auto eq = entry.find('=');
      const auto name = entry.substr(0, eq);
      auto value = entry.substr(eq + 1);
      if (env_value(before, name) != value) set(name, std::move(value));
    }
  }

  auto reapply() const -> void {
    for (const auto& [name, values] : changes_) apply(name, values.second);
  }

  ~StepEnvironment() {
    for (auto& [name, values] : changes_) {
      apply(name, values.first);
      if (values.first) tsugumi::workerd::clear_secret(*values.first);
      if (values.second) tsugumi::workerd::clear_secret(*values.second);
    }
  }

 private:
  static auto apply(const std::string& name, const Value& value) -> void {
    if (value)
      ::setenv(name.c_str(), value->c_str(), 1);
    else
      ::unsetenv(name.c_str());
  }

  std::map<std::string, std::pair<Value, Value>> changes_;
};

class StepDirectory {
 public:
  auto enter(const std::string& path) -> bool {
    previous_ = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    return previous_ >= 0 && ::chdir(path.c_str()) == 0;
  }
  ~StepDirectory() {
    if (previous_ < 0) return;
    if (::fchdir(previous_) != 0) ::_exit(126);
    ::close(previous_);
  }

 private:
  int previous_ = -1;
};

void open_package(const std::string& lib) {
  g_lib = ::dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!g_lib) {
    g_load_error = std::string("cannot load ") + lib + ": " + ::dlerror();
    return;
  }
  g_main = reinterpret_cast<tsugumi_main_fn>(::dlsym(g_lib, "tsugumi_main"));
  if (!g_main) {
    g_load_error = lib + " does not export tsugumi_main";
    return;
  }
  std::fflush(stdout);
  std::fflush(stderr);
}

constexpr tsugumi_host kEffects{
    // The old effect has no ABI 9 payload. It is disabled during the cutover.
    .display = [](void*, uint32_t, const void*) { return 0; },
    .log = log_impl,
    .cancelled = cancelled_impl,
    .remaining_ms = remaining_ms_impl,
    .fail = fail_impl,
    .input = input_impl,
    .output = output_impl,
    .inputs_json = inputs_json_impl,
    .params_json = params_json_impl,
    .input_kind = input_kind_impl,
    .output_json = output_json_impl,
    .fail_at = fail_at_impl,
};

auto make_call(const Call& call, const std::string& work,
               const std::string& tmp, const std::string& out_dir)
    -> tsugumi_call {
  const auto* m = call.msg;
  return {
      .abi_version = TSUGUMI_ABI_VERSION,
      .phase = TSUGUMI_PREPARE,
      .run_id = "",
      .card = "",
      .invocation = 0,
      .params = m->params()->bytes()->data(),
      .params_size = m->params()->bytes()->size(),
      .working_directory = work.c_str(),
      .temporary_directory = tmp.c_str(),
      .output_directory = out_dir.c_str(),
      .host = nullptr,
      .effects = &kEffects,
  };
}

auto invoke(tsugumi_call& call, tsugumi_phase phase, const char* name) -> bool {
  call.phase = phase;
  g_call->phase = phase;
  const int rc = g_main(&call);
  if (rc != 0) {
    record_failure(g_cancel.load() ? D::FailureKind::cancelled
                                   : D::FailureKind::worker_failed,
                   std::string(name) + " returned " + std::to_string(rc));
    return false;
  }
  return !g_call->failure;
}

// Inputs of a bash body: JSON files and File / Directory paths.
void bash_inputs(const W::WorkerCall* m, const std::string& tmp,
                 std::vector<std::string>& env) {
  namespace sc = tsugumi::schema;
  for (const auto* in : *m->inputs()) {
    std::string port = in->name()->str();
    std::string up = upper(port);
    auto* p =
        parser_for(in->value()->schema_bfbs(), in->value()->root_type()->str());
    std::string json;
    if (!p || tsugumi::message_json::gen_text(*p, in->value()->bytes()->data(),
                                              &json)) {
      record_failure(D::FailureKind::worker_failed,
                     "cannot convert input " + port + " to JSON");
      return;
    }
    std::string json_path = tmp + "/input-" + port + ".json";
    write_text(json_path, json);
    set_env(env, "JSON_" + up, json_path);
    if (in->kind() == D::PortKind::File ||
        in->kind() == D::PortKind::Directory) {
      // The message's string `value` is the path inside this environment.
      sc::v::Mapping<sc::v::String, sc::Json> fields;
      if (!sc::decode(json, fields))
        for (const auto& [k, value] : fields.value().entries()) {
          sc::v::String path;
          if (k == "value" && !sc::decode(value, path))
            set_env(env,
                    (in->kind() == D::PortKind::File ? "FILE_" : "DIR_") + up,
                    path.value());
        }
    }
  }
}

// {"port": {...message JSON...}, ...} written by a bash body -> one FlatBuffer
// per port.
void bash_outputs(const std::string& out_json) {
  namespace sc = tsugumi::schema;
  if (::access(out_json.c_str(), R_OK) != 0) return;
  sc::v::Mapping<sc::v::String, sc::Json> doc;
  if (auto err = sc::decode(read_text(out_json), doc)) {
    record_failure(
        D::FailureKind::output_invalid,
        "$TSUGUMI_OUTPUT is not a JSON object of ports: " + err->message);
    return;
  }
  for (const auto& [name, value] : doc.value().entries())
    if (output_json(name, sc::text(value))) return;
}

auto finish_outputs(Call& call) -> flatbuffers::FlatBufferBuilder {
  if (call.failure) return flatbuffers::FlatBufferBuilder();
  auto result = result_frame(call);
  if (result.GetSize() > kMaxFrame)
    record_failure(D::FailureKind::output_invalid,
                   "combined outputs exceed the 16 MiB result frame limit");
  validate_required_outputs(call);
  return result;
}

void execute(const Options& opt, Call& call,
             flatbuffers::FlatBufferBuilder& result) {
  const auto* m = call.msg;
  // The outer WorkerMessage verifier only sees the message as bytes.
  for (const auto* input : *m->inputs()) {
    const auto* value = input->value();
    const auto* bfbs = value->schema_bfbs();
    flatbuffers::Verifier verifier(bfbs->data(), bfbs->size());
    if (reflection::VerifySchemaBuffer(verifier)) {
      const auto& schema = *reflection::GetSchema(bfbs->data());
      const auto* root =
          schema.objects()->LookupByKey(value->root_type()->c_str());
      const auto* bytes = value->bytes();
      if (root && !root->is_struct() &&
          flatbuffers::Verify(schema, *root, bytes->data(), bytes->size()))
        continue;
    }
    record_failure(
        D::FailureKind::type_mismatch,
        "input " + input->name()->str() + " does not match its type");
    return;
  }
  const auto& work = opt.work;
  const auto& tmp = opt.tmp;
  const bool has_file_outputs = std::ranges::any_of(
      *m->outputs(), [](const auto* port) {
        return port->kind() == D::PortKind::File ||
               port->kind() == D::PortKind::Directory;
      });
  const std::string out_dir = has_file_outputs ? opt.out : "";
  std::vector<std::string> env;
  if (!opt.pre.empty() || !opt.run.empty() || !opt.post.empty())
    env = process_env();
  StepEnvironment pre_overrides;
  StepDirectory pre_directory;
  bool timed_out = false;
  std::string cwd = work;
  // Nothing starts after a cancel or once the single call deadline passed.
  auto may_start = [&](const char* step) {
    if (g_cancel.load()) {
      record_failure(D::FailureKind::cancelled,
                     std::string(step) + " not started: cancelled");
      return false;
    }
    if (call.has_deadline &&
        std::chrono::steady_clock::now() >= call.deadline) {
      record_failure(D::FailureKind::timeout,
                     std::string(step) + " not started: the deadline passed");
      return false;
    }
    return true;
  };
  auto step_failed = [&](const char* step, int status) {
    if (g_cancel.load())
      record_failure(D::FailureKind::cancelled,
                     std::string(step) + " cancelled");
    else if (timed_out)
      record_failure(D::FailureKind::timeout,
                     std::string(step) + " exceeded the deadline");
    else
      record_failure(
          D::FailureKind::worker_failed,
          std::string(step) + " failed with " +
              (WIFEXITED(status)
                   ? "exit status " + std::to_string(WEXITSTATUS(status))
                   : "signal " + std::to_string(WTERMSIG(status))));
  };
  if (!opt.pre.empty() && may_start("preRun")) {
    // The environment comes back through a pipe, never a file: it holds the
    // call's Card secrets.
    EnvDump dump;
    auto pre_env = env;
    set_env(pre_env, "TSUGUMI_ENV_DUMP_FD", std::to_string(dump.writer()));
    // Sourced, so that its exports and `cd` can be read back afterwards.
    int st = run_shell(opt.bash,
                       {"-c",
                        ". \"$0\"; exec \"$TSUGUMI_WORKER\" --dump-env "
                        "\"$TSUGUMI_ENV_DUMP_FD\"",
                        opt.pre},
                       pre_env, cwd, call, &timed_out);
    auto dumped = dump.finish();
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
      step_failed("preRun", st);
    } else {
      auto next = parse_env_dump(dumped);
      if (!next.empty()) {
        if (!opt.package.empty()) pre_overrides.adopt(env, next);
        for (auto& entry : env) tsugumi::workerd::clear_secret(entry);
        env = std::move(next);
      }
      // A `cd` in preRun carries over (bash keeps PWD current).
      if (auto pwd = get_env(env, "PWD"); !pwd.empty()) cwd = pwd;
    }
    tsugumi::workerd::clear_secret(dumped);
    for (auto& entry : pre_env) tsugumi::workerd::clear_secret(entry);
  }
  if (!call.failure && !opt.package.empty() && may_start("run")) {
    if (cwd != work && !pre_directory.enter(cwd))
      record_failure(D::FailureKind::worker_failed,
                     "cannot enter working directory " + cwd);
    if (!call.failure) {
      auto c = make_call(call, work, tmp, out_dir);
      if (!g_card_loaded && g_load_error.empty() &&
          may_start("source loading")) {
        open_package(opt.package + "/lib/main.so");
        if (g_main) {
          if (!invoke(c, TSUGUMI_PREPARE, "language runtime initialization"))
            g_load_error =
                "cannot initialize the language runtime (see stderr)";
          pre_overrides.reapply();
          if (g_load_error.empty() &&
              !invoke(c, TSUGUMI_LOAD, "Card source loading"))
            g_load_error = "Card source loading failed (see stderr)";
          g_card_loaded = true;
          std::fflush(stdout);
          std::fflush(stderr);
        }
      }
      pre_overrides.reapply();
      if (!g_load_error.empty())
        record_failure(D::FailureKind::worker_failed, g_load_error);
      if (!call.failure && may_start("main")) {
        invoke(c, TSUGUMI_RUN, "main.so");
        std::fflush(stdout);
        std::fflush(stderr);
      }
    }
  }
  if (!call.failure && !opt.run.empty() && may_start("run")) {
    call.phase = TSUGUMI_RUN;
    auto run_env = env;
    bash_inputs(m, tmp, run_env);
    std::string out_json = tmp + "/output.json";
    ::unlink(out_json.c_str());
    set_env(run_env, "TSUGUMI_OUTPUT", out_json);
    if (!call.failure) {
      int st = run_shell(opt.bash, {opt.run}, run_env, cwd, call, &timed_out);
      if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
        step_failed("run", st);
      } else {
        if (!call.failure) bash_outputs(out_json);
      }
    }
    for (auto& entry : run_env) tsugumi::workerd::clear_secret(entry);
  }
  result = finish_outputs(call);
  // postRun also runs after a failed step (cleanup), but never after a
  // cancel or the deadline; its success never clears an earlier failure.
  bool stopped =
      call.failure && (call.failure->kind == D::FailureKind::cancelled ||
                       call.failure->kind == D::FailureKind::timeout);
  if (!opt.post.empty() && !stopped && may_start("postRun")) {
    bool post_timeout = false;
    auto post_env = env;
    set_env(post_env, "TSUGUMI_FAILED", call.failure ? "1" : "0");
    std::string sent;
    for (const auto& [name, _] : call.outputs)
      sent += (sent.empty() ? "" : " ") + name;
    set_env(post_env, "TSUGUMI_OUTPUTS", sent);
    int st =
        run_shell(opt.bash, {opt.post}, post_env, cwd, call, &post_timeout);
    for (auto& entry : post_env) tsugumi::workerd::clear_secret(entry);
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
      timed_out = post_timeout;
      step_failed("postRun", st);
    }
  }
  for (auto& entry : env) tsugumi::workerd::clear_secret(entry);
}

void reply(const Call& call, flatbuffers::FlatBufferBuilder& result) {
  if (!call.failure) {
    send_frame(result);
    return;
  }
  flatbuffers::FlatBufferBuilder b;
  auto diag = b.CreateString(call.failure->message);
  std::vector<flatbuffers::Offset<D::KeyValue>> entries;
  for (const auto& [key, value] : call.failure->args)
    entries.push_back(
        D::CreateKeyValue(b, b.CreateString(key), b.CreateString(value)));
  auto args = b.CreateVector(entries);
  auto failure = D::CreateFailure(b, call.failure->kind, args, diag);
  auto value = W::CreateWorkerFailure(b, failure);
  W::FinishWorkerMessageBuffer(
      b, W::CreateWorkerMessage(
             b, call.call_id, W::WorkerPayload::WorkerFailure, value.Union()));
  if (b.GetSize() > kMaxFrame)
    throw std::runtime_error("worker frame exceeds 16 MiB");
  send_frame(b);
}

void control_loop() {
  while (true) {
    auto frame = read_frame(kControl);
    if (!frame) return;
    flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(frame->data()),
                            frame->size());
    if (!W::VerifyWorkerMessageBuffer(v)) continue;
    const auto* m = W::GetWorkerMessage(frame->data());
    if (m->value_type() == W::WorkerPayload::WorkerCancel) {
      g_cancel = true;
      if (pid_t c = g_child.load()) ::kill(-c, SIGTERM);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  ::signal(SIGPIPE, SIG_IGN);
  ::signal(
      SIGTERM, +[](int number) {
        if (const pid_t child = g_child.load()) ::kill(-child, SIGKILL);
        ::_exit(128 + number);
      });
  if (argc == 3 && std::string(argv[1]) == "--dump-env") {
    // Used after preRun to carry its environment into the next steps,
    // written to the worker's pipe.
    std::string out;
    for (char** e = environ; *e; ++e) {
      out += *e;
      out.push_back('\0');
    }
    const int fd = std::atoi(argv[2]);
    for (size_t written = 0; written < out.size();) {
      const auto n = ::write(fd, out.data() + written, out.size() - written);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return 1;
      written += static_cast<size_t>(n);
    }
    tsugumi::workerd::clear_secret(out);
    return 0;
  }
  Options opt;
  if (const auto* path = ::getenv("TSUGUMI_WORK_DIR")) opt.work = path;
  opt.tmp = opt.work;
  if (const auto* path = ::getenv("TMPDIR")) opt.tmp = path;
  if (const auto* path = ::getenv("TSUGUMI_OUTPUT_DIR")) opt.out = path;
  if (!::getenv("TSUGUMI_WORKER")) ::setenv("TSUGUMI_WORKER", argv[0], 1);
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    if (k == "--bash")
      opt.bash = argv[i + 1];
    else if (k == "--package")
      opt.package = argv[i + 1];
    else if (k == "--pre")
      opt.pre = argv[i + 1];
    else if (k == "--run")
      opt.run = argv[i + 1];
    else if (k == "--post")
      opt.post = argv[i + 1];
  }
  // Standby starts only the worker executable. No package-controlled code,
  // including constructors or an alleged SDK adapter, runs before a call.
  std::thread(control_loop).detach();
  uint64_t last_call = 0;
  bool started = false;
  while (true) {
    auto frame = read_frame(kData);
    if (!frame) return 0;
    flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(frame->data()),
                            frame->size());
    if (!W::VerifyWorkerMessageBuffer(v)) return 2;
    const auto* m = W::GetWorkerMessage(frame->data());
    if (m->value_type() == W::WorkerPayload::WorkerStart && !started) {
      started = true;
      bool success = true;
      for (const auto* e : *m->value_as_WorkerStart()->environment())
        success &= ::setenv(e->name()->c_str(), e->value()->c_str(), 1) == 0;
      tsugumi::workerd::clear_secret(*frame);
      if (!success) return 2;
      continue;
    }
    started = true;
    if (m->value_type() != W::WorkerPayload::WorkerCall || !m->call_id() ||
        *m->call_id() <= last_call)
      return 2;
    const auto* wc = m->value_as_WorkerCall();
    Call call;
    call.msg = wc;
    call.call_id = *m->call_id();
    last_call = call.call_id;
    if (wc->deadline_ms()) {
      call.has_deadline = true;
      call.deadline = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(*wc->deadline_ms());
    }
    g_cancel = false;
    g_call = &call;
    flatbuffers::FlatBufferBuilder result;
    execute(opt, call, result);
    std::fflush(stdout);
    std::fflush(stderr);
    reply(call, result);
    g_call = nullptr;
  }
}
