// tsugumi-worker: runs one Card invocation at a time on behalf of tsugumid.
//
// fd 3: data socket (WorkerCall in, WorkerResult/WorkerFailure/WorkerDisplay out)
// fd 4: control socket (WorkerCancel)
// stdout/stderr: captured by the core as the invocation's logs.
//
// Steps per call: preRun -> the body (`run`) -> postRun. The body is either a
// package (--package: native lib/main.so or lib/tsugumi-runtime naming an SDK
// adapter; Card code loads inside its first invocation) or a bash script (--run).
// preRun / postRun (--pre / --post) are bash and get no Card inputs; the environment and working
// directory produced by preRun carry over. An earlier failure is never
// cleared by a later step's success. One deadline covers all steps, and
// cancel/deadline are checked before each step starts.
//
// The environment of a package body, weakest first: what the worker was
// started with, the call's, what the package set while loading (the top of a
// Python Card's main.py), preRun's exports. The first preRun also controls
// source loading. Package libraries and runtime initialization are invocation work.
//
// A bash body gets, per input port P:
//   JSON_P          path of a JSON file with the message (any size)
//   FILE_P / DIR_P  the path carried by File / Directory messages
// and writes its outputs as {"port": {...}} JSON to $TSUGUMI_OUTPUT and what it
// shows as JSON to $TSUGUMI_REPORT (read when it ends; see BashDisplay).
// postRun gets TSUGUMI_FAILED (0/1) and TSUGUMI_OUTPUTS (the ports sent).

#include <cmath>
#include <dlfcn.h>
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
#include "tsugumi/message_json.hh"
#include "schema.hh"
#include "tsugumi/workerd/frame.hh"
#include "tsugumi/workerd/display.hh"

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

std::optional<std::string> read_frame(int fd) {
  std::array<char, 4> prefix{};
  if (!read_full(fd, prefix.data(), prefix.size())) return std::nullopt;
  const auto len = tsugumi::workerd::decode_length(prefix.data());
  if (len == 0 || len > kMaxFrame) return std::nullopt;
  std::string buf(len, '\0');
  if (!read_full(fd, buf.data(), len)) return std::nullopt;
  return buf;
}

void send_frame(flatbuffers::FlatBufferBuilder& b) {
  if (b.GetSize() > kMaxFrame) throw std::runtime_error("worker frame exceeds 16 MiB");
  std::lock_guard<std::mutex> l(g_write_mu);
  const auto prefix = tsugumi::workerd::encode_length(b.GetSize());
  write_full(kData, prefix.data(), prefix.size());
  write_full(kData, b.GetBufferPointer(), b.GetSize());
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
  std::string package;  // native main.so or lib/tsugumi-runtime naming the SDK artifact
  std::string pre;      // a file with bash text
  std::string run;      // a file with bash text (a bash body)
  std::string post;     // a file with bash text
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
  std::map<std::string, std::string> json_values;
  std::string display_error;
};

Call* g_call = nullptr;

int64_t remaining_ms_impl(void*) {
  if (!g_call || !g_call->has_deadline) return -1;
  auto left = std::chrono::duration_cast<std::chrono::milliseconds>(g_call->deadline - std::chrono::steady_clock::now()).count();
  return left < 0 ? 0 : left;
}

void record_failure(D::FailureKind kind, std::string message) {
  if (g_call && !g_call->failure) g_call->failure = Failure{kind, message.substr(0, 4096)};
}

// Build the actual result envelope when budgeting: port names, alignment and
// FlatBuffers metadata count towards the same 16 MiB limit as message bytes.
auto result_frame(const Call& call) -> flatbuffers::FlatBufferBuilder {
  flatbuffers::FlatBufferBuilder b;
  std::vector<flatbuffers::Offset<W::WorkerOutput>> outputs;
  for (const auto& [name, bytes] : call.outputs)
    outputs.push_back(W::CreateWorkerOutput(b, b.CreateString(name),
        b.CreateVector(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())));
  auto result = W::CreateWorkerResult(b, b.CreateVector(outputs));
  W::FinishWorkerMessageBuffer(b, W::CreateWorkerMessage(b, call.call_id, W::WorkerPayload::WorkerResult, result.Union()));
  return b;
}

