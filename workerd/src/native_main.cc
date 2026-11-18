// tsugumi-worker: runs one Card invocation at a time on behalf of tsugumid.
//
// fd 3: data socket (WorkerCall in, WorkerResult/WorkerFailure/WorkerDisplay
// out) fd 4: control socket (WorkerCancel) stdout/stderr: captured by the core
// as the invocation's logs.
//
// Steps per call: preRun -> the body (`run`) -> postRun. The body is either a
// package (--package: lib/main.so; Card code loads inside its first invocation) or a bash script
// (--run). preRun / postRun (--pre / --post) are bash and get no Card inputs;
// the environment and working directory produced by preRun carry over. An
// earlier failure is never cleared by a later step's success. One deadline
// covers all steps, and cancel/deadline are checked before each step starts.
//
// The environment of a package body, weakest first: what the worker was
// started with, the call's, what the package set while loading (the top of a
// Python Card's main.py), preRun's exports. The first preRun also controls
// source loading. Package libraries and runtime initialization are invocation
// work.
//
// A bash body gets, per input port P:
//   JSON_P          path of a JSON file with the message (any size)
//   FILE_P / DIR_P  the path carried by File / Directory messages
// and writes its outputs as {"port": {...}} JSON to $TSUGUMI_OUTPUT and what it
// shows as JSON to $TSUGUMI_REPORT (read when it ends; see BashDisplay).
// postRun gets TSUGUMI_FAILED (0/1) and TSUGUMI_OUTPUTS (the ports sent).

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
#include <cmath>
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

