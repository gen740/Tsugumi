// tsugumi-worker: runs one Card invocation at a time on behalf of tsugumid.
//
// fd 3: data socket (WorkerCall in, WorkerResult/WorkerFailure/WorkerDisplay out)
// fd 4: control socket (WorkerCancel)
// stdout/stderr: captured by the core as the invocation's logs.
//
// Steps per call: preRun -> the body (`run`) -> postRun. The body is either a
// package (--package: lib/main.so, loaded once when the worker starts and
// called in memory) or a bash script (--run). preRun / postRun (--pre /
// --post) are bash and get no Card inputs; the environment and working
// directory produced by preRun carry over. An earlier failure is never
// cleared by a later step's success. One deadline covers all steps, and
// cancel/deadline are checked before each step starts.
//
// A bash body gets, per input port P:
//   JSON_P          path of a JSON file with the message (any size)
//   FILE_P / DIR_P  the path carried by File / Directory messages
// and writes its outputs as {"port": {...}} JSON to $TSUGUMI_OUTPUT.
// postRun gets TSUGUMI_FAILED (0/1) and TSUGUMI_OUTPUTS (the ports sent).

#include <dlfcn.h>
#include <fcntl.h>
#include <flatbuffers/idl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/sdk/abi.h"
#include "schema.hh"

extern char** environ;

namespace W = Tsugumi::Worker;
namespace D = Tsugumi::Data;