int emit_output_impl(void*, const char* port, const uint8_t* data, size_t size) {
  if (!g_call) return 1;
  const auto invalid = [&](const std::string& reason) {
    record_failure(D::FailureKind::output_invalid, reason);
    return 1;
  };
  if (!port || !data || size > kMaxFrame) return invalid("output exceeds 16 MiB or has no data");
  for (const auto& [name, _] : g_call->outputs)
    if (name == port) return invalid(std::string("output ") + port + " emitted twice");
  const D::Port* declared = nullptr;
  for (const auto* p : *g_call->msg->outputs())
    if (p->name()->string_view() == port) declared = p;
  if (!declared) return invalid(std::string("unknown output port ") + port);
  if (declared->schema_bfbs() && declared->root_type()) {
    const auto* bfbs = declared->schema_bfbs();
    flatbuffers::Verifier verifier(bfbs->data(), bfbs->size());
    if (!reflection::VerifySchemaBuffer(verifier)) return invalid("invalid output schema");
    const auto* schema = reflection::GetSchema(bfbs->data());
    const auto* root = schema->objects()->LookupByKey(declared->root_type()->c_str());
    if (!root || !flatbuffers::Verify(*schema, *root, data, size))
      return invalid(std::string("output ") + port + " does not match its declared type");
  }
  g_call->outputs.emplace_back(port, std::string(reinterpret_cast<const char*>(data), size));
  if (result_frame(*g_call).GetSize() > kMaxFrame) {
    g_call->outputs.pop_back();
    return invalid("combined outputs exceed the 16 MiB result frame limit");
  }
  return 0;
}

void validate_required_outputs(Call& call) {
  if (call.failure) return;
  for (const auto* port : *call.msg->outputs()) {
    if (port->optional().value_or(false)) continue;
    if (std::none_of(call.outputs.begin(), call.outputs.end(), [&](const auto& output) {
          return output.first == port->name()->string_view();
        })) {
      record_failure(D::FailureKind::output_invalid, "required output " + port->name()->str() + " is missing");
      return;
    }
  }
}

int finish_outputs_impl(void*) {
  if (!g_call) return 1;
  validate_required_outputs(*g_call);
  return g_call->failure ? 1 : 0;
}

auto invalid_display(const std::string& message) -> int {
  if (g_call) g_call->display_error = message;
  return 1;
}

int send_display(const std::function<std::pair<D::DisplayValue, flatbuffers::Offset<void>>(flatbuffers::FlatBufferBuilder&)>& f) {
  if (!g_call) return 1;
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
  const auto* display = flatbuffers::GetTemporaryPointer(b, d);
  if (auto error = tsugumi::workerd::validate_display(*display)) { g_call->display_error = *error; return 1; }
  auto wd = W::CreateWorkerDisplay(b, d);
  W::WorkerMessageBuilder mb(b);
  mb.add_call_id(g_call->call_id);
  mb.add_value_type(W::WorkerPayload::WorkerDisplay);
  mb.add_value(wd.Union());
  W::FinishWorkerMessageBuffer(b, mb.Finish());
  if (b.GetSize() > kMaxFrame) { g_call->display_error = "display frame exceeds 16 MiB"; return 1; }
  send_frame(b);
  return 0;
}

int display_text_impl(void*, const char* text) {
  if (!text || std::strlen(text) > (1u << 20)) return invalid_display("display text exceeds 1 MiB or is missing");
  std::string t = text;
  return send_display([&](flatbuffers::FlatBufferBuilder& b) {
    return std::make_pair(D::DisplayValue::DisplayText, D::CreateDisplayText(b, b.CreateString(t)).Union());
  });
}