#include "tsugumi/message_json.hh"
#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/sdk/abi.h"
#include "tsugumi/util/json.hh"
#include "tsugumi/workerd/display.hh"
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
  std::lock_guard<std::mutex> l(g_write_mu);
  const auto prefix = tsugumi::workerd::encode_length(b.GetSize());
  write_full(kData, prefix.data(), prefix.size());
  write_full(kData, b.GetBufferPointer(), b.GetSize());
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
  std::string pre;   // a file with bash text
  std::string run;   // a file with bash text (a bash body)
  std::string post;  // a file with bash text
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
  const uint8_t* inputs = nullptr;
  std::optional<std::string> inputs_json;
  std::optional<std::string> params_json;
  std::string display_error;
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
    -> void {
  g_call->outputs.emplace_back(
      port, std::string(reinterpret_cast<const char*>(data), size));
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

auto invalid_display(const std::string& message) -> int {
  if (g_call) g_call->display_error = message;
  return 1;
}

int send_display(
    const std::function<std::pair<D::DisplayValue, flatbuffers::Offset<void>>(
        flatbuffers::FlatBufferBuilder&)>& f) {
  if (!g_call) return 1;
  flatbuffers::FlatBufferBuilder b;
  auto [type, value] = f(b);
  std::vector<flatbuffers::Offset<flatbuffers::String>> segs;
  std::string card = g_call->card;
  size_t s = 0;
  while (true) {
    size_t e = card.find('/', s);
    segs.push_back(b.CreateString(
        card.substr(s, e == std::string::npos ? std::string::npos : e - s)));
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
  if (auto error = tsugumi::workerd::validate_display(*display)) {
    g_call->display_error = *error;
    return 1;
  }
  auto wd = W::CreateWorkerDisplay(b, d);
  W::WorkerMessageBuilder mb(b);
  mb.add_call_id(g_call->call_id);
  mb.add_value_type(W::WorkerPayload::WorkerDisplay);
  mb.add_value(wd.Union());
  W::FinishWorkerMessageBuffer(b, mb.Finish());
  if (b.GetSize() > kMaxFrame) {
    g_call->display_error = "display frame exceeds 16 MiB";
    return 1;
  }
  send_frame(b);
  return 0;
}

int display_text_impl(void*, const char* text) {
  if (!text || std::strlen(text) > (1u << 20))
    return invalid_display("display text exceeds 1 MiB or is missing");
  std::string t = text;
  return send_display([&](flatbuffers::FlatBufferBuilder& b) {
    return std::make_pair(D::DisplayValue::DisplayText,
                          D::CreateDisplayText(b, b.CreateString(t)).Union());
  });
}

int display_markdown_impl(void*, const char* text) {
  if (!text || std::strlen(text) > (1u << 20))
    return invalid_display("display text exceeds 1 MiB or is missing");
  std::string t = text;
  return send_display([&](flatbuffers::FlatBufferBuilder& b) {
    return std::make_pair(
        D::DisplayValue::DisplayText,
        D::CreateDisplayText(b, b.CreateString(t), true).Union());
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
  tsugumi::schema::t::Array<"series", tsugumi::schema::Obj<ChartSeriesConfig>>
      series;
};

// The view module mapping of the Card (--views), set once when the worker
// starts.
std::string g_views;

auto display_module_impl(void*, const char*, const char*, const tsugumi_blob*,
                         size_t) -> int;

// All language SDKs keep their line-chart API. Only the common worker encodes
// its immutable view identity and run data; coordinates never pass through
// JSON.
auto display_line_chart_impl(void*, const tsugumi_series* series, size_t count)
    -> int {
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
    if (!row.id || !row.label || (row.count && (!row.x || !row.y)) ||
        row.count > 10000 - points)
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
      if (!std::isfinite(row.x[j]) || !std::isfinite(row.y[j]))
        return invalid("line coordinates must be finite");
      const double xv = flatbuffers::EndianScalar(row.x[j]);
      const double yv = flatbuffers::EndianScalar(row.y[j]);
      x.append(reinterpret_cast<const char*>(&xv), sizeof xv);
      y.append(reinterpret_cast<const char*>(&yv), sizeof yv);
    }
    points += row.count;
  }
  const auto json = tsugumi::schema::encode(config);
  const tsugumi_blob blobs[] = {
      {"x", reinterpret_cast<const uint8_t*>(x.data()), x.size()},
      {"y", reinterpret_cast<const uint8_t*>(y.data()), y.size()}};
  const auto status =
      display_module_impl(nullptr, "line-chart", json.c_str(), blobs, 2);
  return status == 0 ? 0
                     : invalid("the built-in line-chart module is unavailable");
}

int display_table_impl(void*, const char* const* columns, size_t column_count,
                       const char* const* cells, size_t row_count) {
  if (column_count == 0 || column_count > 64 || row_count > 10000 || !columns ||
      (row_count && !cells))
    return invalid_display(
        "a table needs 1 to 64 columns and at most 10,000 rows");
  size_t bytes = 0;
  const auto fits = [&](const char* text) {
    if (!text) return false;
    const auto size = ::strnlen(text, 4097);
    bytes +=
        size + 16;  // Include per-string metadata before allocating copies.
    return size <= 4096 && bytes <= kMaxFrame;
  };
  for (size_t i = 0; i < column_count; ++i)
    if (!fits(columns[i]))
      return invalid_display(
          "table strings exceed 4 KiB or the display frame exceeds 16 MiB");
  for (size_t i = 0; i < column_count * row_count; ++i)
    if (!fits(cells[i]))
      return invalid_display(
          "table strings exceed 4 KiB or the display frame exceeds 16 MiB");
  return send_display([&](flatbuffers::FlatBufferBuilder& b) {
    std::vector<std::string> cols(columns, columns + column_count);
    std::vector<flatbuffers::Offset<D::TableRow>> rows;
    for (size_t r = 0; r < row_count; ++r) {
      std::vector<std::string> row(cells + r * column_count,
                                   cells + (r + 1) * column_count);
      rows.push_back(D::CreateTableRow(b, b.CreateVectorOfStrings(row)));
    }
    auto c = b.CreateVectorOfStrings(cols);
    return std::make_pair(
        D::DisplayValue::DisplayTable,
        D::CreateDisplayTable(b, c, b.CreateVector(rows)).Union());
  });
}

int display_image_impl(void*, const char* media_type, const uint8_t* data,
                       size_t size, const char* alt) {
  if (!media_type || !data || size > (8u << 20))
    return invalid_display("an image exceeds 8 MiB or is missing");
  return send_display([&](flatbuffers::FlatBufferBuilder& b) {
    auto mt = b.CreateString(media_type);
    auto d = b.CreateVector(data, size);
    auto a = b.CreateString(alt ? alt : "");
    return std::make_pair(D::DisplayValue::DisplayImage,
                          D::CreateDisplayImage(b, mt, d, a).Union());
  });
}

int display_points_impl(void*, const float* x, const float* y, const float* z,
                        const uint32_t* colors, size_t count) {
  if (count > 1000000 || (count && (!x || !y || !z)))
    return invalid_display(
        "a point cloud needs x, y and z, up to 1,000,000 points");
  return send_display([&](flatbuffers::FlatBufferBuilder& b) {
    auto xv = b.CreateVector(x, count);
    auto yv = b.CreateVector(y, count);
    auto zv = b.CreateVector(z, count);
    auto cv = b.CreateVector(colors, colors ? count : 0);
    return std::make_pair(
        D::DisplayValue::DisplayPointCloud,
        D::CreateDisplayPointCloud(b, xv, yv, zv, cv).Union());
  });
}

auto display_module_impl(void*, const char* name, const char* config_json,
                         const tsugumi_blob* blobs, size_t count) -> int {
  if (!name || !config_json || std::strlen(config_json) > (1u << 20) ||
      count > 64 || (count && !blobs))
    return -1;
  size_t bytes = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!blobs[i].name || std::strlen(blobs[i].name) > 128 ||
        (!blobs[i].data && blobs[i].size) || blobs[i].size > (8u << 20) - bytes)
      return -1;
    bytes += blobs[i].size;
  }
  std::string identity = name;
  if (!identity.starts_with("/nix/store/")) {
    if (g_views.empty()) return -1;
    tsugumi::schema::v::Mapping<tsugumi::schema::v::String,
                                tsugumi::schema::v::String>
        views;
    if (tsugumi::schema::decode(g_views, views)) return -1;
    bool found = false;
    for (const auto& [key, value] : views.value())
      if (key == identity) {
        identity = value;
        found = true;
        break;
      }
    if (!found) return -1;
  }
  return send_display([&](flatbuffers::FlatBufferBuilder& b) {
    auto n = b.CreateString(identity);
    auto d = b.CreateString(config_json);
    std::vector<flatbuffers::Offset<D::DisplayBlob>> values;
    for (size_t i = 0; i < count; ++i)
      values.push_back(
          D::CreateDisplayBlob(b, b.CreateString(blobs[i].name),
                               b.CreateVector(blobs[i].data, blobs[i].size)));
    return std::make_pair(
        D::DisplayValue::DisplayModule,
        D::CreateDisplayModule(b, n, d, b.CreateVector(values)).Union());
  });
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
    g_call->failure->args = {{"card", g_call->card},
                             {"source", source},
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

std::vector<std::string> parse_env_dump(const std::string& dump) {
  std::vector<std::string> out;
  size_t s = 0;
  while (s < dump.size()) {
    size_t e = dump.find('\0', s);
    if (e == std::string::npos) e = dump.size();
    std::string kv = dump.substr(s, e - s);
    if (kv.find('=') != std::string::npos &&
        !kv.starts_with("TSUGUMI_ENV_DUMP=") && !kv.starts_with("BASH_") &&
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

// These schemas also generate the language bindings. A resident worker only
// parses them once, independently of the set of ports present in a call.
auto catalog_for(const flatbuffers::String* text) -> flatbuffers::Parser* {
  if (!text) return nullptr;
  static std::map<std::string, std::unique_ptr<flatbuffers::Parser>> cache;
  if (auto it = cache.find(text->str()); it != cache.end())
    return it->second.get();
  flatbuffers::IDLOptions options;
  options.strict_json = true;
  options.natural_utf8 = true;
  options.output_default_scalars_in_json = true;
  auto parser = std::make_unique<flatbuffers::Parser>(options);
  parser->known_attributes_["tsugumi"] = true;
  if (!parser->Parse(text->c_str())) return nullptr;
  return (cache[text->str()] = std::move(parser)).get();
}

auto inputs_json_impl(void*) -> const char* {
  if (g_call->inputs_json) return g_call->inputs_json->c_str();
  auto* parser = catalog_for(g_call->msg->input_schema());
  std::string json;
  if (!parser || tsugumi::message_json::gen_language_text(
                     *parser, g_call->inputs, &json)) {
    record_failure(D::FailureKind::worker_failed, "cannot decode inputs");
    return nullptr;
  }
  return g_call->inputs_json.emplace(std::move(json)).c_str();
}

auto input_kind_impl(void*, const char* name) -> uint32_t {
  for (const auto* port : *g_call->msg->inputs())
    if (port->name()->string_view() == name)
      return static_cast<uint32_t>(port->kind());
  return 0;
}

auto outputs_json_impl(void*, const char* json, tsugumi_buffer* outputs) -> int {
  auto* parser = catalog_for(g_call->msg->output_schema());
  if (!parser || !tsugumi::message_json::parse_json(*parser, json)) {
    record_failure(D::FailureKind::output_invalid,
                   "outputs do not match the Card contract" +
                       (parser ? ": " + parser->error_ : ""));
    return 1;
  }
  return tsugumi_copy_outputs(outputs, parser->builder_.GetBufferPointer(),
                              parser->builder_.GetSize());
}

// Put each intact input message into the aggregate buffer. Its internal
// offsets stay relative to their original locations, including those of
// shared tables, strings, and union members.
auto pack_inputs(const W::WorkerCall& call) -> flatbuffers::FlatBufferBuilder {
  namespace fb = flatbuffers;
  fb::FlatBufferBuilder b;
  const auto* parser = catalog_for(call.input_schema());
  if (!parser) {
    record_failure(D::FailureKind::worker_failed, "cannot load input catalog");
    return b;
  }
  std::vector<std::pair<fb::voffset_t, fb::Offset<fb::Table>>> fields;
  for (const auto* in : *call.inputs()) {
    const auto* field = parser->root_struct_def_->fields.Lookup(in->name()->str());
    if (!field) {
      record_failure(D::FailureKind::worker_failed,
                     "input is absent from the Card contract: " + in->name()->str());
      return b;
    }
    const auto* bytes = in->value()->bytes();
    if (bytes->size() < sizeof(fb::uoffset_t)) {
      record_failure(D::FailureKind::type_mismatch, "invalid input buffer");
      return b;
    }
    const auto root = fb::ReadScalar<fb::uoffset_t>(bytes->data());
    if (root >= bytes->size()) {
      record_failure(D::FailureKind::type_mismatch, "invalid input root");
      return b;
    }
    b.PreAlign(bytes->size(), FLATBUFFERS_MAX_ALIGNMENT);
    b.PushBytes(bytes->data(), bytes->size());
    fields.emplace_back(field->value.offset,
                        fb::Offset<fb::Table>(b.GetSize() - root));
  }
  const auto start = b.StartTable();
  for (const auto& [field, value] : fields) b.AddOffset(field, value);
  b.Finish(fb::Offset<fb::Table>(b.EndTable(start)));
  const auto& inputs = *fb::GetAnyRoot(b.GetBufferPointer());
  for (const auto* field : parser->root_struct_def_->fields.vec) {
    if (field->IsRequired() && !inputs.CheckField(field->value.offset)) {
      record_failure(D::FailureKind::input_missing, field->name);
      break;
    }
  }
  return b;
}

// Copy a verified port table out of the aggregate. Keep inline data and
// union discriminators byte-for-byte, including optional scalars and -0.0.
namespace fb = flatbuffers;

auto copy_table(fb::FlatBufferBuilder& b, const reflection::Schema& schema,
                const reflection::Object& type, const fb::Table& table)
    -> fb::Offset<void>;

auto copy_object(fb::FlatBufferBuilder& b, const reflection::Schema& schema,
                 const reflection::Object& type, const uint8_t* data)
    -> fb::Offset<void> {
  if (!type.is_struct())
    return copy_table(b, schema, type,
                      *reinterpret_cast<const fb::Table*>(data));
  b.StartStruct(type.minalign());
  b.PushBytes(data, type.bytesize());
  return fb::Offset<void>(b.EndStruct());
}

auto copy_union(fb::FlatBufferBuilder& b, const reflection::Schema& schema,
                const reflection::Field& field, uint8_t tag,
                const uint8_t* data) -> fb::Offset<void> {
  if (!tag) return {};
  const auto* member =
      schema.enums()->Get(field.type()->index())->values()->Get(tag)->union_type();
  if (member->base_type() == reflection::String)
    return b.CreateString(reinterpret_cast<const fb::String*>(data)).Union();
  return copy_object(b, schema, *schema.objects()->Get(member->index()), data);
}

auto copy_vector(fb::FlatBufferBuilder& b, const reflection::Schema& schema,
                 const reflection::Field& field, const fb::Table& table)
    -> fb::Offset<void> {
  const auto* vector = fb::GetFieldAnyV(table, field);
  const auto element = field.type()->element();
  const auto* object = element == reflection::Obj
                           ? schema.objects()->Get(field.type()->index())
                           : nullptr;
  if (fb::IsScalar(element) || (object && object->is_struct())) {
    const size_t size = object ? object->bytesize() : fb::GetTypeSize(element);
    const size_t align = object ? object->minalign() : size;
    b.StartVector(vector->size(), size, align);
    b.PushBytes(vector->Data(), vector->size() * size);
    return fb::Offset<void>(b.EndVector(vector->size()));
  }
  const auto* offsets =
      reinterpret_cast<const fb::Vector<fb::Offset<uint8_t>>*>(vector);
  const auto* tags = element == reflection::Union
                         ? table.GetPointer<const fb::Vector<uint8_t>*>(
                               field.offset() - sizeof(fb::voffset_t))
                         : nullptr;
  std::vector<fb::Offset<void>> values;
  for (fb::uoffset_t i = 0; i < offsets->size(); ++i) {
    const auto* data = offsets->Get(i);
    if (element == reflection::String)
      values.push_back(
          b.CreateString(reinterpret_cast<const fb::String*>(data)).Union());
    else if (object)
      values.push_back(copy_object(b, schema, *object, data));
    else
      values.push_back(copy_union(b, schema, field, tags->Get(i), data));
  }
  b.StartVector<fb::Offset<void>>(values.size());
  for (auto it = values.rbegin(); it != values.rend(); ++it) {
    if (it->IsNull())
      b.PushElement(fb::uoffset_t{0});
    else
      b.PushElement(*it);
  }
  return fb::Offset<void>(b.EndVector(values.size()));
}

auto copy_table(fb::FlatBufferBuilder& b, const reflection::Schema& schema,
                const reflection::Object& type, const fb::Table& table)
    -> fb::Offset<void> {
  struct Inline {
    fb::voffset_t field;
    const uint8_t* data;
    size_t size;
    size_t alignment;
  };
  std::vector<Inline> inline_fields;
  std::vector<std::pair<fb::voffset_t, fb::Offset<void>>> offsets;
  for (const auto* field : *type.fields()) {
    const auto id = field->offset();
    const auto* data = table.GetAddressOf(id);
    if (!data) continue;
    const auto base = field->type()->base_type();
    if (fb::IsScalar(base)) {
      const auto size = fb::GetTypeSize(base);
      inline_fields.push_back({id, data, size, size});
    } else if (base == reflection::Obj) {
      const auto& object = *schema.objects()->Get(field->type()->index());
      if (object.is_struct()) {
        inline_fields.push_back({id, data, static_cast<size_t>(object.bytesize()),
                                 static_cast<size_t>(object.minalign())});
      } else {
        offsets.emplace_back(
            id, copy_object(b, schema, object, table.GetPointer<const uint8_t*>(id)));
      }
    } else if (base == reflection::String) {
      offsets.emplace_back(id, b.CreateString(fb::GetFieldS(table, *field)).Union());
    } else if (base == reflection::Vector) {
      offsets.emplace_back(id, copy_vector(b, schema, *field, table));
    } else if (base == reflection::Union) {
      offsets.emplace_back(
          id, copy_union(b, schema, *field,
                         table.GetField<uint8_t>(id - sizeof(fb::voffset_t), 0),
                         table.GetPointer<const uint8_t*>(id)));
    }
  }
  const auto start = b.StartTable();
  for (const auto& [id, offset] : offsets) b.AddOffset(id, offset);
  for (const auto& field : inline_fields) {
    b.Align(field.alignment);
    b.PushBytes(field.data, field.size);
    b.TrackField(field.field, b.GetSize());
  }
  return fb::Offset<void>(b.EndTable(start));
}

auto receive_outputs(const tsugumi_buffer& outputs) -> void {
  namespace fb = flatbuffers;
  auto* parser = catalog_for(g_call->msg->output_schema());
  if (!parser || !outputs.data || outputs.size > kMaxFrame) {
    record_failure(D::FailureKind::output_invalid,
                   "missing or oversized Outputs buffer");
    return;
  }
  // Use the same resolved catalog to verify the buffer and extract its ports.
  parser->Serialize();
  const auto* schema = reflection::GetSchema(parser->builder_.GetBufferPointer());
  if (!fb::Verify(*schema, *schema->root_table(), outputs.data, outputs.size)) {
    record_failure(D::FailureKind::output_invalid,
                   "Outputs does not match the Card contract");
    return;
  }
  const auto& root = *fb::GetAnyRoot(outputs.data);
  for (const auto* field : *schema->root_table()->fields()) {
    const auto* table = fb::GetFieldT(root, *field);
    if (!table) continue;
    const auto* type = schema->objects()->Get(field->type()->index());
    fb::FlatBufferBuilder b;
    b.Finish(copy_table(b, *schema, *type, *table));
    append_output(field->name()->string_view(), b.GetBufferPointer(),
                  b.GetSize());
  }
}

auto display_json_impl(void*, const char* json) -> int;

// Package libraries and language state are initialized during the first call.
void* g_lib = nullptr;
tsugumi_main_fn g_main = nullptr;
bool g_card_loaded = false;
std::string g_load_error;
std::string g_last_run;
std::vector<std::string>
    g_loaded_env;  // the process environment once the package was loaded
std::vector<std::string>
    g_set_by_load;  // NAME=value for sets; NAME for removals

std::vector<std::string> process_env() {
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

auto remove_env(std::vector<std::string>& env, const std::string& name)
    -> void {
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
  std::fflush(stdout);
  std::fflush(stderr);
}

constexpr tsugumi_host kEffects{
    .display_text = display_text_impl,
    .display_scalar = display_scalar_impl,
    .display_line_chart = display_line_chart_impl,
    .log = log_impl,
    .cancelled = cancelled_impl,
    .remaining_ms = remaining_ms_impl,
    .fail = fail_impl,
    .display_table = display_table_impl,
    .display_image = display_image_impl,
    .display_points = display_points_impl,
    .display_module = display_module_impl,
    .display_markdown = display_markdown_impl,
    .inputs_json = inputs_json_impl,
    .params_json = params_json_impl,
    .input_kind = input_kind_impl,
    .outputs_json = outputs_json_impl,
    .display_json = display_json_impl,
    .fail_at = fail_at_impl,
};

auto make_call(const Call& call, const flatbuffers::FlatBufferBuilder& inputs,
               const std::string& work, const std::string& tmp,
               const std::string& out_dir) -> tsugumi_call {
  const auto* m = call.msg;
  return {
      .abi_version = TSUGUMI_ABI_VERSION,
      .phase = TSUGUMI_PREPARE,
      .run_id = call.run_id.c_str(),
      .card = call.card.c_str(),
      .invocation = m->invocation().value_or(0),
      .inputs = inputs.GetBufferPointer(),
      .inputs_size = inputs.GetSize(),
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
  tsugumi_buffer outputs{};
  const int rc = g_main(&call, &outputs);
  std::unique_ptr<uint8_t, decltype(&std::free)> owned(outputs.data, std::free);
  if (rc != 0) {
    record_failure(g_cancel.load() ? D::FailureKind::cancelled
                                   : D::FailureKind::worker_failed,
                   std::string(name) + " returned " + std::to_string(rc));
    return false;
  }
  if (!g_call->failure && phase == TSUGUMI_RUN) receive_outputs(outputs);
  return !g_call->failure;
}

// The environment of a package body (the order is at the top of this file):
// `step` is the call's environment with preRun's exports, `call` without them.
std::vector<std::string> body_env(const std::vector<std::string>& call,
                                  const std::vector<std::string>& step) {
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
      if (eq == std::string::npos)
        remove_env(out, name);
      else
        set_env(out, name, e.substr(eq + 1));
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
void bash_outputs(const W::WorkerCall* m, const std::string& out_json) {
  namespace sc = tsugumi::schema;
  if (::access(out_json.c_str(), R_OK) != 0) return;
  sc::v::Mapping<sc::v::String, sc::Json> doc;
  if (auto err = sc::decode(read_text(out_json), doc)) {
    record_failure(
        D::FailureKind::output_invalid,
        "$TSUGUMI_OUTPUT is not a JSON object of ports: " + err->message);
    return;
  }
  for (const auto& [name, value] : doc.value().entries()) {
    const D::Port* port = nullptr;
    for (const auto* p : *m->outputs())
      if (p->name()->str() == name) port = p;
    if (!port) {
      record_failure(D::FailureKind::output_invalid,
                     "unknown output port " + name + " in $TSUGUMI_OUTPUT");
      return;
    }
    auto* p = parser_for(port->schema_bfbs(), port->root_type()->str());
    std::string obj = sc::text(value);
    if (!p || !tsugumi::message_json::parse_json(*p, obj)) {
      record_failure(D::FailureKind::output_invalid,
                     "output " + name + " JSON does not match its type" +
                         (p ? ": " + p->error_ : ""));
      return;
    }
    if (std::ranges::any_of(g_call->outputs, [&](const auto& output) {
          return output.first == name;
        })) {
      record_failure(D::FailureKind::output_invalid,
                     "output " + name + " emitted twice");
      return;
    }
    append_output(name, p->builder_.GetBufferPointer(), p->builder_.GetSize());
  }
}

// A display object from bash or an in-memory language adapter. A relative image
// path is resolved against the working directory.
struct BashDisplayTable {
  tsugumi::schema::Opt<"columns",
                       tsugumi::schema::v::Array<tsugumi::schema::v::String>>
      columns;
  tsugumi::schema::Opt<
      "rows", tsugumi::schema::v::Array<
                  tsugumi::schema::v::Array<tsugumi::schema::v::String>>>
      rows;
};
struct BashDisplayImage {
  tsugumi::schema::Opt<"path", tsugumi::schema::v::String> path;
  tsugumi::schema::Opt<"alt", tsugumi::schema::v::String> alt;
};
struct BashDisplayModule {
  tsugumi::schema::Opt<"module", tsugumi::schema::v::String> module;
  tsugumi::schema::Opt<"config", tsugumi::schema::Json> config;
  tsugumi::schema::Opt<"blobs",
                       tsugumi::schema::v::Mapping<tsugumi::schema::v::String,
                                                   tsugumi::schema::v::String>>
      blobs;
};
struct BashDisplay {
  tsugumi::schema::Opt<"text", tsugumi::schema::v::String> text;
  tsugumi::schema::Opt<"markdown", tsugumi::schema::v::String> markdown;
  tsugumi::schema::Opt<"table", tsugumi::schema::Obj<BashDisplayTable>> table;
  tsugumi::schema::Opt<"image", tsugumi::schema::Obj<BashDisplayImage>> image;
  tsugumi::schema::Opt<"module", tsugumi::schema::Obj<BashDisplayModule>>
      module;
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
        if (row.size() != names.size())
          return invalid("a table row has " + std::to_string(row.size()) +
                         " cells, not " + std::to_string(names.size()));
        for (const auto& c : row) cells.push_back(c.c_str());
        ++row_count;
      }
    return display_table_impl(nullptr, names.data(), names.size(), cells.data(),
                              row_count);
  } else if (const auto* image = sc::get(d.image)) {
    const auto* file = sc::get(image->path);
    if (!file) return invalid("an image needs a path");
    std::string full = file->starts_with("/") ? *file : cwd + "/" + *file;
    if (::access(full.c_str(), R_OK) != 0)
      return invalid("cannot read the image " + *file);
    std::string bytes = read_text(full);
    const char* media = bytes.starts_with("\x89PNG")    ? "image/png"
                        : bytes.starts_with("\xFF\xD8") ? "image/jpeg"
                                                        : nullptr;
    if (!media) return invalid(*file + " is neither PNG nor JPEG");
    const auto* alt = sc::get(image->alt);
    return display_image_impl(nullptr, media,
                              reinterpret_cast<const uint8_t*>(bytes.data()),
                              bytes.size(), alt ? alt->c_str() : "");
  } else if (const auto* module = sc::get(d.module)) {
    const auto* name = sc::get(module->module);
    const auto* data = sc::get(module->config);
    if (!name || !data) return invalid("a module needs module and config");
    std::vector<std::pair<std::string, std::string>> owned;
    if (const auto* blobs = sc::get(module->blobs))
      for (const auto& [key, file] : *blobs) {
        const auto full = file.starts_with("/") ? file : cwd + "/" + file;
        if (::access(full.c_str(), R_OK) != 0)
          return invalid("cannot read blob " + file);
        owned.emplace_back(key, read_text(full));
      }
    std::vector<tsugumi_blob> blobs;
    for (const auto& [key, bytes] : owned)
      blobs.push_back({key.c_str(),
                       reinterpret_cast<const uint8_t*>(bytes.data()),
                       bytes.size()});
    if (display_module_impl(nullptr, name->c_str(), sc::text(*data).c_str(),
                            blobs.data(), blobs.size()))
      return invalid("unknown view module; declare it in views");
  } else {
    return invalid("name one of text, markdown, table, image or module");
  }
  return 0;
}

auto bash_display(const std::string& path, const std::string& cwd) -> void {
  if (::access(path.c_str(), R_OK) == 0 && json_display(read_text(path), cwd))
    record_failure(D::FailureKind::output_invalid,
                   "$TSUGUMI_REPORT: " + g_call->display_error);
}

auto display_json_impl(void*, const char* json) -> int {
  if (!g_call) return 1;
  char* cwd = ::getcwd(nullptr, 0);
  const int status = json_display(json, cwd ? cwd : ".");
  std::free(cwd);
  return status;
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
  std::string work = m->working_directory()->str();
  std::string tmp = m->temporary_directory()->str();
  std::string out_dir =
      m->output_directory() ? m->output_directory()->str() : "";
  std::vector<std::string> env;
  for (const auto* e : *m->environment())
    env.push_back(e->name()->str() + "=" + e->value()->str());
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
    std::string dump = tmp + "/pre-env";
    auto pre_env = env;
    set_env(pre_env, "TSUGUMI_ENV_DUMP", dump);
    // Sourced, so that its exports and `cd` can be read back afterwards.
    int st = run_shell(
        opt.bash,
        {"-c",
         ". \"$0\"; exec \"$TSUGUMI_WORKER\" --dump-env \"$TSUGUMI_ENV_DUMP\"",
         opt.pre},
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
    auto inputs = pack_inputs(*m);
    apply_env(body_env(call_env, env));
    if (::chdir(cwd.c_str()) != 0)
      record_failure(D::FailureKind::worker_failed,
                     "cannot enter working directory " + cwd);
    if (!call.failure) {
      call.inputs = inputs.GetBufferPointer();
      auto c = make_call(call, inputs, work, tmp, out_dir);
      if (!g_card_loaded && g_load_error.empty() && may_start("source loading")) {
        // Keep lifetime HOME for interpreter caches. Card source code gets
        // this invocation's HOME, just like the run-start hook and run.
        const auto before_open = process_env();
        open_package(opt.package + "/lib/main.so");
        remember_loaded_env(before_open);
        if (g_main) {
          auto initialization_env = process_env();
          if (auto home = env_value(g_loaded_env, "HOME"))
            set_env(initialization_env, "HOME", *home);
          apply_env(initialization_env);
          const auto before_prepare = process_env();
          if (!invoke(c, TSUGUMI_PREPARE, "language runtime initialization"))
            g_load_error = "cannot initialize the language runtime (see stderr)";
          remember_loaded_env(before_prepare);
          apply_env(body_env(call_env, env));
          const auto before_load = process_env();
          if (g_load_error.empty() && !invoke(c, TSUGUMI_LOAD, "Card source loading"))
            g_load_error = "Card source loading failed (see stderr)";
          remember_loaded_env(before_load);
          g_card_loaded = true;
          std::fflush(stdout);
          std::fflush(stderr);
        }
      }
      if (g_card_loaded) apply_env(body_env(call_env, env));
      if (!g_load_error.empty())
        record_failure(D::FailureKind::worker_failed, g_load_error);
      if (!call.failure && may_start("run-start hook") && call.run_id != g_last_run)
        invoke(c, TSUGUMI_RUN_START, "onRunStart");
      g_last_run = call.run_id;
      if (!call.failure && may_start("main")) {
        invoke(c, TSUGUMI_RUN, "main.so");
        std::fflush(stdout);
        std::fflush(stderr);
      }
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
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
      timed_out = post_timeout;
      step_failed("postRun", st);
    }
  }
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
    else if (k == "--views")
      g_views = argv[i + 1];
  }
  // Standby starts only the worker executable. No package-controlled code,
  // including constructors or an alleged SDK adapter, runs before a call.
  g_loaded_env = process_env();
  std::thread(control_loop).detach();
  uint64_t last_call = 0;
  while (true) {
    auto frame = read_frame(kData);
    if (!frame) return 0;
    flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(frame->data()),
                            frame->size());
    if (!W::VerifyWorkerMessageBuffer(v)) return 2;
    const auto* m = W::GetWorkerMessage(frame->data());
    if (m->value_type() != W::WorkerPayload::WorkerCall || !m->call_id() ||
        *m->call_id() <= last_call)
      return 2;
    const auto* wc = m->value_as_WorkerCall();
    Call call;
    call.msg = wc;
    call.call_id = *m->call_id();
    last_call = call.call_id;
    call.run_id = wc->run_id()->str();
    for (const auto* s : *wc->card()->segments())
      call.card += (call.card.empty() ? "" : "/") + s->str();
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