namespace {

constexpr uint32_t kMaxFrame = 16u << 20;
constexpr int kData = 3;
constexpr int kControl = 4;

std::mutex g_write_mu;
std::atomic<bool> g_cancel{false};
std::atomic<pid_t> g_child{0};
uint64_t g_step = 0;

bool read_full(int fd, void* p, size_t n) {
  auto* c = static_cast<char*>(p);
  while (n > 0) {
    ssize_t r = ::read(fd, c, n);
    if (r == 0) return false;
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    c += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

bool write_full(int fd, const void* p, size_t n) {
  const auto* c = static_cast<const char*>(p);
  while (n > 0) {
    ssize_t w = ::write(fd, c, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    c += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

// Descriptors received with data frames (SCM_RIGHTS), in arrival order.
std::vector<int> g_received;

bool recv_full(int fd, void* p, size_t n) {
  auto* c = static_cast<char*>(p);
  while (n > 0) {
    iovec iov{c, n};
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * 8)];
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof ctrl;
    ssize_t r = ::recvmsg(fd, &msg, 0);
    if (r == 0) return false;
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    for (cmsghdr* h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h))
      if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS) {
        size_t k = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const int* fds = reinterpret_cast<const int*>(CMSG_DATA(h));
        g_received.insert(g_received.end(), fds, fds + k);
      }
    c += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

std::optional<std::string> read_frame(int fd) {
  uint32_t len;
  auto rd = fd == kData ? recv_full : read_full;
  if (!rd(fd, &len, 4)) return std::nullopt;
  if (len == 0 || len > kMaxFrame) return std::nullopt;
  std::string buf(len, '\0');
  if (!rd(fd, buf.data(), len)) return std::nullopt;
  return buf;
}

void send_frame(flatbuffers::FlatBufferBuilder& b) {
  std::lock_guard<std::mutex> l(g_write_mu);
  uint32_t len = b.GetSize();
  write_full(kData, &len, 4);
  write_full(kData, b.GetBufferPointer(), len);
}

std::string get_env(const std::vector<std::string>& env, const std::string& key) {
  std::string prefix = key + "=";
  for (const auto& e : env)
    if (e.starts_with(prefix)) return e.substr(prefix.size());
  return {};
}

std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
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
};

std::optional<D::FailureKind> kind_from_name(const std::string& n) {
  for (auto k : D::EnumValuesFailureKind())
    if (n == D::EnumNameFailureKind(k) && k != D::FailureKind::unspecified) return k;
  return std::nullopt;
}

struct Options {
  std::string bash;
  std::string package;          // a package body: <package>/lib/main.so
  std::string pre, run, post;   // files with bash text (run: a bash body)
};

struct Call {
  const W::WorkerCall* msg;
  uint64_t call_id;
  std::chrono::steady_clock::time_point deadline;
  bool has_deadline = false;
  std::vector<std::pair<std::string, std::string>> outputs;
  std::optional<Failure> failure;
  std::string card;
  std::string run_id;
};

Call* g_call = nullptr;

int64_t remaining_ms_impl(void*) {
  if (!g_call || !g_call->has_deadline) return -1;
  auto left = std::chrono::duration_cast<std::chrono::milliseconds>(g_call->deadline - std::chrono::steady_clock::now()).count();
  return left < 0 ? 0 : left;
}

void record_failure(D::FailureKind kind, std::string message) {
  if (g_call && !g_call->failure) g_call->failure = Failure{kind, std::move(message)};
}

int emit_output_impl(void*, const char* port, const uint8_t* data, size_t size) {
  if (!g_call) return 1;
  for (const auto& [name, _] : g_call->outputs)
    if (name == port) {
      record_failure(D::FailureKind::output_invalid, std::string("output ") + port + " emitted twice");
      return 1;
    }
  bool known = false;
  for (const auto* p : *g_call->msg->outputs()) known |= p->name()->str() == port;
  if (!known) {
    record_failure(D::FailureKind::output_invalid, std::string("unknown output port ") + port);
    return 1;
  }
  g_call->outputs.emplace_back(port, std::string(reinterpret_cast<const char*>(data), size));
  return 0;
}

void send_display(const std::function<std::pair<D::DisplayValue, flatbuffers::Offset<void>>(flatbuffers::FlatBufferBuilder&)>& f) {
  if (!g_call) return;
  flatbuffers::FlatBufferBuilder b;
  auto [type, value] = f(b);
  std::vector<flatbuffers::Offset<flatbuffers::String>> segs;
  std::string card = g_call->card;
  size_t s = 0;
  while (true) {
    size_t e = card.find('/', s);
    segs.push_back(b.CreateString(card.substr(s, e == std::string::npos ? std::string::npos : e - s)));
    if (e == std::string::npos) break;
    s = e + 1;
  }
  auto path = D::CreateCardPath(b, b.CreateVector(segs));
  auto run = b.CreateString(g_call->run_id);
  D::DisplayBuilder db(b);
  db.add_card(path);
  db.add_run_id(run);
  db.add_invocation(g_call->msg->invocation().value_or(0));
  db.add_revision(0);
  db.add_value_type(type);
  db.add_value(value);
  auto d = db.Finish();
  auto wd = W::CreateWorkerDisplay(b, d);
  W::WorkerMessageBuilder mb(b);
  mb.add_call_id(g_call->call_id);
  mb.add_value_type(W::WorkerPayload::WorkerDisplay);
  mb.add_value(wd.Union());
  W::FinishWorkerMessageBuffer(b, mb.Finish());
  send_frame(b);
}

int display_text_impl(void*, const char* text) {
  std::string t = text;
  send_display([&](flatbuffers::FlatBufferBuilder& b) {
    return std::make_pair(D::DisplayValue::DisplayText, D::CreateDisplayText(b, b.CreateString(t)).Union());
  });
  return 0;
}

int display_scalar_impl(void*, double value, const char* label) {
  std::string l = label;
  send_display([&](flatbuffers::FlatBufferBuilder& b) {
    auto ls = b.CreateString(l);
    D::DisplayScalarBuilder sb(b);
    sb.add_value(value);
    sb.add_label(ls);
    return std::make_pair(D::DisplayValue::DisplayScalar, sb.Finish().Union());
  });
  return 0;
}

int display_line_chart_impl(void*, const tsugumi_series* series, size_t count) {
  send_display([&](flatbuffers::FlatBufferBuilder& b) {
    std::vector<flatbuffers::Offset<D::LineSeries>> v;
    for (size_t i = 0; i < count; ++i)
      v.push_back(D::CreateLineSeries(b, b.CreateString(series[i].id), b.CreateString(series[i].label),
                                      b.CreateVector(series[i].x, series[i].count),
                                      b.CreateVector(series[i].y, series[i].count)));
    return std::make_pair(D::DisplayValue::DisplayLineChart, D::CreateDisplayLineChart(b, b.CreateVector(v)).Union());
  });
  return 0;
}

int display_table_impl(void*, const char* const* columns, size_t column_count, const char* const* cells,
                       size_t row_count) {
  send_display([&](flatbuffers::FlatBufferBuilder& b) {
    std::vector<std::string> cols(columns, columns + column_count);
    std::vector<flatbuffers::Offset<D::TableRow>> rows;
    for (size_t r = 0; r < row_count; ++r) {
      std::vector<std::string> row(cells + r * column_count, cells + (r + 1) * column_count);
      rows.push_back(D::CreateTableRow(b, b.CreateVectorOfStrings(row)));
    }
    auto c = b.CreateVectorOfStrings(cols);
    return std::make_pair(D::DisplayValue::DisplayTable, D::CreateDisplayTable(b, c, b.CreateVector(rows)).Union());
  });
  return 0;
}

int display_image_impl(void*, const char* media_type, const uint8_t* data, size_t size, const char* alt) {
  send_display([&](flatbuffers::FlatBufferBuilder& b) {
    auto mt = b.CreateString(media_type);
    auto d = b.CreateVector(data, size);
    auto a = b.CreateString(alt ? alt : "");
    return std::make_pair(D::DisplayValue::DisplayImage, D::CreateDisplayImage(b, mt, d, a).Union());
  });
  return 0;
}

int display_points_impl(void*, const float* x, const float* y, const float* z, const uint32_t* colors, size_t count) {
  send_display([&](flatbuffers::FlatBufferBuilder& b) {
    auto xv = b.CreateVector(x, count), yv = b.CreateVector(y, count), zv = b.CreateVector(z, count);
    auto cv = b.CreateVector(colors, colors ? count : 0);
    return std::make_pair(D::DisplayValue::DisplayPointCloud, D::CreateDisplayPointCloud(b, xv, yv, zv, cv).Union());
  });
  return 0;
}

int display_module_impl(void*, const char* name, const char* data_json) {
  send_display([&](flatbuffers::FlatBufferBuilder& b) {
    auto n = b.CreateString(name), d = b.CreateString(data_json);
    return std::make_pair(D::DisplayValue::DisplayModule, D::CreateDisplayModule(b, n, d).Union());
  });
  return 0;
}

void log_impl(void*, const char* message) {
  std::fprintf(stderr, "%s\n", message);
  std::fflush(stderr);
}

int cancelled_impl(void*) { return g_cancel.load() ? 1 : 0; }

void fail_impl(void*, const char* kind, const char* message) {
  auto k = kind_from_name(kind).value_or(D::FailureKind::worker_failed);
  record_failure(k, message ? message : "");
}

// Runs bash (argv after the interpreter); returns exit status. Kills the
// process group on cancel or deadline.
int run_shell(const std::string& bash, const std::vector<std::string>& args, const std::vector<std::string>& env,
              const std::string& cwd, Call& call, bool* timed_out) {
  std::vector<char*> envp;
  for (const auto& e : env) envp.push_back(const_cast<char*>(e.c_str()));
  envp.push_back(nullptr);
  std::vector<const char*> argv = {bash.c_str(), "-e", "-o", "pipefail"};
  for (const auto& a : args) argv.push_back(a.c_str());
  argv.push_back(nullptr);
  pid_t pid = ::fork();
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
    if (!term && (g_cancel.load() || (call.has_deadline && now >= call.deadline))) {
      if (!g_cancel.load()) *timed_out = true;
      ::kill(-pid, SIGTERM);
      term = true;
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

std::vector<std::string> parse_env_dump(const std::string& dump) {
  std::vector<std::string> out;
  size_t s = 0;
  while (s < dump.size()) {
    size_t e = dump.find('\0', s);
    if (e == std::string::npos) e = dump.size();
    std::string kv = dump.substr(s, e - s);
    if (kv.find('=') != std::string::npos && !kv.starts_with("TSUGUMI_ENV_DUMP=") &&
        !kv.starts_with("BASH_") && !kv.starts_with("SHLVL=") && !kv.starts_with("_="))
      out.push_back(kv);
    s = e + 1;
  }
  return out;
}

void set_env(std::vector<std::string>& env, const std::string& key, const std::string& value) {
  std::string prefix = key + "=";
  for (auto& e : env)
    if (e.starts_with(prefix)) {
      e = prefix + value;
      return;
    }
  env.push_back(prefix + value);
}

// Parsers by schema and root type; built only for bash bodies.
flatbuffers::Parser* parser_for(const flatbuffers::Vector<uint8_t>* bfbs, const std::string& root) {
  static std::map<std::string, std::unique_ptr<flatbuffers::Parser>> cache;
  std::string key = root + '\0' + std::string(reinterpret_cast<const char*>(bfbs->data()), bfbs->size());
  if (auto it = cache.find(key); it != cache.end()) return it->second.get();
  flatbuffers::IDLOptions o;
  o.strict_json = true;
  o.natural_utf8 = true;
  o.output_default_scalars_in_json = true;
  auto p = std::make_unique<flatbuffers::Parser>(o);
  p->known_attributes_["tsugumi"] = true;
  if (!p->Deserialize(bfbs->data(), bfbs->size()) || !p->SetRootType(root.c_str())) return nullptr;
  return (cache[key] = std::move(p)).get();
}

// The package body, loaded when the worker starts (a standby worker is ready
// before its first call). tsugumi_load / tsugumi_run_start are optional.
using load_fn = int (*)();
void* g_lib = nullptr;
tsugumi_main_fn g_main = nullptr;
tsugumi_main_fn g_run_start = nullptr;
std::string g_load_error;
std::string g_last_run;

void load_package(const Options& opt) {
  std::string lib = opt.package + "/lib/main.so";
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
  g_run_start = reinterpret_cast<tsugumi_main_fn>(::dlsym(g_lib, "tsugumi_run_start"));
  if (auto load = reinterpret_cast<load_fn>(::dlsym(g_lib, "tsugumi_load")); load && load() != 0)
    g_load_error = lib + ": tsugumi_load failed (see stderr)";
  std::fflush(stdout);
  std::fflush(stderr);
}

tsugumi_call make_call(const Call& call, std::vector<tsugumi_value>& values, const std::string& work,
                       const std::string& tmp, const std::string& out_dir) {
  const auto* m = call.msg;
  tsugumi_call c{};
  c.abi_version = TSUGUMI_ABI_VERSION;
  c.run_id = call.run_id.c_str();
  c.card = call.card.c_str();
  c.invocation = m->invocation().value_or(0);
  c.inputs = values.data();
  c.input_count = values.size();
  c.params = m->params()->bytes()->data();
  c.params_size = m->params()->bytes()->size();
  c.working_directory = work.c_str();
  c.temporary_directory = tmp.c_str();
  c.output_directory = out_dir.c_str();
  c.emit_output = emit_output_impl;
  c.display_text = display_text_impl;
  c.display_scalar = display_scalar_impl;
  c.display_line_chart = display_line_chart_impl;
  c.log = log_impl;
  c.cancelled = cancelled_impl;
  c.remaining_ms = remaining_ms_impl;
  c.fail = fail_impl;
  c.display_table = display_table_impl;
  c.display_image = display_image_impl;
  c.display_points = display_points_impl;
  c.display_module = display_module_impl;
  return c;
}

// Replaces the process environment with the step environment.
void apply_env(const std::vector<std::string>& env) {
  std::vector<std::string> names;
  for (char** e = environ; *e; ++e) {
    std::string kv = *e;
    names.push_back(kv.substr(0, kv.find('=')));
  }
  for (const auto& n : names) ::unsetenv(n.c_str());
  for (const auto& e : env) {
    auto eq = e.find('=');
    ::setenv(e.substr(0, eq).c_str(), e.substr(eq + 1).c_str(), 1);
  }
}

// Inputs of a bash body: JSON files and File / Directory paths.
void bash_inputs(const W::WorkerCall* m, const std::string& tmp, std::vector<std::string>& env) {
  namespace sc = tsugumi::schema;
  for (const auto* in : *m->inputs()) {
    std::string port = in->name()->str();
    std::string up = upper(port);
    auto* p = parser_for(in->value()->schema_bfbs(), in->value()->root_type()->str());
    std::string json;
    if (!p || flatbuffers::GenText(*p, in->value()->bytes()->data(), &json)) {
      record_failure(D::FailureKind::worker_failed, "cannot convert input " + port + " to JSON");
      return;
    }
    std::string json_path = tmp + "/input-" + port + ".json";
    write_text(json_path, json);
    set_env(env, "JSON_" + up, json_path);
    if (in->kind() == D::PortKind::File || in->kind() == D::PortKind::Directory) {
      // The message's string `value` is the path inside this environment.
      sc::v::Mapping<sc::v::String, sc::Json> fields;
      if (!sc::decode(json, fields))
        for (const auto& [k, value] : fields.value().entries()) {
          sc::v::String path;
          if (k == "value" && !sc::decode(value, path))
            set_env(env, (in->kind() == D::PortKind::File ? "FILE_" : "DIR_") + up, path.value());
        }
    }
  }
}

// {"port": {...message JSON...}, ...} written by a bash body -> one FlatBuffer per port.
void bash_outputs(const W::WorkerCall* m, const std::string& out_json) {
  namespace sc = tsugumi::schema;
  if (::access(out_json.c_str(), R_OK) != 0) return;
  sc::v::Mapping<sc::v::String, sc::Json> doc;
  if (auto err = sc::decode(read_text(out_json), doc)) {
    record_failure(D::FailureKind::output_invalid, "$TSUGUMI_OUTPUT is not a JSON object of ports: " + err->message);
    return;
  }
  for (const auto& [name, value] : doc.value().entries()) {
    const D::Port* port = nullptr;
    for (const auto* p : *m->outputs())
      if (p->name()->str() == name) port = p;
    if (!port) {
      record_failure(D::FailureKind::output_invalid, "unknown output port " + name + " in $TSUGUMI_OUTPUT");
      return;
    }
    auto* p = parser_for(port->schema_bfbs(), port->root_type()->str());
    std::string obj = sc::text(value);
    if (!p || !p->ParseJson(obj.c_str())) {
      record_failure(D::FailureKind::output_invalid,
                     "output " + name + " JSON does not match its type" + (p ? ": " + p->error_ : ""));
      return;
    }
    emit_output_impl(nullptr, name.c_str(), p->builder_.GetBufferPointer(), p->builder_.GetSize());
  }
}

void execute(const Options& opt, Call& call) {
  const auto* m = call.msg;
  std::string work = m->working_directory()->str();
  std::string tmp = m->temporary_directory()->str();
  std::string out_dir = m->output_directory() ? m->output_directory()->str() : "";
  std::vector<std::string> env;
  for (const auto* e : *m->environment()) env.push_back(e->name()->str() + "=" + e->value()->str());
  set_env(env, "TSUGUMI_WORK_DIR", work);
  set_env(env, "TSUGUMI_TMP_DIR", tmp);
  if (!out_dir.empty()) set_env(env, "TSUGUMI_OUTPUT_DIR", out_dir);
  set_env(env, "TMPDIR", tmp);
  bool timed_out = false;
  std::string cwd = work;
  // Nothing starts after a cancel or once the single call deadline passed.
  auto may_start = [&](const char* step) {
    if (g_cancel.load()) {
      record_failure(D::FailureKind::cancelled, std::string(step) + " not started: cancelled");
      return false;
    }
    if (call.has_deadline && std::chrono::steady_clock::now() >= call.deadline) {
      record_failure(D::FailureKind::timeout, std::string(step) + " not started: the deadline passed");
      return false;
    }
    return true;
  };
  auto step_failed = [&](const char* step, int status) {
    if (g_cancel.load()) record_failure(D::FailureKind::cancelled, std::string(step) + " cancelled");
    else if (timed_out) record_failure(D::FailureKind::timeout, std::string(step) + " exceeded the deadline");
    else
      record_failure(D::FailureKind::worker_failed,
                     std::string(step) + " failed with " +
                         (WIFEXITED(status) ? "exit status " + std::to_string(WEXITSTATUS(status))
                                            : "signal " + std::to_string(WTERMSIG(status))));
  };
  if (!opt.pre.empty() && may_start("preRun")) {
    std::string dump = tmp + "/pre-env";
    auto pre_env = env;
    set_env(pre_env, "TSUGUMI_ENV_DUMP", dump);
    // Sourced, so that its exports and `cd` can be read back afterwards.
    int st = run_shell(opt.bash, {"-c", ". \"$0\"; exec \"$TSUGUMI_WORKER\" --dump-env \"$TSUGUMI_ENV_DUMP\"", opt.pre},
                       pre_env, cwd, call, &timed_out);
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
      step_failed("preRun", st);
    } else {
      auto next = parse_env_dump(read_text(dump));
      if (!next.empty()) env = next;
      // A `cd` in preRun carries over (bash keeps PWD current).
      if (auto pwd = get_env(env, "PWD"); !pwd.empty()) cwd = pwd;
    }
  }
  if (!call.failure && !opt.package.empty() && may_start("run")) {
    if (!g_load_error.empty()) {
      record_failure(D::FailureKind::worker_failed, g_load_error);
    } else {
      std::vector<tsugumi_value> values;
      for (const auto* in : *m->inputs())
        values.push_back({in->name()->c_str(), static_cast<uint32_t>(in->kind()), in->value()->bytes()->data(),
                          in->value()->bytes()->size()});
      apply_env(env);
      if (::chdir(cwd.c_str()) != 0) record_failure(D::FailureKind::worker_failed, "cannot enter working directory " + cwd);
      tsugumi_call c = make_call(call, values, work, tmp, out_dir);
      // The first invocation of each run starts it (onRunStart / on_run_start).
      if (!call.failure && g_run_start && call.run_id != g_last_run) {
        g_last_run = call.run_id;
        if (g_run_start(&c) != 0) record_failure(D::FailureKind::worker_failed, "onRunStart failed");
      }
      g_last_run = call.run_id;
      if (!call.failure) {
        int rc = g_main(&c);
        std::fflush(stdout);
        std::fflush(stderr);
        if (rc != 0) record_failure(g_cancel.load() ? D::FailureKind::cancelled : D::FailureKind::worker_failed,
                                    "main.so returned " + std::to_string(rc));
      }
    }
  }
  if (!call.failure && !opt.run.empty() && may_start("run")) {
    auto run_env = env;
    bash_inputs(m, tmp, run_env);
    std::string out_json = tmp + "/output.json";
    ::unlink(out_json.c_str());
    set_env(run_env, "TSUGUMI_OUTPUT", out_json);
    if (!call.failure) {
      int st = run_shell(opt.bash, {opt.run}, run_env, cwd, call, &timed_out);
      if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) step_failed("run", st);
      else bash_outputs(m, out_json);
    }
  }
  // postRun also runs after a failed step (cleanup), but never after a
  // cancel or the deadline; its success never clears an earlier failure.
  bool stopped = call.failure && (call.failure->kind == D::FailureKind::cancelled || call.failure->kind == D::FailureKind::timeout);
  if (!opt.post.empty() && !stopped && may_start("postRun")) {
    bool post_timeout = false;
    auto post_env = env;
    set_env(post_env, "TSUGUMI_FAILED", call.failure ? "1" : "0");
    std::string sent;
    for (const auto& [name, _] : call.outputs) sent += (sent.empty() ? "" : " ") + name;
    set_env(post_env, "TSUGUMI_OUTPUTS", sent);
    int st = run_shell(opt.bash, {opt.post}, post_env, cwd, call, &post_timeout);
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
      timed_out = post_timeout;
      step_failed("postRun", st);
    }
  }
}

void reply(const Call& call) {
  flatbuffers::FlatBufferBuilder b;
  W::WorkerPayload type;
  flatbuffers::Offset<void> value;
  if (call.failure) {
    auto diag = b.CreateString(call.failure->message);
    auto args = b.CreateVector(std::vector<flatbuffers::Offset<D::KeyValue>>{});
    D::FailureBuilder fb(b);
    fb.add_kind(call.failure->kind);
    fb.add_args(args);
    fb.add_diagnostic(diag);
    auto f = fb.Finish();
    value = W::CreateWorkerFailure(b, f).Union();
    type = W::WorkerPayload::WorkerFailure;
  } else {
    std::vector<flatbuffers::Offset<W::WorkerOutput>> outs;
    for (const auto& [name, bytes] : call.outputs)
      outs.push_back(W::CreateWorkerOutput(b, b.CreateString(name),
                                           b.CreateVector(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())));
    value = W::CreateWorkerResult(b, b.CreateVector(outs)).Union();
    type = W::WorkerPayload::WorkerResult;
  }
  W::WorkerMessageBuilder mb(b);
  mb.add_call_id(call.call_id);
  mb.add_value_type(type);
  mb.add_value(value);
  W::FinishWorkerMessageBuffer(b, mb.Finish());
  send_frame(b);
}

void control_loop() {
  while (true) {
    auto frame = read_frame(kControl);
    if (!frame) return;
    flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(frame->data()), frame->size());
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
  if (argc == 3 && std::string(argv[1]) == "--dump-env") {
    // Used after preRun to carry its environment into the next steps.
    std::string out;
    for (char** e = environ; *e; ++e) {
      out += *e;
      out.push_back('\0');
    }
    write_text(argv[2], out);
    return 0;
  }
  Options opt;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    if (k == "--bash") opt.bash = argv[i + 1];
    else if (k == "--package") opt.package = argv[i + 1];
    else if (k == "--pre") opt.pre = argv[i + 1];
    else if (k == "--run") opt.run = argv[i + 1];
    else if (k == "--post") opt.post = argv[i + 1];
  }
  if (!opt.package.empty()) load_package(opt);
  std::thread(control_loop).detach();
  uint64_t last_call = 0;
  while (true) {
    auto frame = read_frame(kData);
    if (!frame) return 0;
    flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(frame->data()), frame->size());
    if (!W::VerifyWorkerMessageBuffer(v)) return 2;
    const auto* m = W::GetWorkerMessage(frame->data());
    if (m->value_type() != W::WorkerPayload::WorkerCall || !m->call_id() || *m->call_id() <= last_call) return 2;
    const auto* wc = m->value_as_WorkerCall();
    Call call;
    call.msg = wc;
    call.call_id = *m->call_id();
    last_call = call.call_id;
    call.run_id = wc->run_id()->str();
    for (const auto* s : *wc->card()->segments()) call.card += (call.card.empty() ? "" : "/") + s->str();
    if (wc->deadline_ms()) {
      call.has_deadline = true;
      call.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(*wc->deadline_ms());
    }
    g_cancel = false;
    g_call = &call;
    // Folders passed as descriptors are installed where the call says (the
    // directories then read /proc/self/fd/<n>); they are closed afterwards.
    std::vector<int> installed;
    size_t want = wc->passed_fds() ? wc->passed_fds()->size() : 0;
    if (want != g_received.size()) {
      record_failure(D::FailureKind::worker_failed, "the call's folder descriptors did not arrive");
    } else {
      for (size_t i = 0; i < want; ++i) {
        int slot = wc->passed_fds()->Get(i);
        if (::fcntl(slot, F_GETFD) != -1 || ::dup2(g_received[i], slot) != slot) {
          record_failure(D::FailureKind::worker_failed, "descriptor " + std::to_string(slot) + " is not free");
          break;
        }
        installed.push_back(slot);
      }
    }
    for (int f : g_received) ::close(f);
    g_received.clear();
    if (!call.failure) execute(opt, call);
    for (int f : installed) ::close(f);
    std::fflush(stdout);
    std::fflush(stderr);
    reply(call);
    g_call = nullptr;
  }
}