int display_markdown_impl(void*, const char* text) {
  if (!text || std::strlen(text) > (1u << 20)) return invalid_display("display text exceeds 1 MiB or is missing");
  std::string t = text;
  return send_display([&](flatbuffers::FlatBufferBuilder& b) {
    return std::make_pair(D::DisplayValue::DisplayText, D::CreateDisplayText(b, b.CreateString(t), true).Union());
  });
}

int display_scalar_impl(void*, double value, const char* label) {
  if (!label) return invalid_display("display scalar needs a label");
  std::string l = label;
  return send_display([&](flatbuffers::FlatBufferBuilder& b) {
    auto ls = b.CreateString(l);
    D::DisplayScalarBuilder sb(b);
    sb.add_value(value);
    sb.add_label(ls);
    return std::make_pair(D::DisplayValue::DisplayScalar, sb.Finish().Union());
  });
}

struct ChartSeriesConfig {
  tsugumi::schema::t::String<"id"> id;
  tsugumi::schema::t::String<"label"> label;
  tsugumi::schema::t::Uint64<"offset"> offset;
  tsugumi::schema::t::Uint64<"count"> count;
};
struct ChartConfig {
  tsugumi::schema::t::Array<"series", tsugumi::schema::Obj<ChartSeriesConfig>> series;
};

// The view module mapping of the Card (--views), set once when the worker starts.
std::string g_views;

auto display_module_impl(void*, const char*, const char*, const tsugumi_blob*, size_t) -> int;

// All language SDKs keep their line-chart API. Only the common worker encodes
// its immutable view identity and run data; coordinates never pass through JSON.
auto display_line_chart_impl(void*, const tsugumi_series* series, size_t count) -> int {
  const auto invalid = [](const char* message) {
    if (g_call) g_call->display_error = message;
    return -1;
  };
  if (count && !series) return invalid("missing line series");
  ChartConfig config;
  std::string x, y;
  size_t points = 0, bytes = 0;
  for (size_t i = 0; i < count; ++i) {
    const auto& row = series[i];
    if (!row.id || !row.label || (row.count && (!row.x || !row.y)) || row.count > 10000 - points)
      return invalid("invalid line series or more than 10,000 points");
    bytes += row.count * 16 + std::strlen(row.id) + std::strlen(row.label);
    if (bytes > (1u << 20)) return invalid("line chart exceeds 1 MiB");
    ChartSeriesConfig meta;
    *meta.id = row.id;
    *meta.label = row.label;
    *meta.offset = points;
    *meta.count = row.count;
    config.series->push_back(std::move(meta));
    for (size_t j = 0; j < row.count; ++j) {
      if (!std::isfinite(row.x[j]) || !std::isfinite(row.y[j])) return invalid("line coordinates must be finite");
      const double xv = flatbuffers::EndianScalar(row.x[j]);
      const double yv = flatbuffers::EndianScalar(row.y[j]);
      x.append(reinterpret_cast<const char*>(&xv), sizeof xv);
      y.append(reinterpret_cast<const char*>(&yv), sizeof yv);
    }
    points += row.count;
  }
  const auto json = tsugumi::schema::encode(config);
  const tsugumi_blob blobs[] = {{"x", reinterpret_cast<const uint8_t*>(x.data()), x.size()},
                                {"y", reinterpret_cast<const uint8_t*>(y.data()), y.size()}};
  const auto status = display_module_impl(nullptr, "line-chart", json.c_str(), blobs, 2);
  return status == 0 ? 0 : invalid("the built-in line-chart module is unavailable");
}

int display_table_impl(void*, const char* const* columns, size_t column_count, const char* const* cells,
                       size_t row_count) {
  if (column_count == 0 || column_count > 64 || row_count > 10000 || !columns || (row_count && !cells))
    return invalid_display("a table needs 1 to 64 columns and at most 10,000 rows");
  size_t bytes = 0;
  const auto fits = [&](const char* text) {
    if (!text) return false;
    const auto size = ::strnlen(text, 4097);
    bytes += size + 16;  // Include per-string metadata before allocating copies.
    return size <= 4096 && bytes <= kMaxFrame;
  };
  for (size_t i = 0; i < column_count; ++i) if (!fits(columns[i])) return invalid_display("table strings exceed 4 KiB or the display frame exceeds 16 MiB");
  for (size_t i = 0; i < column_count * row_count; ++i) if (!fits(cells[i])) return invalid_display("table strings exceed 4 KiB or the display frame exceeds 16 MiB");
  return send_display([&](flatbuffers::FlatBufferBuilder& b) {
    std::vector<std::string> cols(columns, columns + column_count);
    std::vector<flatbuffers::Offset<D::TableRow>> rows;
    for (size_t r = 0; r < row_count; ++r) {
      std::vector<std::string> row(cells + r * column_count, cells + (r + 1) * column_count);
      rows.push_back(D::CreateTableRow(b, b.CreateVectorOfStrings(row)));
    }
    auto c = b.CreateVectorOfStrings(cols);
    return std::make_pair(D::DisplayValue::DisplayTable, D::CreateDisplayTable(b, c, b.CreateVector(rows)).Union());
  });
}

int display_image_impl(void*, const char* media_type, const uint8_t* data, size_t size, const char* alt) {
  if (!media_type || !data || size > (8u << 20)) return invalid_display("an image exceeds 8 MiB or is missing");
  return send_display([&](flatbuffers::FlatBufferBuilder& b) {
    auto mt = b.CreateString(media_type);
    auto d = b.CreateVector(data, size);
    auto a = b.CreateString(alt ? alt : "");
    return std::make_pair(D::DisplayValue::DisplayImage, D::CreateDisplayImage(b, mt, d, a).Union());
  });
}

int display_points_impl(void*, const float* x, const float* y, const float* z, const uint32_t* colors, size_t count) {
  if (count > 1000000 || (count && (!x || !y || !z))) return invalid_display("a point cloud needs x, y and z, up to 1,000,000 points");
  return send_display([&](flatbuffers::FlatBufferBuilder& b) {
    auto xv = b.CreateVector(x, count);
    auto yv = b.CreateVector(y, count);
    auto zv = b.CreateVector(z, count);
    auto cv = b.CreateVector(colors, colors ? count : 0);
    return std::make_pair(D::DisplayValue::DisplayPointCloud, D::CreateDisplayPointCloud(b, xv, yv, zv, cv).Union());
  });
}

auto display_module_impl(void*, const char* name, const char* config_json,
                         const tsugumi_blob* blobs, size_t count) -> int {
  if (!name || !config_json || std::strlen(config_json) > (1u << 20) || count > 64 || (count && !blobs)) return -1;
  size_t bytes = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!blobs[i].name || std::strlen(blobs[i].name) > 128 || (!blobs[i].data && blobs[i].size) ||
        blobs[i].size > (8u << 20) - bytes) return -1;
    bytes += blobs[i].size;
  }
  std::string identity = name;
  if (!identity.starts_with("/nix/store/")) {
    if (g_views.empty()) return -1;
    tsugumi::schema::v::Mapping<tsugumi::schema::v::String, tsugumi::schema::v::String> views;
    if (tsugumi::schema::decode(g_views, views)) return -1;
    bool found = false;
    for (const auto& [key, value] : views.value())
      if (key == identity) { identity = value; found = true; break; }
    if (!found) return -1;
  }
  return send_display([&](flatbuffers::FlatBufferBuilder& b) {
    auto n = b.CreateString(identity);
    auto d = b.CreateString(config_json);
    std::vector<flatbuffers::Offset<D::DisplayBlob>> values;
    for (size_t i = 0; i < count; ++i)
      values.push_back(D::CreateDisplayBlob(b, b.CreateString(blobs[i].name),
          b.CreateVector(blobs[i].data, blobs[i].size)));
    return std::make_pair(D::DisplayValue::DisplayModule,
        D::CreateDisplayModule(b, n, d, b.CreateVector(values)).Union());
  });
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

// Parsers by schema and root type, shared by bash and in-memory language adapters.
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

auto input_json_impl(void*, const char* port) -> const char* {
  if (!g_call) return nullptr;
  if (auto it = g_call->json_values.find(port); it != g_call->json_values.end()) return it->second.c_str();
  for (const auto* in : *g_call->msg->inputs()) {
    if (in->name()->str() != port) continue;
    auto* parser = parser_for(in->value()->schema_bfbs(), in->value()->root_type()->str());
    std::string json;
    if (!parser || tsugumi::message_json::gen_language_text(*parser, in->value()->bytes()->data(), &json)) {
      record_failure(D::FailureKind::worker_failed, std::string("cannot decode input ") + port);
      return nullptr;
    }
    return g_call->json_values.emplace(port, std::move(json)).first->second.c_str();
  }
  return nullptr;
}

auto params_json_impl(void*) -> const char* {
  if (!g_call) return nullptr;
  // Port names cannot contain a NUL, so this cache key cannot alias an input.
  const std::string key(1, '\0');
  if (auto it = g_call->json_values.find(key); it != g_call->json_values.end()) return it->second.c_str();
  const auto* params = g_call->msg->params();
  auto* parser = parser_for(params->schema_bfbs(), params->root_type()->str());
  std::string json;
  if (!parser || tsugumi::message_json::gen_language_text(*parser, params->bytes()->data(), &json)) {
    record_failure(D::FailureKind::worker_failed, "cannot decode params");
    return nullptr;
  }
  return g_call->json_values.emplace(key, std::move(json)).first->second.c_str();
}

auto emit_json_impl(void*, const char* name, const char* json) -> int {
  if (!g_call) return 1;
  for (const auto* port : *g_call->msg->outputs()) {
    if (port->name()->str() != name) continue;
    auto* parser = parser_for(port->schema_bfbs(), port->root_type()->str());
    if (!parser || !tsugumi::message_json::parse_json(*parser, json)) {
      record_failure(D::FailureKind::output_invalid, std::string("output ") + name +
                     " does not match its type" + (parser ? ": " + parser->error_ : ""));
      return 1;
    }
    return emit_output_impl(nullptr, name, parser->builder_.GetBufferPointer(), parser->builder_.GetSize());
  }
  record_failure(D::FailureKind::output_invalid, std::string("unknown output port ") + name);
  return 1;
}

auto display_json_impl(void*, const char* json) -> int;

// Package metadata does not establish provenance. Libraries are opened
// only inside an invocation; tsugumi_prepare initializes the interpreter
// and tsugumi_load loads the Card after preRun.
using prepare_fn = int (*)();
void* g_lib = nullptr;
tsugumi_main_fn g_main = nullptr;
tsugumi_main_fn g_load = nullptr;
bool g_card_loaded = false;
tsugumi_main_fn g_run_start = nullptr;
std::string g_load_error;
std::string g_last_run;
std::vector<std::string> g_loaded_env;  // the process environment once the package was loaded
std::vector<std::string> g_set_by_load;  // NAME=value for sets; NAME for removals

std::vector<std::string> process_env() {
  std::vector<std::string> out;
  for (char** e = environ; *e; ++e) out.emplace_back(*e);
  return out;
}

auto env_value(const std::vector<std::string>& env, const std::string& name) -> std::optional<std::string> {
  for (const auto& e : env)
    if (e.starts_with(name + "=")) return e.substr(name.size() + 1);
  return std::nullopt;
}

auto remove_env(std::vector<std::string>& env, const std::string& name) -> void {
  std::erase_if(env, [&](const auto& e) { return e.starts_with(name + "="); });
}

// Save only loading's changes, including unset variables. Invocation identity
// and preRun's transient environment must not become the resident baseline.
auto remember_loaded_env(const std::vector<std::string>& before) -> void {
  const auto loaded = process_env();
  for (const auto& e : before) {
    const auto name = e.substr(0, e.find('='));
    if (!env_value(loaded, name)) {
      g_set_by_load.push_back(name);
      remove_env(g_loaded_env, name);
    }
  }
  for (const auto& e : loaded)
    if (std::find(before.begin(), before.end(), e) == before.end()) {
      g_set_by_load.push_back(e);
      const auto eq = e.find('=');
      set_env(g_loaded_env, e.substr(0, eq), e.substr(eq + 1));
    }
}

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
  g_run_start = reinterpret_cast<tsugumi_main_fn>(::dlsym(g_lib, "tsugumi_run_start"));
  g_load = reinterpret_cast<tsugumi_main_fn>(::dlsym(g_lib, "tsugumi_load"));
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
  c.display_markdown = display_markdown_impl;
  c.input_json = input_json_impl;
  c.params_json = params_json_impl;
  c.emit_json = emit_json_impl;
  c.display_json = display_json_impl;
  c.finish_outputs = finish_outputs_impl;
  return c;
}

// The environment of a package body (the order is at the top of this file):
// `step` is the call's environment with preRun's exports, `call` without them.
std::vector<std::string> body_env(const std::vector<std::string>& call, const std::vector<std::string>& step) {
  auto out = g_loaded_env;
  for (const auto& e : call) {
    const auto name = e.substr(0, e.find('='));
    if (!env_value(step, name)) remove_env(out, name);
  }
  for (const auto& e : step) {
    auto eq = e.find('=');
    set_env(out, e.substr(0, eq), e.substr(eq + 1));
  }
  for (const auto& e : g_set_by_load) {
    auto eq = e.find('=');
    std::string name = e.substr(0, eq);
    if (env_value(step, name) == env_value(call, name)) {
      if (eq == std::string::npos) remove_env(out, name);
      else set_env(out, name, e.substr(eq + 1));
    }
  }
  return out;
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
    if (!p || tsugumi::message_json::gen_text(*p, in->value()->bytes()->data(), &json)) {
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
    if (!p || !tsugumi::message_json::parse_json(*p, obj)) {
      record_failure(D::FailureKind::output_invalid,
                     "output " + name + " JSON does not match its type" + (p ? ": " + p->error_ : ""));
      return;
    }
    emit_output_impl(nullptr, name.c_str(), p->builder_.GetBufferPointer(), p->builder_.GetSize());
  }
}

// A display object from bash or an in-memory language adapter. A relative image
// path is resolved against the working directory.
struct BashDisplayTable {
  tsugumi::schema::Opt<"columns", tsugumi::schema::v::Array<tsugumi::schema::v::String>> columns;
  tsugumi::schema::Opt<"rows", tsugumi::schema::v::Array<tsugumi::schema::v::Array<tsugumi::schema::v::String>>> rows;
};
struct BashDisplayImage {
  tsugumi::schema::Opt<"path", tsugumi::schema::v::String> path;
  tsugumi::schema::Opt<"alt", tsugumi::schema::v::String> alt;
};
struct BashDisplayModule {
  tsugumi::schema::Opt<"module", tsugumi::schema::v::String> module;
  tsugumi::schema::Opt<"config", tsugumi::schema::Json> config;
  tsugumi::schema::Opt<"blobs", tsugumi::schema::v::Mapping<tsugumi::schema::v::String, tsugumi::schema::v::String>> blobs;
};
struct BashDisplay {
  tsugumi::schema::Opt<"text", tsugumi::schema::v::String> text;
  tsugumi::schema::Opt<"markdown", tsugumi::schema::v::String> markdown;
  tsugumi::schema::Opt<"table", tsugumi::schema::Obj<BashDisplayTable>> table;
  tsugumi::schema::Opt<"image", tsugumi::schema::Obj<BashDisplayImage>> image;
  tsugumi::schema::Opt<"module", tsugumi::schema::Obj<BashDisplayModule>> module;
};

auto json_display(const std::string& json, const std::string& cwd) -> int {
  namespace sc = tsugumi::schema;
  auto invalid = [](const std::string& why) {
    if (g_call) g_call->display_error = why;
    return 1;
  };
  BashDisplay d;
  if (auto err = sc::decode(json, d)) return invalid(err->message);
  if (const auto* t = sc::get(d.text)) {
    return display_text_impl(nullptr, t->c_str());
  } else if (const auto* m = sc::get(d.markdown)) {
    return display_markdown_impl(nullptr, m->c_str());
  } else if (const auto* table = sc::get(d.table)) {
    const auto* columns = sc::get(table->columns);
    if (!columns) return invalid("a table needs columns");
    std::vector<const char*> names, cells;
    for (const auto& c : *columns) names.push_back(c.c_str());
    size_t row_count = 0;
    if (const auto* rows = sc::get(table->rows))
      for (const auto& row : *rows) {
        if (row.size() != names.size()) return invalid("a table row has " + std::to_string(row.size()) + " cells, not " + std::to_string(names.size()));
        for (const auto& c : row) cells.push_back(c.c_str());
        ++row_count;
      }
    return display_table_impl(nullptr, names.data(), names.size(), cells.data(), row_count);
  } else if (const auto* image = sc::get(d.image)) {
    const auto* file = sc::get(image->path);
    if (!file) return invalid("an image needs a path");
    std::string full = file->starts_with("/") ? *file : cwd + "/" + *file;
    if (::access(full.c_str(), R_OK) != 0) return invalid("cannot read the image " + *file);
    std::string bytes = read_text(full);
    const char* media = bytes.starts_with("\x89PNG") ? "image/png" : bytes.starts_with("\xFF\xD8") ? "image/jpeg" : nullptr;
    if (!media) return invalid(*file + " is neither PNG nor JPEG");
    const auto* alt = sc::get(image->alt);
    return display_image_impl(nullptr, media, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), alt ? alt->c_str() : "");
  } else if (const auto* module = sc::get(d.module)) {
    const auto* name = sc::get(module->module);
    const auto* data = sc::get(module->config);
    if (!name || !data) return invalid("a module needs module and config");
    std::vector<std::pair<std::string, std::string>> owned;
    if (const auto* blobs = sc::get(module->blobs))
      for (const auto& [key, file] : *blobs) {
        const auto full = file.starts_with("/") ? file : cwd + "/" + file;
        if (::access(full.c_str(), R_OK) != 0) return invalid("cannot read blob " + file);
        owned.emplace_back(key, read_text(full));
      }
    std::vector<tsugumi_blob> blobs;
    for (const auto& [key, bytes] : owned)
      blobs.push_back({key.c_str(), reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
    if (display_module_impl(nullptr, name->c_str(), sc::text(*data).c_str(), blobs.data(), blobs.size()))
      return invalid("unknown view module; declare it in views");
  } else {
    return invalid("name one of text, markdown, table, image or module");
  }
  return 0;
}

auto bash_display(const std::string& path, const std::string& cwd) -> void {
  if (::access(path.c_str(), R_OK) == 0 && json_display(read_text(path), cwd))
    record_failure(D::FailureKind::output_invalid, "$TSUGUMI_REPORT: " + g_call->display_error);
}

auto display_json_impl(void*, const char* json) -> int {
  if (!g_call) return 1;
  char* cwd = ::getcwd(nullptr, 0);
  const int status = json_display(json, cwd ? cwd : ".");
  std::free(cwd);
  return status;
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
  const auto call_env = env;
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
    std::vector<tsugumi_value> values;
    for (const auto* in : *m->inputs())
      values.push_back({in->name()->c_str(), static_cast<uint32_t>(in->kind()), in->value()->bytes()->data(),
                        in->value()->bytes()->size()});
    apply_env(body_env(call_env, env));
    if (::chdir(cwd.c_str()) != 0) record_failure(D::FailureKind::worker_failed, "cannot enter working directory " + cwd);
    tsugumi_call c = make_call(call, values, work, tmp, out_dir);
    if (!call.failure && !g_card_loaded && g_load_error.empty() && may_start("source loading")) {
      if (!g_lib) {
        const auto descriptor = opt.package + "/lib/tsugumi-runtime";
        const bool adapter = ::access(descriptor.c_str(), F_OK) == 0;
        auto runtime = adapter ? read_text(descriptor) : std::string{};
        if (!runtime.empty() && runtime.back() == '\n') runtime.pop_back();
        // Runtime initialization is invocation work too: a package filename
        // does not prove SDK provenance. Preserve lifetime HOME for R/Julia
        // session caches while the Card itself gets this call's HOME.
        if (adapter) {
          auto initialization_env = process_env();
          if (auto home = env_value(g_loaded_env, "HOME")) set_env(initialization_env, "HOME", *home);
          apply_env(initialization_env);
        }
        const auto before = process_env();
        if (adapter && (runtime.empty() || runtime.front() != '/'))
          g_load_error = "the SDK runtime descriptor must name an absolute library path";
        else
          open_package(adapter ? runtime : opt.package + "/lib/main.so");
        if (adapter && g_load_error.empty()) {
          auto prepare = reinterpret_cast<prepare_fn>(::dlsym(g_lib, "tsugumi_prepare"));
          if (!prepare || prepare() != 0) g_load_error = "cannot initialize the language runtime (see stderr)";
        }
        remember_loaded_env(before);
        apply_env(body_env(call_env, env));
      }
      const auto before = process_env();
      if (g_load_error.empty() && g_load && g_load(&c) != 0)
        g_load_error = "tsugumi_load failed (see stderr)";
      remember_loaded_env(before);
      g_card_loaded = true;
      std::fflush(stdout);
      std::fflush(stderr);
    }
    if (g_card_loaded) apply_env(body_env(call_env, env));
    if (!g_load_error.empty()) record_failure(D::FailureKind::worker_failed, g_load_error);
    // The first invocation of each run starts it (onRunStart / on_run_start).
    if (!call.failure && may_start("run-start hook") && g_run_start && call.run_id != g_last_run) {
      g_last_run = call.run_id;
      if (g_run_start(&c) != 0) record_failure(D::FailureKind::worker_failed, "onRunStart failed");
    }
    g_last_run = call.run_id;
    if (!call.failure && may_start("main")) {
      int rc = g_main(&c);
      std::fflush(stdout);
      std::fflush(stderr);
      if (rc != 0) record_failure(g_cancel.load() ? D::FailureKind::cancelled : D::FailureKind::worker_failed,
                                  "main.so returned " + std::to_string(rc));
    }
  }
  if (!call.failure && !opt.run.empty() && may_start("run")) {
    auto run_env = env;
    bash_inputs(m, tmp, run_env);
    std::string out_json = tmp + "/output.json";
    ::unlink(out_json.c_str());
    set_env(run_env, "TSUGUMI_OUTPUT", out_json);
    std::string display_json = tmp + "/display.json";
    ::unlink(display_json.c_str());
    set_env(run_env, "TSUGUMI_REPORT", display_json);
    if (!call.failure) {
      int st = run_shell(opt.bash, {opt.run}, run_env, cwd, call, &timed_out);
      if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
        step_failed("run", st);
      } else {
        bash_display(display_json, cwd);
        if (!call.failure) bash_outputs(m, out_json);
      }
    }
  }
  validate_required_outputs(call);
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
  if (!call.failure) {
    auto b = result_frame(call);
    send_frame(b);
    return;
  }
  flatbuffers::FlatBufferBuilder b;
  auto diag = b.CreateString(call.failure->message);
  auto args = b.CreateVector(std::vector<flatbuffers::Offset<D::KeyValue>>{});
  auto failure = D::CreateFailure(b, call.failure->kind, args, diag);
  auto value = W::CreateWorkerFailure(b, failure);
  W::FinishWorkerMessageBuffer(b, W::CreateWorkerMessage(b, call.call_id, W::WorkerPayload::WorkerFailure, value.Union()));
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
    else if (k == "--views") g_views = argv[i + 1];
  }
  // Standby starts only the worker executable. No package-controlled code,
  // including constructors or an alleged SDK adapter, runs before a call.
  g_loaded_env = process_env();
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
    execute(opt, call);
    std::fflush(stdout);
    std::fflush(stderr);
    reply(call);
    g_call = nullptr;
  }
}
