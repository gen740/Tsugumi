#include "service.hh"

#include <fcntl.h>
#include <flatbuffers/reflection.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <fstream>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include <iterator>
#include <set>

#include "execution_service.hh"
#include "nix.hh"
#include "nix_params.hh"
#include "packed_builds.hh"
#include "permissions.hh"
#include "remote_execution_service.hh"
#include "run_scratch.hh"
#include "scheduler.hh"
#include "tsugumi/document/host_requirements.hh"
#include "tsugumi/document/input_types.hh"
#include "tsugumi/protocol/runtime_state_generated.h"
#include "tsugumi/protocol/trace_codec.hh"
#include "tsugumi/runtime_support/cache_usage.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/flat_file.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/log.hh"
#include "tsugumi/workerd/frame.hh"
#include "tsugumi/workerd/store.hh"
#include "version_facts.hh"
#include "worker_selection.hh"

extern const unsigned char tsugumi_rpc_bfbs[];

namespace tsugumi {

namespace R = Tsugumi::RPC;
namespace D = Tsugumi::Data;
using FBB = flatbuffers::FlatBufferBuilder;
namespace stdfs = std::filesystem;

// ---------------------------------------------------------------------------
// Request digests

namespace {

constexpr size_t kMaxViewBytes = 1u << 20;
const std::string kExampleId = "00000000-0000-0000-0000-000000000000";

const std::map<std::string, std::string>& digest_defaults() {
  static const std::map<std::string, std::string> d = {
      {"Tsugumi.RPC.ProjectValidateRequest.mode", "2"},
      {"Tsugumi.RPC.ProjectFileListRequest.depth", "0"},
      {"Tsugumi.RPC.ProjectFileListRequest.include_directories", "0"},
      {"Tsugumi.RPC.ProjectFileListRequest.limit", "100"},
      {"Tsugumi.RPC.ProjectFileReadRequest.offset", "0"},
      {"Tsugumi.RPC.ProjectFileReadRequest.limit_bytes", "262144"},
      {"Tsugumi.RPC.RunPlanRequest.propagate", "1"},
      {"Tsugumi.RPC.RunRequest.propagate", "1"},
      {"Tsugumi.RPC.RunRequest.max_concurrency", "4"},
  };
  return d;
}

void digest_table(const reflection::Schema& s, const reflection::Object& obj,
                  const flatbuffers::Table& t, crypto::Sha256& h);

void put_len(crypto::Sha256& h, std::string_view v) {
  uint64_t n = v.size();
  h.update(&n, sizeof n);
  h.update(v);
}

void digest_scalar(const reflection::Field& f, const flatbuffers::Table& t,
                   crypto::Sha256& h, const std::string& qualified) {
  bool present = t.CheckField(f.offset());
  std::string repr;
  if (present) {
    auto bt = f.type()->base_type();
    if (bt == reflection::Float || bt == reflection::Double) {
      repr = std::to_string(flatbuffers::GetAnyFieldF(t, f));
    } else {
      repr = std::to_string(flatbuffers::GetAnyFieldI(t, f));
    }
  } else {
    auto it = digest_defaults().find(qualified);
    if (it != digest_defaults().end())
      repr = it->second;
    else if (!f.optional())
      repr = std::to_string(f.default_integer());
    else
      repr =
          "\x01"
          "absent";
  }
  put_len(h, repr);
}

void digest_table(const reflection::Schema& s, const reflection::Object& obj,
                  const flatbuffers::Table& t, crypto::Sha256& h) {
  std::vector<const reflection::Field*> fields(obj.fields()->begin(),
                                               obj.fields()->end());
  std::sort(fields.begin(), fields.end(),
            [](auto* a, auto* b) { return a->id() < b->id(); });
  for (const auto* f : fields) {
    std::string qualified = obj.name()->str() + "." + f->name()->str();
    put_len(h, f->name()->str());
    auto bt = f->type()->base_type();
    switch (bt) {
      case reflection::String: {
        const auto* str = flatbuffers::GetFieldS(t, *f);
        put_len(h, str ? std::string_view(str->c_str(), str->size())
                       : std::string_view("\x01"
                                          "absent"));
        break;
      }
      case reflection::Obj: {
        const auto* sub = s.objects()->Get(f->type()->index());
        const auto* st = flatbuffers::GetFieldT(t, *f);
        if (st)
          digest_table(s, *sub, *st, h);
        else
          put_len(h,
                  "\x01"
                  "absent");
        break;
      }
      case reflection::Union: {
        // The type field precedes; resolve the member table.
        auto type_name = f->name()->str() + "_type";
        const auto* tf = obj.fields()->LookupByKey(type_name.c_str());
        auto utype = static_cast<int>(flatbuffers::GetFieldI<uint8_t>(t, *tf));
        put_len(h, std::to_string(utype));
        if (utype) {
          const auto* en = s.enums()->Get(f->type()->index());
          const auto* ev = en->values()->LookupByKey(utype);
          const auto* sub = s.objects()->Get(ev->union_type()->index());
          const auto* st = flatbuffers::GetFieldT(t, *f);
          if (st) digest_table(s, *sub, *st, h);
        }
        break;
      }
      case reflection::Vector: {
        const auto* vec = flatbuffers::GetFieldAnyV(t, *f);
        uint64_t n = vec ? vec->size() : 0;  // absent vectors equal empty ones
        h.update(&n, sizeof n);
        if (!vec) break;
        auto et = f->type()->element();
        if (et == reflection::UByte || et == reflection::Byte) {
          put_len(h,
                  std::string_view(reinterpret_cast<const char*>(vec->Data()),
                                   vec->size()));
        } else if (et == reflection::String) {
          for (uint64_t i = 0; i < n; ++i) {
            const auto* str =
                flatbuffers::GetAnyVectorElemPointer<const flatbuffers::String>(
                    vec, i);
            put_len(h, std::string_view(str->c_str(), str->size()));
          }
        } else if (et == reflection::Obj) {
          const auto* sub = s.objects()->Get(f->type()->index());
          for (uint64_t i = 0; i < n; ++i)
            digest_table(
                s, *sub,
                *flatbuffers::GetAnyVectorElemPointer<const flatbuffers::Table>(
                    vec, i),
                h);
        } else {
          for (uint64_t i = 0; i < n; ++i)
            put_len(h,
                    std::to_string(flatbuffers::GetAnyVectorElemI(vec, et, i)));
        }
        break;
      }
      case reflection::UType:
        break;  // covered with the union value
      default:
        digest_scalar(*f, t, h, qualified);
    }
  }
}

}  // namespace

std::string request_digest(const R::Request* req) {
  if (const auto* save = req->value_as_ProjectSaveRequest();
      save && save->save_intent())
    return crypto::sha256_hex("save-intent\n" + save->save_intent()->str());
  const auto* schema = reflection::GetSchema(tsugumi_rpc_bfbs);
  const char* name = R::EnumNameRequestPayload(req->value_type());
  std::string full = std::string("Tsugumi.RPC.") + name;
  const auto* obj = schema->objects()->LookupByKey(full.c_str());
  crypto::Sha256 h;
  h.update(full);
  digest_table(*schema, *obj,
               *reinterpret_cast<const flatbuffers::Table*>(req->value()), h);
  return h.finish_hex();
}

// ---------------------------------------------------------------------------

namespace {

// What a run records about its binary caches: where packages may have come
// from, never how to authenticate there. A URL's userinfo and query string
// (tokens, S3 profiles) are dropped; so is anything that is not one printable
// line of a sane length.
std::vector<std::string> configured_substituters(const stdfs::path& file) {
  std::vector<std::string> raw;
  if (file.empty()) {
    raw = NixStore::substituters();
  } else {
    std::ifstream in(file);
    if (!in)
      log::error("cannot read the binary cache list " + file.string() +
                 ": runs will record none");
    for (std::string line; std::getline(in, line);) raw.push_back(line);
  }
  std::vector<std::string> urls;
  for (auto url : raw) {
    url = text::public_cache_endpoint(std::move(url));
    if (!url.empty() && urls.size() < 64 &&
        std::find(urls.begin(), urls.end(), url) == urls.end())
      urls.push_back(url);
  }
  return urls;
}

}  // namespace

Service::Service(ServiceOptions opts) : opts_(std::move(opts)) {
  mutation_lane_ = std::make_unique<Lane>(1);
  read_pool_ = std::make_unique<Lane>(6);
  // Plan requests can occupy every reader while the execution broker is down.
  cancel_lane_ = std::make_unique<Lane>(1);
  shell_lane_ = std::make_unique<Lane>(2);
}

Service::~Service() { shutdown(); }

auto Service::packed_debug() const -> bool {
  return packed_builds_ && packed_builds_->debug;
}

auto Service::packed_fast() const -> bool {
  return packed_builds_ && packed_builds_->fast;
}

void Service::open() {
  const auto state = opts_.application_support.empty()
                         ? stdfs::path{}
                         : opts_.application_support / ".tsugumi";
  Versions::recover_checkout(opts_.root);
  // The Card products in a bundle already contain the export-time parameters.
  store_ = std::make_unique<Store>(StoreOptions{
      opts_.root, opts_.fault, state, opts_.packed_builds.empty()});
  store_->open();
  if (!opts_.packed_builds.empty()) {
    packed_builds_ = std::make_unique<PackedBuilds>(
        PackedBuilds::read(opts_.packed_builds,
                           *store_->current()->resolution));
    for (const auto& name : packed_builds_->host_paths) {
      const auto folder = opts_.application_support / name;
      stdfs::create_directories(folder);
      if (stdfs::canonical(folder) != folder)
        throw std::runtime_error("packed host path points outside its folder: " +
                                 folder.string());
      // Fast Cards share an address space and the process environment.
      if (packed_builds_->fast &&
          ::setenv(grant_environment(name).c_str(), folder.c_str(), 1) != 0)
        throw std::runtime_error("cannot set the packed host path " + name);
    }
  }
  const auto scratch =
      opts_.scratch_dir.empty()
          ? stdfs::temp_directory_path() / ("tsugumi-" + store_->project_id())
          : opts_.scratch_dir;
  nix_ = std::make_unique<NixRunner>(*this);
  versions_ = std::make_unique<Versions>(opts_.root);

  if (!packed_builds_)
    substituters_ = configured_substituters(opts_.substituters_file);
  run_scratch_ = disk_scratch_path(opts_.run_scratch_dir.empty()
                                       ? default_run_scratch(opts_.root)
                                       : opts_.run_scratch_dir);
  history_ = std::make_unique<History>(store_->state_dir() / "runs.sqlite",
                                       opts_.fault, run_scratch_);
  execution_service_ =
      std::make_unique<LocalExecutionService>(*this, scratch / "workers");
  remote_execution_ =
      std::make_unique<RemoteExecutionService>(*this, remote_jobs_);
  scheduler_ = std::make_unique<Scheduler>(*this);
  scheduler_->recover_interrupted();
  if (packed_builds_) {
#ifdef __APPLE__
    if (packed_builds_->fast) {
      const auto libraries = packed_builds_->libraries();
      load_fast_pack_images(*this, libraries);
    }
#endif
    return;
  }
  const auto roots = opts_.gcroots_dir.empty()
                         ? store_->state_dir() / "cache" / "gcroots"
                         : opts_.gcroots_dir;
  stdfs::create_directories(roots);
  fs::write_atomic(roots / ".name", opts_.root.filename().string(), 0644);
  if (opts_.usage_file.empty()) {
    if (opts_.usage_roots_dir.empty())
      cache_usage_ = std::make_unique<CacheUsage>(
          stdfs::path{}, stdfs::path{}, roots, run_scratch_, project_id());
    else
      cache_usage_ = std::make_unique<CacheUsage>(opts_.usage_roots_dir,
                                                  opts_.usage_scratch_dir);
  }
}

auto Service::invalidate_resident(const std::string& card) -> void {
  {
    std::lock_guard lock(worker_selection_mutex_);
    resident_targets_.erase(card);
  }
  execution_service_->invalidate(card, "the Card definition changed");
}

namespace {
class SelectedExecution final : public PreparedExecution {
 public:
  SelectedExecution(std::unique_ptr<PreparedExecution> execution,
                    std::function<void()> release)
      : execution_(std::move(execution)), release_(std::move(release)) {}
  ~SelectedExecution() override { release_(); }
  auto stage(const std::atomic<bool>& cancel) -> void override {
    execution_->stage(cancel);
  }
  auto execute(const ExecutionRequest& request) -> ExecutionResult override {
    return execution_->execute(request);
  }

 private:
  std::unique_ptr<PreparedExecution> execution_;
  std::function<void()> release_;
};
}  // namespace

namespace {
auto unsupported_local_option(const Settings& settings,
                              const RemoteTarget& target) -> std::string {
#ifdef __linux__
  if (!settings.grants.host_paths.empty() && !settings.sandbox.filesystem)
    return "named host paths require sandbox.filesystem";
  // Without bubblewrap only the plain worker could run, which enforces none
  // of these: the Card fails instead of running without what it selected.
  if (!target.namespace_isolation) {
    if (!settings.grants.host_paths.empty())
      return "named host paths require a Linux worker with bubblewrap";
    if (settings.sandbox.filesystem)
      return "sandbox.filesystem requires a Linux worker with bubblewrap";
    if (!settings.grants.network)
      return "a Card without grants.network requires a Linux worker with "
             "bubblewrap to keep it off the network";
    if (settings.sandbox.pid)
      return "sandbox.pid requires a Linux worker with bubblewrap";
    if (settings.sandbox.ipc)
      return "sandbox.ipc requires a Linux worker with bubblewrap";
    if (settings.sandbox.identity)
      return "sandbox.identity requires a Linux worker with bubblewrap";
  }
#elif defined(__APPLE__)
  if (!settings.grants.host_paths.empty())
    return "named host paths on macOS require the shared Linux VM";
  if ((settings.sandbox.filesystem || settings.sandbox.pid ||
       settings.sandbox.ipc || settings.sandbox.identity ||
       !settings.grants.network) &&
      !worker_has_seatbelt())
    return "the requested Card isolation requires Seatbelt";
#endif
  if (settings.sandbox.limits.limited() && !target.resource_limits)
    return "sandbox.limits requires a delegated cgroup v2 subtree";
  if (settings.grants.gui && !target.gui)
    return "grants.gui requires a native Linux worker with an available GUI "
           "socket";
  return {};
}

auto effective_local_options(const Settings& settings) -> std::string {
  std::vector<std::string_view> notes;
  if (!settings.sandbox.filesystem
#ifdef __APPLE__
      && !settings.sandbox.identity
#endif
  ) {
    notes.push_back(
        "host paths and GUI endpoints can be visible with filesystem isolation "
        "off");
#ifndef __APPLE__
    if (!settings.grants.network)
      notes.push_back(
          "network isolation does not hide filesystem Unix sockets");
#endif
  }
#ifdef __APPLE__
  if (settings.sandbox.filesystem)
    notes.push_back(
        "Seatbelt allows host file metadata and reads of the "
        "full Nix store and selected system paths");
  if (settings.grants.network &&
      (settings.sandbox.filesystem || settings.sandbox.identity))
    notes.push_back("Seatbelt still restricts access to the local host");
  if (settings.grants.network && settings.sandbox.ipc)
    notes.push_back("Seatbelt IPC isolation blocks the system DNS resolver");
#endif
  std::string message;
  for (const auto note : notes) {
    if (!message.empty()) message += ", ";
    message += note;
  }
  return message;
}

auto supports_remote_options(const Settings& settings) -> bool {
  return settings.sandbox.filesystem && settings.sandbox.pid &&
         !settings.reuse_worker && !settings.grants.gui &&
         settings.grants.host_paths.empty();
}
auto score_from_output(std::string_view output) -> std::optional<double> {
  if (output.ends_with('\n')) output.remove_suffix(1);
  if (const auto newline = output.rfind('\n'); newline != output.npos)
    output.remove_prefix(newline + 1);
  const auto first = output.find_first_not_of(" \t\r");
  if (first == output.npos) return std::nullopt;
  output = output.substr(first, output.find_last_not_of(" \t\r") - first + 1);
  if (output.starts_with('+')) {
    output.remove_prefix(1);
    if (output.starts_with('-')) return std::nullopt;
  }
  double score = 0;
  const auto [end, error] =
      std::from_chars(output.data(), output.data() + output.size(), score);
  if (error != std::errc{} || end != output.data() + output.size() ||
      !std::isfinite(score))
    return std::nullopt;
  return score;
}

auto local_probe_life() -> std::string {
#ifdef __linux__
  if (auto boot = fs::read_file("/proc/sys/kernel/random/boot_id"); boot)
    return crypto::sha256_hex(*boot);
#elif defined(__APPLE__)
  timeval boot{};
  size_t size = sizeof boot;
  if (::sysctlbyname("kern.boottime", &boot, &size, nullptr, 0) == 0)
    return crypto::sha256_hex(std::to_string(boot.tv_sec) + ":" +
                              std::to_string(boot.tv_usec));
#endif
  static const auto life = crypto::random_token(32);
  return life;
}

auto supports_target(const Settings& settings, const RemoteTarget& target)
    -> bool {
  if (target.id == kLocalExecutionService)
    return unsupported_local_option(settings, target).empty();
  return supports_remote_options(settings) &&
         (!settings.sandbox.limits.limited() || target.resource_limits);
}
}  // namespace

auto Service::measure_card_probe(const RemoteTarget& target, const Node& node,
                                 const stdfs::path& directory,
                                 const BuildRequest& build,
                                 std::string& observed_life,
                                 std::string_view expected_key,
                                 bool* probe_declared)
    -> D::WorkerProbeResultT {
  D::WorkerProbeResultT result;
  result.worker = target.id;
  if (!target.available || !supports_target(node.settings, target)) {
    result.unavailable_reason =
        target.unavailable_reason.empty()
            ? (!target.available
                   ? "The worker is unavailable"
                   : "The worker cannot use the Card's sandbox or grants")
            : target.unavailable_reason;
    return result;
  }

  const bool local = target.id == kLocalExecutionService;
  std::unique_ptr<PreparedExecution> prepared;
  auto request = build;
  request.probe_only = true;
  request.max_implementations = 1;
  request.timeout_seconds =
      std::min(request.timeout_seconds.value_or(kProbeBuildTimeoutSeconds),
               kProbeBuildTimeoutSeconds);
  request.target = {local ? WorkerKind::native : WorkerKind::remote,
                    target.native_system};
  Node probe = node;
  probe.outputs.clear();
  probe.settings.reuse_worker = false;
  probe.settings.timeout_seconds =
      std::min(node.settings.timeout_seconds.value_or(kProbeTimeoutSeconds),
               kProbeTimeoutSeconds);
  probe.settings.build_timeout_seconds = request.timeout_seconds;
  probe.settings.secrets.clear();
  const auto scratch = directory.parent_path() / (".probe-" + crypto::uuid4());
  struct Scratch {
    stdfs::path path;
    ~Scratch() {
      std::error_code error;
      stdfs::remove_all(path, error);
    }
  } cleanup{scratch};
  std::string key, life, revision, retry_system;
  // The Card declares a probe: a failure from here on is the probe's, even
  // when its evaluation fails or times out before needs_probe_build.
  if (probe_declared) {
    *probe_declared = false;
    request.on_probe_declared = [probe_declared] { *probe_declared = true; };
  }
  request.needs_probe_build = [&](std::string_view output) {
    result.probe = output;
    if (!local) {
      auto live = remote_jobs_.probe(target.id, target.server_id, build.cancel);
      observed_life = live.probe_life;
      if (live.native_system != request.target.system) {
        retry_system = std::move(live.native_system);
        return false;
      }
    }
    const auto& settings = probe.settings;
    const auto& sandbox = settings.sandbox;
    crypto::Sha256 hash;
    hash.update(output);
    hash.update(json::Encode(std::vector<std::string>{
        sandbox.filesystem ? "true" : "false", sandbox.pid ? "true" : "false",
        sandbox.ipc ? "true" : "false", sandbox.identity ? "true" : "false",
        settings.grants.network ? "true" : "false",
        settings.grants.gui ? "true" : "false",
        std::to_string(sandbox.limits.cpu_millis),
        std::to_string(sandbox.limits.memory_bytes),
        std::to_string(*settings.timeout_seconds),
        std::to_string(*settings.build_timeout_seconds)}));
    for (const auto& bind : host_path_binds(probe))
      hash.update(json::Encode(std::vector<std::string>{
          json::Quote(bind.name), json::Quote(bind.source),
          json::Quote(bind.target), bind.write ? "true" : "false"}));
    key = hash.finish_hex();
    if (!expected_key.empty() && expected_key != key)
      throw Failed(FailureKind::invalid_request,
                   "The Card probe or its execution settings changed");
    life = local ? local_probe_life() : observed_life;
    if (remote_jobs_.enabled() && !life.empty()) {
      try {
        auto cached =
            remote_jobs_.probe_lookup(target.id, key, life, build.cancel);
        revision = std::move(cached.revision);
        if (cached.result) {
          result = std::move(*cached.result);
          return false;
        }
      } catch (const std::exception&) {
        if (!local || build.cancel.stop_requested()) throw;
        // A local Card can run even when hostd cannot serve its cache.
      }
    }
    prepared =
        local ? execution_service_->prepare(probe, scratch, request)
              : remote_execution_->prepare(target, probe, scratch, request);
    return true;
  };
  try {
    auto plan = nix().build(request);
    if (!retry_system.empty()) {
      request.target.system = std::exchange(retry_system, std::string{});
      result.probe.clear();
      plan = nix().build(request);
      if (!retry_system.empty())
        throw Failed(FailureKind::unsupported,
                     "Worker system changed while evaluating the probe");
    }
    if (!prepared) return result;
    std::stop_source stop;
    std::atomic<bool> cancelled{false};
    std::stop_callback cancel(build.cancel, [&] {
      cancelled = true;
      stop.request_stop();
    });
    // A cold remote store must fetch the derivation closure before the
    // worker's build and Card-call deadlines start.
    const auto wait_budget =
        std::chrono::seconds(kProbeBuildTimeoutSeconds + kProbeTimeoutSeconds) +
        std::chrono::minutes(local ? 0 : 10);
    std::mutex timer_mutex;
    std::condition_variable_any timer_ready;
    std::jthread timer([&](std::stop_token done) {
      std::unique_lock lock(timer_mutex);
      timer_ready.wait_for(lock, done, wait_budget,
                           [] { return false; });
      if (!done.stop_requested()) {
        cancelled = true;
        stop.request_stop();
      }
    });
    const auto executable = plan.implementation(0);
    InvocationSummaryData invocation;
    std::mutex output_mutex;
    bool truncated = false;
    auto outcome = prepared->execute(
        {probe,
         executable,
         {},
         {},
         invocation,
         scratch,
         cancelled,
         [&] { return cancelled.load(); },
         stop.get_token(),
         [&](LogStream stream, const std::string& bytes) {
           if (stream != LogStream::output) return;
           std::lock_guard guard(output_mutex);
           const auto remaining = kProbeOutputBytes - result.output_text.size();
           const auto accepted = std::min<size_t>(remaining, bytes.size());
           result.output_text.append(bytes, 0, accepted);
           truncated |= accepted < bytes.size();
         },
         {},
         [](int64_t, int64_t) {},
         [](const auto&) {},
         [] {},
         {},
         kProbeTransportBytes,
         {},
         {},
         stop.get_token()});
    if (outcome.failure) {
      result.failure = outcome.failure->diagnostic.value_or(
          failure_kind_name(outcome.failure->kind));
      result.failure_kind = outcome.failure->kind;
      // A local probe is built before it runs; a remote worker builds it in
      // the same job, so which limit it reached is in its message only.
      if (local && outcome.failure->kind == FailureKind::timeout)
        result.time_limit_seconds = *probe.settings.timeout_seconds;
    } else if (truncated)
      result.failure = "Probe output exceeded 8192 bytes";
    else if (const auto score = score_from_output(result.output_text))
      result.score = *score;
    else
      result.failure = "The last line of probe output must be a finite number";
  } catch (const ExecutionDetached&) {
    result.failure = "Probe did not finish within the measurement time limit";
    result.failure_kind = FailureKind::timeout;
    result.time_limit_seconds =
        kProbeBuildTimeoutSeconds + kProbeTimeoutSeconds + (local ? 0 : 600);
  } catch (const Failed& error) {
    result.failure = error.what();
    result.failure_kind = error.failure.kind;
    if (error.failure.kind == FailureKind::timeout)
      result.time_limit_seconds = *probe.settings.build_timeout_seconds;
  } catch (const std::exception& error) {
    result.failure = error.what();
  }
  if (build.cancel.stop_requested())
    throw Failed(FailureKind::cancelled, "worker selection cancelled");
  result.measured_at = now_ms();
  result.failure.resize(
      std::min<uint64_t>(result.failure.size(), kProbeOutputBytes));
  if (!revision.empty()) {
    try {
      remote_jobs_.probe_publish(target.id, key, life, revision, result,
                                 node.path, build.cancel);
    } catch (const std::exception& error) {
      log::warn("cannot cache Card probe: " + std::string(error.what()));
    }
  }
  return result;
}

void Service::worker_probe_measure(RequestIn& req) {
  const auto* request =
      project_request(req)->value_as_WorkerProbeMeasureRequest();
  const auto key = request->key()->string_view();
  if (!crypto::is_sha256_hex(key))
    throw Failed(FailureKind::invalid_request, "Invalid Card probe identity");
  BuildRequest build;
  build.doc = store_->current();
  build.node = build.doc->resolution->find(wire::card_arg(request->card()));
  if (!build.node || build.node->kind != CardType::Code ||
      build.node->is_virtual)
    throw Failed(FailureKind::not_found,
                 "The Card with this probe is unavailable");
  build.timeout_seconds = build.node->settings.build_timeout_seconds;
  std::stop_source stop;
  std::stop_callback disconnect(req.connection_stop,
                                [&] { stop.request_stop(); });
  std::stop_callback shutdown(shutdown_stop_.get_token(),
                              [&] { stop.request_stop(); });
  build.cancel = stop.get_token();
  const auto targets = remote_targets(build.cancel);
  const auto target = std::ranges::find(
      targets, request->worker()->string_view(), &RemoteTarget::id);
  if (target == targets.end())
    throw Failed(FailureKind::not_found,
                 "The worker is unavailable to this project");
  std::string life;
  auto result = measure_card_probe(*target, *build.node,
                                   run_scratch() / "probe", build, life, key);
  if (!result.unavailable_reason.empty())
    throw Failed(FailureKind::unsupported, result.unavailable_reason);
  if (!result.score && result.failure.empty())
    throw Failed(FailureKind::not_found,
                 "The Card no longer declares this probe");
  FBB b;
  const auto value = R::CreateWorkerProbeMeasurement(
      b, D::WorkerProbeResult::Pack(b, &result));
  reply(req,
        wire::reply(b, req.request_id, R::ReplyPayload::WorkerProbeMeasurement,
                    value.Union()));
}

auto Service::prepare_execution(const Node& node, const stdfs::path& directory,
                                BuildRequest& build,
                                InvocationSummaryData& summary)
    -> std::unique_ptr<PreparedExecution> {
  if (packed_builds_) {
#ifdef __APPLE__
    if (packed_builds_->fast) {
      build.target = {WorkerKind::native, native_system()};
      return prepare_fast_pack_execution(*this, node);
    }
#endif
    return execution_service_->prepare(node, directory, build);
  }
  auto targets = remote_targets(build.cancel);
  auto& measurements = summary.worker_probes;
  std::map<std::string, std::string> remote_lives;
  const auto measure_targets = [&] {
    measurements.clear();
    remote_lives.clear();
    bool measured = false;
    for (const auto& target : targets) {
      bool declared = false;
      auto result = measure_card_probe(target, node, directory, build,
                                       remote_lives[target.id], {}, &declared);
      // A declared probe whose evaluation fails or times out never reaches
      // needs_probe_build; it is measured all the same, so its failure stays
      // and no worker becomes eligible by default. A Card without a probe, or
      // one whose own evaluation fails before the probe is known, is not
      // measured: the execution reports its own failure.
      measured |= !result.probe.empty() || (declared && !result.failure.empty());
      measurements.push_back(std::move(result));
    }
    if (measured) {
      for (auto& result : measurements)
        if (!result.score && result.failure.empty() &&
            result.unavailable_reason.empty())
          result.failure = "No probe is defined for this worker's system";
    } else {
      for (auto& result : measurements) {
        if (!result.unavailable_reason.empty()) continue;
        result.failure.clear();
        result.measured_at = 0;
        result.score = 1.0;
      }
    }
    return measured;
  };
  bool measured = measure_targets();
  std::stop_callback wake(build.cancel, [&] {
    std::lock_guard lock(worker_selection_mutex_);
    worker_selection_cv_.notify_all();
  });
  const auto card = path_key(node.path);
  std::unique_lock lock(worker_selection_mutex_);
  if (!node.settings.reuse_worker) resident_targets_.erase(card);
  RemoteTarget selected;
  using Clock = std::chrono::steady_clock;
  constexpr auto kCatalogInterval = std::chrono::milliseconds(100);
  auto catalog_at = Clock::now() + kCatalogInterval;
  for (;;) {
    if (build.cancel.stop_requested())
      throw Failed(FailureKind::cancelled, "worker selection cancelled");
    std::vector<WorkerCandidate> candidates;
    for (const auto& target : targets) {
      const auto capacity = target.id == kLocalExecutionService
                                ? opts_.env_cpus
                                : target.max_jobs;
      const auto result = std::ranges::find(measurements, target.id,
                                            &D::WorkerProbeResultT::worker);
      candidates.push_back(
          {target.id, !capacity || worker_slots_[target.id] < capacity,
           target.available && supports_target(node.settings, target),
           result != measurements.end() && result->score
               ? std::optional<double>(*result->score)
               : std::nullopt});
    }
    const auto resident = resident_targets_.find(card);
    const auto resident_id =
        resident == resident_targets_.end() ? std::string{} : resident->second;
    bool worker_restarted = false;
    for (;;) {
      const auto choice = select_worker(candidates, measured, resident_id);
      if (!choice.matches) {
        // Said once for the Card, the log and the Agent: no worker was chosen,
        // so the Card never started, and why for each worker. `worker:<id>`
        // arguments let a UI say the same in its own words.
        std::string message =
            measured ? "No worker could be chosen, so the Card was not "
                       "started: no worker received a positive probe score"
                     : "No worker could be chosen, so the Card was not "
                       "started: no registered worker supports the Card's "
                       "settings";
        std::vector<std::pair<std::string, std::string>> args{
            {"stage", "worker_selection"}};
        // An out-of-date worker (built from another revision) is named as
        // such, with its contract in `contract:<id>` and this core's in
        // `contract`, so a UI can say how to bring it up to date.
        const auto outdated = [&](const std::string& id) {
          const auto target = std::ranges::find(targets, id, &RemoteTarget::id);
          return target != targets.end() && !target->available &&
                         id != kLocalExecutionService &&
                         !target->server_id.empty() &&
                         target->worker_contract != workerd::kContract
                     ? &*target
                     : nullptr;
        };
        bool contract = false;
        for (const auto& probe : measurements) {
          message += "\n" + probe.worker + ": ";
          std::string reason;
          if (const auto* target = outdated(probe.worker);
              target && !probe.unavailable_reason.empty()) {
            reason = "outdated";
            message += probe.unavailable_reason;
            args.emplace_back("contract:" + probe.worker,
                              std::to_string(target->worker_contract));
            if (!std::exchange(contract, true))
              args.emplace_back("contract",
                                std::to_string(workerd::kContract));
          } else if (!probe.unavailable_reason.empty()) {
            reason = "unavailable";
            message += probe.unavailable_reason;
          } else if (probe.score) {
            reason = "not_positive";
            message += "probe score " + std::to_string(*probe.score);
          } else if (probe.failure_kind == FailureKind::timeout) {
            reason = "timeout";
            message += "the probe timed out";
            if (probe.time_limit_seconds)
              message += " after " + std::to_string(*probe.time_limit_seconds) +
                         " s";
            message += " (" + probe.failure + ")";
            if (probe.time_limit_seconds)
              args.emplace_back("timeout:" + probe.worker,
                                std::to_string(*probe.time_limit_seconds));
          } else {
            reason = "failed";
            message += "the probe failed: " + probe.failure;
          }
          args.emplace_back("worker:" + probe.worker, std::move(reason));
        }
        throw Failed(FailureKind::unsupported, std::move(message),
                     std::move(args));
      }
      if (!choice.index) break;
      const auto& target = targets[*choice.index];
      if (target.id != kLocalExecutionService) {
        lock.unlock();
        const auto live =
            remote_jobs_.probe(target.id, target.server_id, build.cancel);
        lock.lock();
        if (measured && remote_lives[target.id] != live.probe_life) {
          worker_restarted = true;
          break;
        }
        if (live.active_jobs >= live.max_jobs ||
            worker_slots_[target.id] >= live.max_jobs) {
          candidates[*choice.index].free = false;
          continue;
        }
      }
      selected = target;
      break;
    }
    if (!selected.id.empty()) break;
    const auto now = Clock::now();
    if (!worker_restarted && now < catalog_at) {
      worker_selection_cv_.wait_until(lock, catalog_at);
      continue;
    }
    std::vector<RemoteTarget> current;
    if (!worker_restarted && now - worker_catalog_checked_ < kCatalogInterval) {
      current = worker_catalog_;
    } else {
      if (worker_catalog_refreshing_) {
        worker_selection_cv_.wait_for(lock, kCatalogInterval);
        continue;
      }
      worker_catalog_refreshing_ = true;
      lock.unlock();
      try {
        current = remote_targets(build.cancel);
      } catch (...) {
        lock.lock();
        worker_catalog_refreshing_ = false;
        worker_selection_cv_.notify_all();
        throw;
      }
      lock.lock();
      worker_catalog_refreshing_ = false;
      worker_selection_cv_.notify_all();
      if (build.cancel.stop_requested())
        throw Failed(FailureKind::cancelled, "worker selection cancelled");
      worker_catalog_ = current;
      worker_catalog_checked_ = Clock::now();
    }
    catalog_at = Clock::now() + kCatalogInterval;
    lock.unlock();
    const auto unchanged = [measured](const RemoteTarget& a,
                                      const RemoteTarget& b) {
      return a.id == b.id && a.server_id == b.server_id &&
             a.available == b.available && a.native_system == b.native_system &&
             a.resource_limits == b.resource_limits &&
             (!measured || a.probe_revision == b.probe_revision);
    };
    if (worker_restarted || !std::ranges::equal(targets, current, unchanged)) {
      targets = current;
      measured = measure_targets();
    } else {
      targets = current;
    }
    lock.lock();
  }
  ++worker_slots_[selected.id];
  lock.unlock();
  auto release = [this, id = selected.id] {
    std::lock_guard guard(worker_selection_mutex_);
    --worker_slots_[id];
    worker_selection_cv_.notify_all();
  };
  try {
    auto execution =
        selected.id == kLocalExecutionService
            ? execution_service_->prepare(node, directory, build)
            : remote_execution_->prepare(selected, node, directory, build);
    if (node.settings.reuse_worker) {
      std::lock_guard guard(worker_selection_mutex_);
      resident_targets_[card] = selected.id;
    }
    return std::make_unique<SelectedExecution>(std::move(execution), release);
  } catch (...) {
    release();
    throw;
  }
}

auto Service::remote_execution() -> RemoteExecutionService& {
  return *remote_execution_;
}

auto Service::host_path_binds(const Node& node) const
    -> std::vector<WorkerSpawn::Bind> {
  if (node.settings.grants.host_paths.empty()) return {};
  if (packed_builds_) {
    std::vector<WorkerSpawn::Bind> result;
    for (const auto& grant : node.settings.grants.host_paths) {
      if (!packed_builds_->host_paths.contains(grant.name))
        throw Failed(FailureKind::not_found,
                     "host path is missing from the packed app: " + grant.name);
      const auto source = opts_.application_support / grant.name;
      std::error_code ec;
      if (stdfs::canonical(source, ec) != source ||
          !stdfs::is_directory(source))
        throw Failed(FailureKind::not_found,
                     "packed host path is unavailable: " + source.string());
      result.push_back(
          {source.string(), "/host/" + grant.name, grant.write, grant.name});
    }
    return result;
  }
  json::Members bindings;
  if (!opts_.control_dir.empty())
    if (const auto text =
            fs::read_file(opts_.control_dir / "host-path-bindings.json"))
      bindings = json::Object(*text);
  std::vector<WorkerSpawn::Bind> result;
  for (const auto& grant : node.settings.grants.host_paths) {
    const auto key = node.project_dir + grant.name;
    auto hint = std::vector<std::pair<std::string, std::string>>{
        {"host_path_name", grant.name},
        {"host_path_scope", node.project_dir},
        {"card", path_key(node.path)}};
    const auto entry = bindings.find(key);
    if (entry == bindings.end())
      throw Failed(FailureKind::not_found,
                   "Host path '" + grant.name +
                       "' is unbound. Choose its folder in Project Settings > "
                       "Host Paths.",
                   hint);
    const auto binding = json::Object(entry->second);
    const auto path = json::Text(binding, "path");
    if (!opts_.host_folders.empty() &&
        (path.empty() ||
         (grant.write && json::Get(binding, "write") != "true"))) {
      hint.emplace_back("host_path_refresh", "true");
      throw Failed(FailureKind::not_found,
                   "Host path '" + grant.name +
                       "' needs an updated VM share. Reconnect the project and "
                       "try again.",
                   std::move(hint));
    }
    stdfs::path source;
    if (opts_.host_folders.empty()) {
      source = path;
      std::error_code ec;
      if (!source.is_absolute() || stdfs::canonical(source, ec) != source)
        source.clear();
    } else if (path.size() == 16 &&
               path.find_first_not_of("0123456789abcdef") == path.npos) {
      source = opts_.host_folders / path;
    }
    std::error_code ec;
    if (source.empty() || !stdfs::is_directory(source, ec) ||
        (grant.write && json::Get(binding, "write") != "true"))
      throw Failed(FailureKind::not_found,
                   "Host path '" + grant.name +
                       "' is unavailable. Choose its folder in Project "
                       "Settings > Host Paths.",
                   hint);
    result.push_back(
        {source.string(), "/host/" + grant.name, grant.write, grant.name});
  }
  return result;
}

auto Service::card_secrets(const Node& node, std::stop_token stop) const
    -> std::vector<std::pair<std::string, std::string>> {
  const auto& names = node.settings.secrets;
  if (names.empty()) return {};
  if (packed_builds_)
    throw Failed(FailureKind::unsupported,
                 "packed apps cannot resolve Card secrets");
  WorkerSecrets supplied;
  auto& values = supplied.values;
  try {
    values = remote_jobs_.card_secrets(names, stop);
  } catch (const std::exception& error) {
    throw Failed(FailureKind::not_found,
                 "the Card's secrets are unavailable (" +
                     std::string(error.what()) +
                     "): open the project from the Tsugumi app or Web UI");
  }
  WorkerSecrets result;
  auto& ordered = result.values;
  ordered.reserve(names.size());
  std::string missing;
  for (const auto& name : names) {
    const auto found = std::ranges::find_if(
        values, [&](const auto& value) { return value.first == name; });
    if (found == values.end() || found->second.empty())
      missing += (missing.empty() ? "" : ", ") + name;
    else
      ordered.push_back(*found);
  }
  if (!missing.empty())
    throw Failed(FailureKind::not_found,
                 "Card secret not set: " + missing +
                     ". Set it in Settings > Card secrets, then run again",
                 {{"secrets", missing}});
  return std::move(result.values);
}

auto Service::remote_targets(std::stop_token stop) const
    -> std::vector<RemoteTarget> {
  const auto fill_local = [this](RemoteTarget& local) {
    local.available = true;
    local.native_system = native_system();
    local.systems = NixRunner::execution_systems();
    local.resource_limits = worker_limits_available();
    local.namespace_isolation = worker_has_bwrap();
    local.gui = opts_.gcroots_dir.empty() && worker_gui_available();
  };
  if (packed_builds_) {
    RemoteTarget local;
    local.id = kLocalExecutionService;
    fill_local(local);
    return {std::move(local)};
  }
  try {
    auto targets = remote_jobs_.catalog(stop);
    auto local =
        std::ranges::find(targets, kLocalExecutionService, &RemoteTarget::id);
    if (local == targets.end()) {
      RemoteTarget target;
      target.id = kLocalExecutionService;
      target.available = true;
      targets.insert(targets.begin(), std::move(target));
      local = targets.begin();
    }
    const bool local_available = local->available;
    fill_local(*local);
    local->available = local_available;
    std::rotate(targets.begin(), local, local + 1);
    return targets;
  } catch (const std::exception&) {
    RemoteTarget local;
    local.id = kLocalExecutionService;
    fill_local(local);
    return {std::move(local)};
  }
}

auto Service::start_remote_cleanup() -> void {
  scheduler_->start_remote_cleanup();
}

auto Service::execution_diagnostic(const Node& node,
                                   const std::vector<RemoteTarget>& targets)
    -> std::optional<Diag> {
  const auto& settings = node.settings;
  if (node.kind != CardType::Code) return std::nullopt;
  const auto error = [&](std::string message) -> Diag {
    return {true, "invalid_setting", std::move(message), node.path, {}, {}};
  };
  const auto unavailable = [&](const RemoteTarget& target) {
    return error(target.unavailable_reason.empty()
                     ? "execution service " + target.id + " is not available"
                     : target.unavailable_reason);
  };
  const auto local_diagnostic =
      [&](const RemoteTarget& target) -> std::optional<Diag> {
    if (auto message = unsupported_local_option(settings, target);
        !message.empty())
      return error(std::move(message));
    if (auto message = effective_local_options(settings); !message.empty())
      return Diag{false,
                  "sandbox_effective_settings",
                  std::move(message),
                  node.path,
                  {},
                  {}};
    return std::nullopt;
  };
  const RemoteTarget* blocked = nullptr;
  const RemoteTarget* local = nullptr;
  for (const auto& target : targets) {
    if (target.id == kLocalExecutionService) {
      local = &target;
      continue;
    }
    if (!supports_remote_options(settings) ||
        (settings.sandbox.limits.limited() && !target.resource_limits))
      continue;
    if (target.available) return std::nullopt;
    if (!blocked) blocked = &target;
  }
  if (local && local->available) {
    auto diagnostic = local_diagnostic(*local);
    if (!diagnostic || !diagnostic->error || !blocked) return diagnostic;
  }
  if (blocked) return unavailable(*blocked);
  return error("no registered worker supports the Card settings");
}

void Service::shutdown() {
  remote_jobs_.stop();
  shutdown_stop_.request_stop();
  {
    std::lock_guard lock(cancellations_mu_);
    shell_stopping_ = true;
    // Accepted saves drain with the mutation lane. Only a peer disconnect
    // cancels its save; stopping the service must not discard accepted edits.
    for (const auto& [key, cancelled] : shell_cancellations_)
      cancelled->store(true);
  }
  if (shell_lane_) shell_lane_->stop();
  if (cancel_lane_) cancel_lane_->stop();
  if (scheduler_) scheduler_->shutdown();
#ifdef __APPLE__
  if (packed_fast()) stop_fast_pack_images(*this);
#endif
  execution_service_.reset();  // the project's resident and standby workers end
  // Queued history writes announce themselves through the server, which
  // outlives the service only until it is closed.
  if (history_) history_->flush();
  if (mutation_lane_) mutation_lane_->stop();
  if (read_pool_) read_pool_->stop();
  cache_usage_.reset();
}

std::vector<std::string> Service::capabilities() {
  std::vector<std::string> caps = {"SessionHello",
                                   "ProjectActivity",
                                   "CardId",
                                   "ProjectHostRequirements",
                                   "ProjectOpen",
                                   "ProjectInspect",
                                   "ProjectSave",
                                   "ProjectReload",
                                   "ProjectValidate",
                                   "ProjectCacheClear",
                                   "ProjectCacheUsage",
                                   "RunScratchDelete",
                                   "RunHistoryDelete",
                                   "RunCategoryDelete",
                                   "OperationGet",
                                   "ProjectFileList",
                                   "ProjectFileStat",
                                   "ProjectFileRead",
                                   "OutputTypes",
                                   "InputTypes",
                                   "EvaluateNix",
                                   "RunPlan",
                                   "Run",
                                   "RunCancel",
                                   "RunStatus",
                                   "ProjectShell",
                                   "RunEnvironments",
                                   "RunEnvironmentPaths",
                                   "RunEnvironmentUses",
                                   "RunHistory",
                                   "RunInvocations",
                                   "RunLatestInvocations",
                                   "RunDefinitionPaths",
                                   "WorkerProbeMeasure",
                                   "RunRecord",
                                   "RunValueRead",
                                   "ArtifactList",
                                   "ArtifactRead",
                                   "LogRange",
                                   "ViewClose",
                                   "ViewSend",
                                   "ViewCredit",
                                   "ViewSave",
                                   "CardViewRead",
                                   "NixPathLockSource",
                                   "RewriteParamLiteral",
                                   "VersionInit",
                                   "VersionMark",
                                   "VersionCheckout",
                                   "VersionGraph",
                                   "VersionCanvas",
                                   "VersionNote",
                                   "VersionDiff",
                                   "VersionComments"};
  return caps;
}

void Service::reply(const RequestIn& req, std::string bytes) {
  server_->reply(req.conn, std::move(bytes));
}

void Service::fail(const RequestIn& req, const FailureData& f) {
  reply(req, wire::failure_reply(req.request_id, f));
}

std::pair<std::string, bool> Service::hello(const RequestIn& in) {
  const auto* h = project_request(in)->value_as_SessionHelloRequest();
  auto failure = [&](FailureKind k, std::string msg) {
    return std::make_pair(
        wire::failure_reply(in.request_id,
                            FailureData::make(k, std::move(msg))),
        false);
  };
  if (in.websocket) {
    if (!h->token() || !crypto::equal_secret(h->token()->str(), opts_.token))
      return failure(FailureKind::unauthenticated, "invalid token");
  } else if (h->token() &&
             !crypto::equal_secret(h->token()->str(), opts_.token)) {
    return failure(FailureKind::unauthenticated, "invalid token");
  }
  if (h->expected_project_id() &&
      h->expected_project_id()->str() != store_->project_id())
    return failure(FailureKind::permission_denied,
                   "this endpoint serves a different project");
  FBB b;
  auto caps = capabilities();
  std::vector<flatbuffers::Offset<flatbuffers::String>> cv;
  for (const auto& c : caps) cv.push_back(b.CreateString(c));
  auto r = R::CreateHelloResult(b, b.CreateString(store_->project_id()),
                                b.CreateString(opts_.server_instance_id),
                                b.CreateVector(cv));
  auto response = wire::reply(b, in.request_id, R::ReplyPayload::HelloResult,
                              r.Union());
  if (h->client() == Tsugumi::Trace::Client::MacOS ||
      h->client() == Tsugumi::Trace::Client::Web) {
    std::lock_guard lock(subs_mu_);
    ui_connections_.insert(in.conn);
  }
  return {std::move(response), true};
}

void Service::disconnected(ConnId conn) {
  if (trace_owner_ == conn) {
    trace::CancelWatch();
    trace::SetMode(trace::Mode::Off);
    trace_owner_.reset();
  }
  {
    std::lock_guard lock(cancellations_mu_);
    for (const auto& [key, cancelled] : shell_cancellations_)
      if (key.first == conn) cancelled->store(true);
    for (const auto& [key, cancelled] : save_cancellations_)
      if (key.first == conn) cancelled->store(true);
  }
  std::lock_guard<std::mutex> l(subs_mu_);
  subscriptions_.erase(conn);
  end_views(conn);
  ui_connections_.erase(conn);
  if (ui_owner_ == conn) ui_owner_.reset();
}

void Service::on_lane(Lane& lane, RequestIn req,
                      void (Service::*fn)(RequestIn&)) {
  const auto key = std::make_pair(req.conn, req.request_id);
  if (fn == &Service::save) {
    req.cancelled = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard lock(cancellations_mu_);
    save_cancellations_[key] = req.cancelled;
  }
  bool ok = lane.submit([this, req, fn]() mutable {
    auto cleanup = [this, &req](void*) {
      if (!req.cancelled) return;
      std::lock_guard lock(cancellations_mu_);
      save_cancellations_.erase({req.conn, req.request_id});
    };
    std::unique_ptr<void, decltype(cleanup)> guard(this, cleanup);
    try {
      (this->*fn)(req);
    } catch (const Failed& f) {
      fail(req, f.failure);
    } catch (const std::exception& e) {
      log::error(std::string("request failed: ") + e.what());
      fail(req, FailureData::make(FailureKind::internal_error, e.what()));
    }
  });
  if (!ok) {
    if (req.cancelled) {
      std::lock_guard lock(cancellations_mu_);
      save_cancellations_.erase(key);
    }
    fail(req,
         FailureData::make(FailureKind::busy, "the request queue is full"));
  }
}

void Service::dispatch(RequestIn req) {
  using P = R::RequestPayload;
  Lane& m = *mutation_lane_;
  Lane& r = *read_pool_;
  const auto* request = project_request(req);
  if (packed_builds_) {
    switch (request->value_type()) {
      case P::ProjectOpenRequest:
      case P::ProjectInspectRequest:
      case P::RunRequest:
      case P::RunCancelRequest:
      case P::RunStatusRequest:
      case P::CardViewReadRequest:
      case P::ViewSendRequest:
      case P::ViewCreditRequest:
      case P::ViewCloseRequest:
        break;
      case P::RunHistoryRequest:
      case P::RunInvocationsRequest:
      case P::RunLatestInvocationsRequest:
      case P::RunRecordRequest:
      case P::RunValueReadRequest:
      case P::ArtifactListRequest:
      case P::ArtifactReadRequest:
        if (packed_debug()) break;
        [[fallthrough]];
      default:
        return fail(req, FailureData::make(
                             FailureKind::unsupported,
                             "request unavailable in an exported app"));
    }
  }
  switch (request->value_type()) {
    case P::CoreTraceRequest: {
      namespace T = Tsugumi::Trace;
      if (!trace::LaunchEnabled())
        return fail(req, FailureData::make(
                             FailureKind::permission_denied,
                             "Project core was started without RPC tracing"));
      const auto* debug = request->value_as_CoreTraceRequest();
      if (req.websocket) {
        const auto* path =
            std::getenv("TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE");
        const auto capability = path ? fs::read_file(path) : std::nullopt;
        if (!capability || capability->empty() || !debug->capability() ||
            !crypto::equal_secret(debug->capability()->string_view(),
                                  *capability))
          return fail(
              req, FailureData::make(FailureKind::permission_denied,
                                     "RPC capture requires local host access"));
      }
      if (trace_owner_ && trace_owner_ != req.conn)
        return fail(
            req, FailureData::make(FailureKind::busy,
                                   "Another host connection owns RPC tracing"));
      T::SnapshotT snapshot;
      if (auto mode = debug->mode()) {
        if (*mode > static_cast<uint8_t>(T::Mode::MAX))
          return fail(req, FailureData::make(FailureKind::invalid_request,
                                             "Unknown RPC trace mode"));
        trace_owner_ = req.conn;
        trace::SetMode(static_cast<trace::Mode>(*mode));
        snapshot.mode = static_cast<T::Mode>(*mode);
      } else {
        std::function<void()> changed;
        if (trace_owner_ == req.conn) {
          changed = [server = server_, conn = req.conn] {
            FBB b;
            auto value = T::CreateTraceChanged(b);
            auto note = R::CreateNotification(
                b, b.CreateString("rpc-trace"), flatbuffers::nullopt,
                R::NotificationPayload::TraceChanged, value.Union());
            R::FinishMessageBuffer(
                b, R::CreateMessage(b, flatbuffers::nullopt,
                                    R::Payload::Notification, note.Union()));
            server->reply(
                conn,
                std::string(reinterpret_cast<const char*>(b.GetBufferPointer()),
                            b.GetSize()));
          };
        }
        snapshot =
            trace::ToWire(trace::Read(std::move(changed), debug->after()));
      }
      FBB b;
      auto value = T::Snapshot::Pack(b, &snapshot);
      reply(req, wire::reply(b, req.request_id, R::ReplyPayload::TraceSnapshot,
                             value.Union()));
      return;
    }
    case P::ProjectActivityRequest: {
      // Observe outside the lanes so this query cannot count itself as work.
      flatbuffers::FlatBufferBuilder b;
      auto value = R::CreateProjectActivity(b, busy());
      reply(req, wire::reply(b, req.request_id,
                             R::ReplyPayload::ProjectActivity, value.Union()));
      return;
    }
    case P::WorkerProbeMeasureRequest:
      return on_lane(r, std::move(req), &Service::worker_probe_measure);
    case P::ProjectHostRequirementsRequest:
      return on_lane(r, std::move(req), &Service::project_host_requirements);
    case P::ProjectOpenRequest:
      return on_lane(m, std::move(req), &Service::open_project);
    case P::CardIdRequest:
      return on_lane(m, std::move(req), &Service::card_id);
    case P::ProjectSaveRequest:
      return on_lane(m, std::move(req), &Service::save);
    case P::ProjectReloadRequest:
      return on_lane(m, std::move(req), &Service::reload);
    case P::ProjectCacheClearRequest:
      return on_lane(m, std::move(req), &Service::cache_clear);
    case P::ProjectCacheUsageRequest:
      return on_lane(r, std::move(req), &Service::cache_usage);
    case P::RunScratchDeleteRequest:
      return on_lane(m, std::move(req), &Service::scratch_delete);
    case P::RunCategoryDeleteRequest:
      return on_lane(m, std::move(req), &Service::history_category_delete);
    case P::RunHistoryDeleteRequest:
      return on_lane(m, std::move(req), &Service::history_delete);
    case P::RunRequest:
      return on_lane(m, std::move(req), &Service::run);
    case P::VersionInitRequest:
      return on_lane(m, std::move(req), &Service::version_init);
    case P::VersionMarkRequest:
      return on_lane(m, std::move(req), &Service::version_mark);
    case P::VersionCheckoutRequest:
      return on_lane(m, std::move(req), &Service::version_checkout);
    case P::VersionGraphRequest:
      return on_lane(r, std::move(req), &Service::version_graph);
    case P::VersionCanvasRequest:
      return on_lane(r, std::move(req), &Service::version_canvas);
    case P::VersionNoteRequest:
      return on_lane(m, std::move(req), &Service::version_note);
    case P::VersionDiffRequest:
      return on_lane(r, std::move(req), &Service::version_diff);
    case P::VersionCommentsRequest:
      return on_lane(r, std::move(req), &Service::version_comments);
    case P::ProjectInspectRequest:
      return on_lane(r, std::move(req), &Service::inspect);
    case P::ProjectValidateRequest:
      return on_lane(r, std::move(req), &Service::validate);
    case P::OperationGetRequest:
      return on_lane(r, std::move(req), &Service::operation_get);
    case P::ProjectFileListRequest:
      return on_lane(r, std::move(req), &Service::file_list);
    case P::ProjectFileStatRequest:
      return on_lane(r, std::move(req), &Service::file_stat);
    case P::ProjectFileReadRequest:
      return on_lane(r, std::move(req), &Service::file_read);
    case P::InputTypesRequest:
      return on_lane(r, std::move(req), &Service::input_types);
    case P::OutputTypesRequest:
      return on_lane(r, std::move(req), &Service::output_types);
    case P::NixPathLockSourceRequest:
      return on_lane(r, std::move(req), &Service::nix_path_lock_source);
    case P::RewriteParamLiteralRequest:
      return on_lane(r, std::move(req),
                     &Service::rewrite_param_literal_request);
    case P::EvaluateNixRequest:
      return on_lane(r, std::move(req), &Service::evaluate_nix);
    case P::RunPlanRequest:
      return on_lane(r, std::move(req), &Service::run_plan);
    case P::RunCancelRequest:
      return on_lane(*cancel_lane_, std::move(req), &Service::run_cancel);
    case P::RunStatusRequest:
      return on_lane(r, std::move(req), &Service::run_status);
    case P::RunHistoryRequest:
      return on_lane(r, std::move(req), &Service::run_history);
    case P::RunInvocationsRequest:
      return on_lane(r, std::move(req), &Service::run_invocations);
    case P::RunDefinitionPathsRequest:
      return on_lane(r, std::move(req), &Service::run_definition_paths);
    case P::RunLatestInvocationsRequest:
      return on_lane(r, std::move(req), &Service::run_latest_invocations);
    case P::ProjectShellRequest:
      return dispatch_shell(std::move(req));
    case P::RunEnvironmentsRequest:
      return on_lane(r, std::move(req), &Service::run_environments);
    case P::RunEnvironmentUsesRequest:
      return on_lane(r, std::move(req), &Service::run_environment_uses);
    case P::RunEnvironmentPathsRequest:
      return on_lane(r, std::move(req), &Service::run_environment_paths);
    case P::RunRecordRequest:
      return on_lane(r, std::move(req), &Service::run_record);
    case P::RunValueReadRequest:
      return on_lane(r, std::move(req), &Service::run_value_read);
    case P::ArtifactListRequest:
      return on_lane(r, std::move(req), &Service::artifact_list);
    case P::ArtifactReadRequest:
      return on_lane(r, std::move(req), &Service::artifact_read);
    case P::LogRangeRequest:
      return on_lane(r, std::move(req), &Service::log_range);
    case P::ViewSendRequest:
      return on_lane(r, std::move(req), &Service::view_send);
    case P::ViewCreditRequest:
      return on_lane(r, std::move(req), &Service::view_credit);
    case P::ViewCloseRequest:
      return on_lane(r, std::move(req), &Service::view_close);
    case P::ViewSaveRequest:
      return on_lane(r, std::move(req), &Service::view_save);
    case P::CardViewReadRequest:
      return on_lane(r, std::move(req), &Service::card_view_read);
    default:
      return fail(req, FailureData::make(FailureKind::unsupported,
                                         "request is not supported"));
  }
}

// ---------------------------------------------------------------------------
// Receipts

std::string Service::receipt_reply(uint64_t request_id,
                                   const std::string& receipt_bytes,
                                   bool replayed) {
  const auto* rc =
      flatbuffers::GetRoot<R::OperationReceipt>(receipt_bytes.data());
  FBB b;
  R::ReplyPayload type;
  flatbuffers::Offset<void> value;
  const char* tname = nullptr;
  switch (rc->result_type()) {
    case R::ReceiptValue::SaveResult:
      type = R::ReplyPayload::SaveResult;
      tname = "Tsugumi.RPC.SaveResult";
      break;
    case R::ReceiptValue::RunAccepted:
      type = R::ReplyPayload::RunAccepted;
      tname = "Tsugumi.RPC.RunAccepted";
      break;
    case R::ReceiptValue::ProjectCacheClearResult:
      type = R::ReplyPayload::ProjectCacheClearResult;
      tname = "Tsugumi.RPC.ProjectCacheClearResult";
      break;
    case R::ReceiptValue::RunScratchDeleteResult:
      type = R::ReplyPayload::RunScratchDeleteResult;
      tname = "Tsugumi.RPC.RunScratchDeleteResult";
      break;
    case R::ReceiptValue::RunCategoryDeleteResult:
      type = R::ReplyPayload::RunCategoryDeleteResult;
      tname = "Tsugumi.RPC.RunCategoryDeleteResult";
      break;
    case R::ReceiptValue::RunHistoryDeleteResult:
      type = R::ReplyPayload::RunHistoryDeleteResult;
      tname = "Tsugumi.RPC.RunHistoryDeleteResult";
      break;
    case R::ReceiptValue::ProjectShellResult:
      type = R::ReplyPayload::ProjectShellResult;
      tname = "Tsugumi.RPC.ProjectShellResult";
      break;
    case R::ReceiptValue::VersionResult:
      type = R::ReplyPayload::VersionResult;
      tname = "Tsugumi.RPC.VersionResult";
      break;
    default:
      type = R::ReplyPayload::Failure;
      tname = "Tsugumi.Data.Failure";
      break;
  }
  value = wire::copy_table(
      b, tname, reinterpret_cast<const flatbuffers::Table*>(rc->result()));
  std::string out = wire::reply(b, request_id, type, value);
  if (replayed) {
    auto* m = R::GetMutableMessage(out.data());
    auto* rep = static_cast<R::Reply*>(m->mutable_payload());
    switch (type) {
      case R::ReplyPayload::SaveResult:
        static_cast<R::SaveResult*>(rep->mutable_value())
            ->mutate_replayed(true);
        break;
      case R::ReplyPayload::RunAccepted:
        static_cast<R::RunAccepted*>(rep->mutable_value())
            ->mutate_replayed(true);
        break;
      default:
        break;
    }
  }
  return out;
}

bool Service::replay_or_join(RequestIn& req, const std::string& op_id,
                             const std::string& kind,
                             const std::string& digest) {
  if (!crypto::is_operation_id(op_id))
    throw Failed(FailureKind::invalid_request, "invalid op_id");
  const auto pending_digest = kind + ":" + digest;
  const auto join_pending = [&] {
    auto it = pending_ops_.find(op_id);
    if (it == pending_ops_.end()) return false;
    if (it->second.digest != pending_digest)
      throw Failed(FailureKind::operation_conflict,
                   "op_id is pending for a different request");
    it->second.waiters.emplace_back(req.conn, req.request_id);
    return true;
  };
  // The pending entry publishes before the durable shell uncertainty marker
  // and disappears after the final receipt. Read them in the same order.
  std::unique_lock lock(ops_mu_, std::defer_lock);
  if (kind == "shell") {
    lock.lock();
    if (join_pending()) return true;
  }
  std::optional<Receipt> rc = store_->receipt(op_id);
  if (!rc) {
    auto row = history_->read(
        [&](sql::Db& db) { return History::receipt(db, op_id); });
    if (row) rc = Receipt{op_id, row->kind, row->digest, row->bytes};
  }
  if (rc) {
    if (rc->kind != kind || rc->digest != digest)
      throw Failed(FailureKind::operation_conflict,
                   "op_id was already used for a different request",
                   {{"op_id", op_id}});
    reply(req, receipt_reply(req.request_id, rc->bytes, true));
    return true;
  }
  if (!lock.owns_lock()) lock.lock();
  if (join_pending()) return true;
  pending_ops_[op_id] = Pending{pending_digest, {}};
  return false;
}

void Service::finish_op(const std::string& op_id,
                        const std::string& receipt_bytes) {
  std::vector<std::pair<ConnId, uint64_t>> waiters;
  {
    std::lock_guard<std::mutex> l(ops_mu_);
    auto it = pending_ops_.find(op_id);
    if (it == pending_ops_.end()) return;
    waiters = std::move(it->second.waiters);
    pending_ops_.erase(it);
  }
  for (auto [conn, rid] : waiters)
    server_->reply(conn, receipt_reply(rid, receipt_bytes, true));
}

namespace {
auto operation_reply(uint64_t request_id, const std::string* bytes,
                     bool pending) -> std::string {
  FBB b;
  flatbuffers::Offset<R::OperationReceipt> receipt;
  if (bytes)
    receipt = flatbuffers::Offset<R::OperationReceipt>(
        wire::copy_table(
            b, "Tsugumi.RPC.OperationReceipt",
            reinterpret_cast<const flatbuffers::Table*>(
                flatbuffers::GetRoot<R::OperationReceipt>(bytes->data())))
            .o);
  const auto state = bytes     ? D::OperationState::completed
                     : pending ? D::OperationState::pending
                               : D::OperationState::unknown;
  auto result = R::CreateOperationResult(b, state, receipt);
  return wire::reply(b, request_id, R::ReplyPayload::OperationResult,
                     result.Union());
}

void check_frame(size_t size, std::string_view payload) {
  if (size > kMaxFrameBytes)
    throw Failed(FailureKind::resource_limit,
                 std::string(payload) + " exceeds the 16 MiB frame limit");
}

std::string failure_receipt(const std::string& op_id, const FailureData& f) {
  FBB b;
  auto fo = wire::failure(b, f);
  auto r = R::CreateOperationReceipt(b, b.CreateString(op_id),
                                     R::ReceiptValue::Failure, fo.Union());
  b.Finish(r);
  return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()),
                     b.GetSize());
}
}  // namespace

void Service::store_failure_receipt(const std::string& op_id,
                                    const std::string& kind,
                                    const std::string& digest,
                                    const FailureData& f) {
  // busy/timeout/persistence failures (and a save waiting for the user's
  // approval) leave the operation retryable under the same id.
  if (f.kind == FailureKind::busy || f.kind == FailureKind::timeout ||
      f.kind == FailureKind::approval_required ||
      f.kind == FailureKind::persistence_failed ||
      f.kind == FailureKind::internal_error) {
    finish_op(op_id, failure_receipt(op_id, f));
    return;
  }
  std::string bytes = failure_receipt(op_id, f);
  try {
    if (kind == "save" || kind == "reload")
      store_->record_receipt({op_id, kind, digest, bytes});
    else
      history_->write_sync([&](sql::Db& db) {
        History::put_receipt(db, op_id, kind, digest, bytes);
      });
  } catch (const std::exception& e) {
    log::error(std::string("cannot persist failure receipt: ") + e.what());
  }
  finish_op(op_id, bytes);
}

// ---------------------------------------------------------------------------
// Snapshots and deltas

namespace {

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<R::SourceInfo>>>
manifest(FBB& b, const Document& doc, const std::vector<std::string>* only) {
  std::vector<flatbuffers::Offset<R::SourceInfo>> v;
  auto add = [&](const std::string& p, const SourceEntry& e) {
    v.push_back(wire::source_info(b, p, D::SourceKind::file, true, e.hash,
                                  e.size, e.document.get()));
  };
  if (only) {
    for (const auto& p : *only) add(p, doc.sources.at(p));
  } else {
    for (const auto& [p, e] : doc.sources) add(p, e);
  }
  return b.CreateVector(v);
}

}  // namespace

std::optional<Service::ViewRecord> Service::load_view(
    const std::string& view_id) {
  std::lock_guard<std::mutex> l(view_mu_);
  auto bytes = fs::read_file(store_->state_dir() / "view");
  const auto file =
      bytes ? flat::decode<Tsugumi::State::Views>(*bytes) : std::nullopt;
  if (!file) return std::nullopt;
  for (const auto& entry : file->views)
    if (entry->id == view_id) return ViewRecord{entry->rev, std::string(entry->view.begin(), entry->view.end())};
  return std::nullopt;
}

auto Service::busy() -> bool {
  return !mutation_lane_->idle() || !read_pool_->idle() ||
         !cancel_lane_->idle() || !shell_lane_->idle() ||
         scheduler_->active_status().has_value();
}

RunStatusData Service::current_run_status() {
  if (auto s = scheduler_->active_status()) return *s;
  auto last =
      history_->read([](sql::Db& db) { return History::latest_run(db); });
  if (last) return last->status;
  return RunStatusData{};
}

flatbuffers::Offset<R::ProjectSnapshot> Service::snapshot(
    FBB& b, const Document& doc, const std::optional<std::string>& view_id,
    const ViewRecord* record) {
  std::vector<flatbuffers::Offset<R::Definition>> defs;
  for (const auto& [id, d] : doc.definitions)
    defs.push_back(wire::definition(b, id, d));
  auto defs_v = b.CreateVector(defs);
  auto src = manifest(b, doc, nullptr);
  auto res = wire::resolution(b, *doc.resolution);
  const auto targets = remote_targets();
  auto diagnostics = doc.resolution->diags;
  for (const auto& node : doc.resolution->nodes)
    if (auto diagnostic = execution_diagnostic(node, targets))
      diagnostics.push_back(std::move(*diagnostic));
  auto diags = wire::diagnostics(b, diagnostics);
  auto run = wire::run_status(b, current_run_status());
  auto pid = b.CreateString(store_->project_id());
  auto pj = b.CreateString(doc.project_json);
  auto lj = b.CreateVector(reinterpret_cast<const uint8_t*>(doc.layout.data()),
                           doc.layout.size());
  std::vector<flatbuffers::Offset<R::ExecutionService>> services;
  for (const auto& target : targets) {
    const bool remote = target.id != kLocalExecutionService;
    using Guarantee = R::IsolationGuarantee;
    auto isolation = remote || target.namespace_isolation
                         ? Guarantee::enforced
                         : Guarantee::unavailable;
    auto filesystem = isolation;
    auto network = isolation;
    const bool host_paths = !remote && target.namespace_isolation;
#ifdef __APPLE__
    if (!remote && worker_has_seatbelt()) {
      isolation = Guarantee::enforced;
      network = filesystem = Guarantee::partial;
    }
#endif
    const auto sandbox = R::CreateSandboxCapabilities(
        b, filesystem, isolation, isolation, isolation, network, target.gui,
        host_paths, remote, remote);
    services.push_back(R::CreateExecutionService(
        b, b.CreateString(target.id), b.CreateString(target.native_system),
        b.CreateVectorOfStrings(target.systems),
        remote ? R::ExecutionServiceKind::remote_daemon
               : R::ExecutionServiceKind::local,
        target.available, target.resource_limits,
        target.max_jobs ? flatbuffers::Optional(target.max_jobs)
                        : flatbuffers::nullopt,
        sandbox));
  }
  auto execution_services = b.CreateVector(services);
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> view;
  flatbuffers::Offset<flatbuffers::String> vid;
  std::optional<uint64_t> vrev;
  if (view_id) {
    vid = b.CreateString(*view_id);
    if (record) {
      view = b.CreateVector(reinterpret_cast<const uint8_t*>(record->view.data()),
                            record->view.size());
      vrev = record->rev;
    } else {
      vrev = 0;
    }
  }
  R::ProjectSnapshotBuilder sb(b);
  sb.add_project_id(pid);
  sb.add_rev(doc.rev);
  sb.add_execution_rev(doc.execution_rev);
  sb.add_project_json(pj);
  sb.add_layout(lj);
  sb.add_definitions(defs_v);
  sb.add_sources(src);
  sb.add_resolution(res);
  sb.add_diagnostics(diags);
  sb.add_run(run);
  sb.add_execution_services(execution_services);
  if (view_id) {
    if (!view.IsNull()) sb.add_view(view);
    sb.add_view_id(vid);
    sb.add_view_rev(*vrev);
  }
  return sb.Finish();
}

std::string Service::snapshot_reply(uint64_t request_id, const Document& doc,
                                    const std::optional<std::string>& view_id,
                                    bool open, const std::string& sub_id,
                                    const ViewRecord* view) {
  FBB b(1 << 16);
  auto snap = snapshot(b, doc, view_id, view);
  std::string out;
  if (open) {
    auto r = R::CreateOpenResult(b, snap, b.CreateString(sub_id), uint64_t(1));
    out = wire::reply(b, request_id, R::ReplyPayload::OpenResult, r.Union());
  } else {
    out = wire::reply(b, request_id, R::ReplyPayload::ProjectSnapshot,
                      snap.Union());
  }
  check_frame(out.size(), "the snapshot");
  return out;
}

void Service::check_snapshot(const Document& doc) {
  // The open envelope is larger than inspect and a view can grow independently
  // of the document. Reserve the full view payload allowed by ViewSave.
  static const ViewRecord largest_view{1, std::string(kMaxViewBytes, ' ')};
  snapshot_reply(1, doc, kExampleId, true, kExampleId, &largest_view);
}

namespace {

flatbuffers::Offset<R::DocumentDelta> delta_offset(FBB& b, const Delta& d) {
  const Document& doc = *d.doc;
  std::vector<flatbuffers::Offset<R::Definition>> ups;
  for (const auto& id : d.definitions_upsert)
    ups.push_back(wire::definition(b, id, doc.definitions.at(id)));
  auto ups_v = b.CreateVector(ups);
  std::vector<flatbuffers::Offset<flatbuffers::String>> rem;
  for (const auto& id : d.definitions_removed)
    rem.push_back(b.CreateString(id));
  auto rem_v = b.CreateVector(rem);
  auto src_v = manifest(b, doc, &d.sources_upsert);
  std::vector<flatbuffers::Offset<flatbuffers::String>> srem;
  for (const auto& p : d.sources_removed) srem.push_back(b.CreateString(p));
  auto srem_v = b.CreateVector(srem);
  auto res = wire::resolution(b, *doc.resolution);
  auto diags = wire::diagnostics(b, doc.resolution->diags);
  auto op = b.CreateString(d.op_id);
  auto origin = b.CreateString(d.origin);
  auto pj = d.project_changed ? b.CreateString(doc.project_json)
                              : flatbuffers::Offset<flatbuffers::String>();
  auto lj = d.layout_changed
                ? b.CreateVector(
                      reinterpret_cast<const uint8_t*>(doc.layout.data()),
                      doc.layout.size())
                : flatbuffers::Offset<flatbuffers::Vector<uint8_t>>();
  R::DocumentDeltaBuilder db(b);
  db.add_base_rev(d.base_rev);
  db.add_rev(d.rev);
  db.add_execution_rev(d.execution_rev);
  db.add_op_id(op);
  db.add_origin(origin);
  if (d.project_changed) db.add_project_json(pj);
  if (d.layout_changed) {
    db.add_layout(lj);
    db.add_layout_changed(true);
  }
  db.add_definitions_upsert(ups_v);
  db.add_definitions_removed(rem_v);
  db.add_sources_upsert(src_v);
  db.add_sources_removed(srem_v);
  db.add_resolution(res);
  db.add_diagnostics(diags);
  return db.Finish();
}

flatbuffers::Offset<R::SaveResult> save_result(
    FBB& b, const Delta* d, const Document& base,
    const flatbuffers::Vector<uint8_t>* operation_data = nullptr) {
  flatbuffers::Offset<R::DocumentDelta> delta;
  if (d) delta = delta_offset(b, *d);
  const auto metadata =
      operation_data
          ? b.CreateVector(operation_data->data(), operation_data->size())
          : flatbuffers::Offset<flatbuffers::Vector<uint8_t>>{};
  R::SaveResultBuilder sb(b);
  if (d) sb.add_delta(delta);
  sb.add_rev(d ? d->rev : base.rev);
  sb.add_execution_rev(d ? d->execution_rev : base.execution_rev);
  sb.add_replayed(false);
  if (operation_data) sb.add_operation_data(metadata);
  return sb.Finish();
}

std::string finish_receipt(FBB& b, const std::string& op_id,
                           R::ReceiptValue type, flatbuffers::Offset<void> v) {
  auto r = R::CreateOperationReceipt(b, b.CreateString(op_id), type, v);
  b.Finish(r);
  return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()),
                     b.GetSize());
}

auto doc_notification(FBB& b, const Delta& delta) {
  const auto d = delta_offset(b, delta);
  return std::make_pair(R::NotificationPayload::DocPatched,
                        R::CreateDocPatched(b, d).Union());
}

}  // namespace

auto Service::prepare_save_reply(
    Store::Staged& staged, const Document& base,
    const flatbuffers::Vector<uint8_t>* operation_data) -> PreparedSaveReply {
  store_->prepare_delta(staged);
  const auto* delta = staged.delta.doc ? &staged.delta : nullptr;
  FBB b(1 << 14);
  const auto result = save_result(b, delta, base, operation_data);
  auto receipt = finish_receipt(b, staged.delta.op_id,
                                R::ReceiptValue::SaveResult, result.Union());
  // Request ids have the same serialized width. Replay toggles a flag in
  // place. A receipt must fit both its direct reply and OperationGet's wrapper.
  check_frame(receipt_reply(1, receipt, true).size(), "the save reply");
  check_frame(operation_reply(1, &receipt, false).size(), "the save receipt");
  size_t notification_bytes = 0;
  if (delta) {
    check_snapshot(*delta->doc);
    // Match ProjectOpen's subscription id width and the notification encoder.
    FBB note;
    auto [type, value] = doc_notification(note, *delta);
    notification_bytes =
        Server::notification(note, kExampleId, 2, type, value).size();
    check_frame(notification_bytes, "the document notification");
  }
  return {std::move(receipt), notification_bytes};
}

void Service::publish_delta(const Delta& delta, size_t notification_bytes) {
  auto shared = std::make_shared<Delta>(delta);
  Note n;
  n.kind = Note::Kind::Doc;
  n.approx_bytes = notification_bytes;
  n.build = std::make_shared<const Note::Build>(
      [shared](FBB& b) { return doc_notification(b, *shared); });
  server_->broadcast(std::move(n));
}

void Service::check_at_rev(std::optional<uint64_t> at_rev,
                           const Document& doc) const {
  if (!at_rev) return;  // omitted: the latest revision
  if (*at_rev != doc.rev) {
    FailureData f = FailureData::make(FailureKind::revision_conflict,
                                      "the document is at another revision");
    f.current_rev = doc.rev;
    throw Failed(f);
  }
}

// ---------------------------------------------------------------------------
// Mutation lane

void Service::open_project(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ProjectOpenRequest();
  std::optional<std::string> view_id;
  if (r->view_id()) {
    if (!crypto::is_uuid(r->view_id()->str()))
      throw Failed(FailureKind::invalid_request, "view_id must be a UUID");
    view_id = r->view_id()->str();
  }
  std::string sub = crypto::uuid4();
  // Runs on the mutation lane: no save can commit between this snapshot and
  // the subscription becoming active, and the reply is queued before any later
  // notification.
  auto doc = store_->current();
  const auto view = view_id ? load_view(*view_id) : std::nullopt;
  std::string bytes = snapshot_reply(req.request_id, *doc, view_id, true, sub,
                                     view ? &*view : nullptr);
  {
    std::lock_guard<std::mutex> l(subs_mu_);
    if (req.connection_stop.stop_requested()) return;
    if (ui_connections_.contains(req.conn)) {
      if (ui_owner_ && ui_owner_ != req.conn)
        throw Failed(FailureKind::project_in_use,
                     "the project is open in another UI connection");
      ui_owner_ = req.conn;
    }
    end_views(req.conn);
    subscriptions_[req.conn] = sub;
    server_->subscribe(req.conn, sub, std::move(bytes));
  }
}

namespace {

std::vector<ChangeIn> decode_changes(
    const flatbuffers::Vector<flatbuffers::Offset<R::DocumentChange>>* list) {
  std::vector<ChangeIn> out;
  if (!list) return out;
  for (const auto* c : *list) {
    ChangeIn ch;
    switch (c->value_type()) {
      case R::DocumentChangeValue::SetProject:
        ch.kind = ChangeIn::Kind::SetProject;
        ch.json = c->value_as_SetProject()->json()->str();
        break;
      case R::DocumentChangeValue::SetProjectLock: {
        const auto* lock = c->value_as_SetProjectLock();
        ch.kind = ChangeIn::Kind::SetProjectLock;
        ch.path = lock->path()->str();
        if (lock->json()) ch.lock_json = lock->json()->str();
        if (lock->base_hash()) ch.base_hash = lock->base_hash()->str();
        break;
      }
      case R::DocumentChangeValue::SetLayout:
        ch.kind = ChangeIn::Kind::SetLayout;
        if (const auto* bytes = c->value_as_SetLayout()->layout())
          ch.json.assign(reinterpret_cast<const char*>(bytes->data()),
                         bytes->size());
        break;
      case R::DocumentChangeValue::PutDefinition: {
        const auto* p = c->value_as_PutDefinition();
        ch.kind = ChangeIn::Kind::PutDefinition;
        ch.id = p->id()->str();
        ch.json = p->card_json()->str();
        // An empty optional document is one not given (a model fills in every
        // field). Neither file is ever empty. The README and comments stay:
        // they may hold the user's intent, and leaving a field blank must not
        // remove that.
        if (p->readme() && p->readme()->size() > 0)
          ch.readme = p->readme()->str();
        ch.keep_readme =
            p->keep_readme() || (p->readme() && p->readme()->size() == 0);
        if (p->comments_json() && p->comments_json()->size() > 0)
          ch.comments_json = p->comments_json()->str();
        ch.keep_comments = p->keep_comments() ||
                           (p->comments_json() && p->comments_json()->size() == 0);
        break;
      }
      case R::DocumentChangeValue::RemoveDefinition:
        ch.kind = ChangeIn::Kind::RemoveDefinition;
        ch.id = c->value_as_RemoveDefinition()->id()->str();
        break;
      case R::DocumentChangeValue::PutSource: {
        const auto* p = c->value_as_PutSource();
        ch.kind = ChangeIn::Kind::PutSource;
        ch.path = p->path()->str();
        ch.bytes.assign(reinterpret_cast<const char*>(p->bytes()->data()),
                        p->bytes()->size());
        if (p->base_hash()) ch.base_hash = p->base_hash()->str();
        break;
      }
      case R::DocumentChangeValue::PatchSource: {
        const auto* p = c->value_as_PatchSource();
        ch.kind = ChangeIn::Kind::PatchSource;
        ch.path = p->path()->str();
        ch.base_hash = p->base_hash()->str();
        for (const auto* sp : *p->patches()) {
          if (!sp->offset() || !sp->remove())
            throw Failed(FailureKind::invalid_request,
                         "patch offset and remove are required");
          ch.patches.push_back(
              {*sp->offset(), *sp->remove(), sp->text()->str()});
        }
        break;
      }
      case R::DocumentChangeValue::RemoveSource: {
        const auto* p = c->value_as_RemoveSource();
        ch.kind = ChangeIn::Kind::RemoveSource;
        ch.path = p->path()->str();
        ch.base_hash = p->base_hash()->str();
        break;
      }
      default:
        throw Failed(FailureKind::invalid_request,
                     "unknown or missing document change variant");
    }
    out.push_back(std::move(ch));
  }
  return out;
}

auto require_base_rev(std::optional<uint64_t> base_rev) -> void {
  if (!base_rev)
    throw Failed(FailureKind::invalid_request, "base_rev is required");
}

auto RequireApprovedPermissions(
    const Resolution* before, const Resolution& after,
    const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>*
        approvals) -> void {
  std::set<std::string> approved;
  if (approvals)
    for (const auto* permission : *approvals)
      approved.insert(permission->str());
  std::vector<std::pair<std::string, std::string>> missing;
  for (const auto& widening : permissions::widenings(before, after))
    if (!approved.contains(widening))
      missing.emplace_back("permission", widening);
  if (!missing.empty())
    throw Failed(
        FailureKind::approval_required,
        "this change widens permissions absent from approved_permissions",
        std::move(missing));
}

}  // namespace

auto Service::card_id(RequestIn& req) -> void {
  const auto* r = project_request(req)->value_as_CardIdRequest();
  auto id = store_->allocate_card_id(
      r->stem()->str(), r->project_path() ? r->project_path()->str() : "");
  FBB b;
  auto result = R::CreateCardIdResult(b, b.CreateString(id));
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::CardIdResult,
                         result.Union()));
}

void Service::save(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ProjectSaveRequest();
  std::string op_id = r->op_id()->str();
  std::string digest = request_digest(project_request(req));
  if (replay_or_join(req, op_id, "save", digest)) return;
  try {
    auto check_cancelled = [&] {
      if (req.cancelled && req.cancelled->load())
        throw Failed(FailureKind::cancelled,
                     "the save was cancelled before committing");
    };
    check_cancelled();
    require_base_rev(r->base_rev());
    auto changes = decode_changes(r->changes());
    auto base = store_->current();
    if (*r->base_rev() != base->rev) {
      FailureData f = FailureData::make(FailureKind::revision_conflict,
                                        "the document changed since base_rev");
      f.current_rev = base->rev;
      throw Failed(f);
    }
    if (r->transfers())
      for (const auto* transfer : *r->transfers()) {
        ChangeIn change{};
        change.kind = ChangeIn::Kind::PutSource;
        change.path = transfer->path()->str();
        if (transfer->base_hash())
          change.base_hash = transfer->base_hash()->str();
        auto root = transfer->source_root()
                        ? std::filesystem::path(transfer->source_root()->str())
                        : opts_.root;
        if (!root.is_absolute())
          throw Failed(FailureKind::invalid_path,
                       "transfer root must be absolute");
        std::optional<std::string> expected;
        if (transfer->expected_hash())
          expected = transfer->expected_hash()->str();
        try {
          change.file = store_->stage_file(root, transfer->from()->str(),
                                           expected, check_cancelled);
        } catch (const fs::Error& error) {
          throw Failed(FailureKind::invalid_path, error.what());
        }
        changes.push_back(std::move(change));
      }
    auto staged = store_->stage(changes, op_id, r->origin()->str());
    if (opts_.fault == "save_throws")
      throw std::runtime_error("injected: not a Failed");
    // Compare permissions only when the caller requests this mechanism.
    if (r->check_permissions() && staged.effective) {
      store_->resolve_staged(staged);
      RequireApprovedPermissions(base->resolution.get(),
                                 *staged.doc->resolution,
                                 r->approved_permissions());
    }
    // The active run keeps executing the revision it started with; only the
    // definition files it still reads from the project folder must stay.
    if (staged.effective) require_unread_by_run(*base, *staged.doc);
    const auto response =
        prepare_save_reply(staged, *base, r->operation_data());
    complete_save(req, op_id, "save", digest, *base, staged, response,
                  check_cancelled);
  } catch (const Failed& f) {
    // The caller can explicitly retry a host operation after a revision
    // conflict. No write happened, so the operation ID remains available.
    if (r->save_intent() && f.failure.kind == FailureKind::revision_conflict)
      finish_op(op_id, failure_receipt(op_id, f.failure));
    else
      store_failure_receipt(op_id, "save", digest, f.failure);
    throw;
  } catch (const std::exception& e) {
    // Any other error ends the operation too (retryable under the same id).
    auto f = FailureData::make(FailureKind::internal_error, e.what());
    store_failure_receipt(op_id, "save", digest, f);
    throw Failed(f);
  }
}

namespace {
auto changed_sources(const Document& before, const Document& after)
    -> std::vector<std::string> {
  std::vector<std::string> out;
  auto a = before.sources.begin();
  auto b = after.sources.begin();
  while (a != before.sources.end() || b != after.sources.end()) {
    if (b == after.sources.end() ||
        (a != before.sources.end() && a->first < b->first)) {
      out.push_back((a++)->first);
    } else if (a == before.sources.end() || b->first < a->first) {
      out.push_back((b++)->first);
    } else {
      if (a->second.hash != b->second.hash) out.push_back(a->first);
      ++a;
      ++b;
    }
  }
  return out;
}

// Files the evaluator never reads: the document supplies them to the run.
auto evaluated_definition_file(std::string_view name) -> bool {
  return name != "_card.json" && name != "README.md" &&
         name != "_comments.json" && name != "_inputs.fbs";
}
}  // namespace

void Service::require_unread_by_run(const Document& before,
                                    const Document& after) {
  const auto reads = scheduler_->active_reads();
  if (!reads) return;
  const auto refuse = [&](const std::string& path, const std::string& what,
                          std::optional<std::string> card) {
    std::vector<std::pair<std::string, std::string>> args{
        {"run_id", reads->run_id}, {"path", path}};
    if (card) args.emplace_back("card", *card);
    throw Failed(FailureKind::busy,
                 "run " + reads->run_id + " still reads " + what +
                     " from the project folder; save this change after the "
                     "run ends, or cancel the run",
                 args);
  };
  for (const auto& path : changed_sources(before, after)) {
    if (reads->locks.contains(path)) refuse(path, path, std::nullopt);
    auto it = reads->definitions.upper_bound(path);
    if (it == reads->definitions.begin()) continue;
    --it;
    if (!path.starts_with(it->first) ||
        !evaluated_definition_file(path.substr(it->first.size())))
      continue;
    const auto card = path_key(it->second);
    refuse(path, "the definition of " + card, card);
  }
}

void Service::complete_save(RequestIn& req, const std::string& op_id,
                            const std::string& kind, const std::string& digest,
                            const Document& base, Store::Staged& staged,
                            const PreparedSaveReply& response,
                            const std::function<void()>& check_cancelled) {
  store_->commit(
      staged, kind, digest, [&](const Delta*) { return response.receipt; },
      check_cancelled);
  if (staged.delta.doc) scheduler_->document_changed(base, *staged.delta.doc);
  reply(req, receipt_reply(req.request_id, response.receipt, false));
  if (staged.delta.doc) {
    publish_delta(staged.delta, response.notification_bytes);
  }
  finish_op(op_id, response.receipt);
}

void Service::reload(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ProjectReloadRequest();
  std::string op_id = r->op_id()->str();
  std::string digest = request_digest(project_request(req));
  if (replay_or_join(req, op_id, "reload", digest)) return;
  try {
    require_base_rev(r->base_rev());
    auto base = store_->current();
    if (*r->base_rev() != base->rev) {
      FailureData f = FailureData::make(FailureKind::revision_conflict,
                                        "the document changed since base_rev");
      f.current_rev = base->rev;
      throw Failed(f);
    }
    if (auto active = scheduler_->active_status())
      throw Failed(FailureKind::busy, "a run is in progress",
                   {{"run_id", *active->run_id}});
    auto staged = store_->stage_reload(op_id, r->origin()->str());
    if (r->check_permissions() && staged.effective) {
      store_->resolve_staged(staged);
      RequireApprovedPermissions(base->resolution.get(),
                                 *staged.doc->resolution,
                                 r->approved_permissions());
    }
    const auto response = prepare_save_reply(staged, *base);
    complete_save(req, op_id, "reload", digest, *base, staged, response);
  } catch (const Failed& f) {
    store_failure_receipt(op_id, "reload", digest, f.failure);
    throw;
  } catch (const std::exception& e) {
    // Any other error ends the operation too (retryable under the same id).
    auto f = FailureData::make(FailureKind::internal_error, e.what());
    store_failure_receipt(op_id, "reload", digest, f);
    throw Failed(f);
  }
}

// ---------------------------------------------------------------------------
// Versions (git): tsugumid/runtime/src/versions.cc

namespace {
std::string version_receipt(const std::string& op_id, const std::string& commit,
                            const std::string& branch) {
  FBB b;
  auto vr = R::CreateVersionResult(b, b.CreateString(commit),
                                   branch.empty() ? 0 : b.CreateString(branch));
  auto rc = R::CreateOperationReceipt(
      b, b.CreateString(op_id), R::ReceiptValue::VersionResult, vr.Union());
  b.Finish(rc);
  return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()),
                     b.GetSize());
}
}  // namespace

void Service::note_run_end(const RunStatusData& s,
                           const std::string& facts) {
  if (!s.commit || !s.run_id || !s.terminal()) return;
  VersionRunNote n;
  n.run_id = *s.run_id;
  n.state = Tsugumi::Data::EnumNameRunState(s.state);
  int64_t end = s.ended_at.value_or(0);
  int64_t start = s.started_at.value_or(s.accepted_at.value_or(end));
  n.ended = Versions::iso_utc(end);
  n.seconds = end > start ? static_cast<double>(end - start) / 1000.0 : 0;
  n.facts = facts;
  try {
    versions_->note_run(*s.commit, n);
  } catch (const std::exception& e) {
    log::warn(std::string("versions: no note for run ") + *s.run_id + ": " +
              e.what());
  }
}

void Service::version_init(RequestIn& req) {
  version_operation(req, [&] { return versions_->init(); });
}

void Service::version_mark(RequestIn& req) {
  const auto* r = project_request(req)->value_as_VersionMarkRequest();
  version_operation(req, [&] {
    if (!versions_->enabled())
      throw Failed(FailureKind::invalid_request,
                   "the project is not versioned yet");
    return versions_->mark(r->name()->str(),
                           r->message() ? r->message()->str() : "");
  });
}

auto Service::version_operation(RequestIn& req,
                                const std::function<std::string()>& operation)
    -> void {
  const auto* request = project_request(req);
  const auto* id = request->value_as_VersionInitRequest()
                       ? request->value_as_VersionInitRequest()->op_id()
                       : request->value_as_VersionMarkRequest()->op_id();
  const auto op_id = id->str();
  const auto digest = request_digest(request);
  if (replay_or_join(req, op_id, "version", digest)) return;
  try {
    const auto commit = operation();
    auto receipt = version_receipt(op_id, commit, versions_->graph(1).branch);
    history_->write_sync([&](sql::Db& db) {
      History::put_receipt(db, op_id, "version", digest, receipt);
    });
    reply(req, receipt_reply(req.request_id, receipt, false));
    finish_op(op_id, receipt);
  } catch (const Failed& f) {
    store_failure_receipt(op_id, "version", digest, f.failure);
    throw;
  } catch (const std::exception& e) {
    auto f = FailureData::make(FailureKind::internal_error, e.what());
    store_failure_receipt(op_id, "version", digest, f);
    throw Failed(f);
  }
}

void Service::version_checkout(RequestIn& req) {
  const auto* r = project_request(req)->value_as_VersionCheckoutRequest();
  std::string op_id = r->op_id()->str();
  std::string digest = request_digest(project_request(req));
  if (replay_or_join(req, op_id, "reload", digest)) return;
  try {
    require_base_rev(r->base_rev());
    auto base = store_->current();
    if (*r->base_rev() != base->rev) {
      FailureData f = FailureData::make(FailureKind::revision_conflict,
                                        "the document changed since base_rev");
      f.current_rev = base->rev;
      throw Failed(f);
    }
    if (store_->halted())
      throw Failed(FailureKind::persistence_failed,
                   "reopen the project to recover persistence");
    if (auto active = scheduler_->active_status())
      throw Failed(FailureKind::busy, "a run is in progress",
                   {{"run_id", *active->run_id}});
    if (!versions_->enabled())
      throw Failed(FailureKind::invalid_request,
                   "the project is not versioned yet");
    Store::Staged staged;
    PreparedSaveReply response;
    versions_->checkout(r->commit()->str(), [&](const stdfs::path& target) {
      staged = store_->stage_reload(op_id, r->origin()->str(), target);
      response = prepare_save_reply(staged, *base);
    }, opts_.fault);
    complete_save(req, op_id, "reload", digest, *base, staged, response);
  } catch (const Failed& f) {
    store_failure_receipt(op_id, "reload", digest, f.failure);
    throw;
  } catch (const std::exception& e) {
    auto f = FailureData::make(FailureKind::internal_error, e.what());
    store_failure_receipt(op_id, "reload", digest, f);
    throw Failed(f);
  }
}

namespace {
auto pack_comments(FBB& b, const std::vector<comments::Change>& changes) {
  std::vector<flatbuffers::Offset<R::VersionComment>> entries;
  for (const auto& c : changes)
    entries.push_back(R::CreateVersionComment(
        b, b.CreateString(c.card), b.CreateString(c.text),
        b.CreateString(c.change),
        c.resolution.empty() ? 0 : b.CreateString(c.resolution),
        c.by.empty() ? 0 : b.CreateString(c.by),
        b.CreateVectorOfStrings(c.cards)));
  return b.CreateVector(entries);
}
}  // namespace

void Service::version_graph(RequestIn& req) {
  const auto* r = project_request(req)->value_as_VersionGraphRequest();
  auto g = versions_->graph(std::min<uint32_t>(r->limit().value_or(500), 5000),
                            r->include_comments());
  FBB b(1 << 16);
  auto strings = [&](const std::vector<std::string>& v) {
    std::vector<flatbuffers::Offset<flatbuffers::String>> o;
    for (const auto& s : v) o.push_back(b.CreateString(s));
    return b.CreateVector(o);
  };
  std::vector<flatbuffers::Offset<R::VersionCommit>> commits;
  for (const auto& c : g.commits) {
    std::vector<flatbuffers::Offset<R::VersionRun>> runs;
    for (const auto& n : c.runs) {
      const auto facts = version_facts::run(n.facts);
      runs.push_back(R::CreateVersionRun(
          b, b.CreateString(n.run_id), b.CreateString(n.state),
          b.CreateString(n.ended), n.seconds, 0,
          facts ? R::VersionRunFacts::Pack(b, facts.get()) : 0));
    }
    const auto facts = version_facts::commit(c.message);
    commits.push_back(R::CreateVersionCommit(
        b, b.CreateString(c.id), strings(c.parents),
        b.CreateString(facts ? "" : c.summary),
        b.CreateString(facts ? "" : c.message), b.CreateString(c.author),
        c.time_ms, strings(c.branches), strings(c.tags), b.CreateVector(runs),
        strings(c.changed),
        facts ? R::VersionCommitFacts::Pack(b, facts.get()) : 0,
        r->include_comments() ? pack_comments(b, c.comments) : 0));
  }
  auto vg = R::CreateVersionGraph(b, g.enabled, b.CreateString(g.head),
                                  b.CreateString(g.branch), g.dirty,
                                  b.CreateVector(commits));
  auto bytes =
      wire::reply(b, req.request_id, R::ReplyPayload::VersionGraph, vg.Union());
  check_frame(bytes.size(), "the version graph");
  reply(req, std::move(bytes));
}

void Service::version_canvas(RequestIn& req) {
  const auto* r = project_request(req)->value_as_VersionCanvasRequest();
  auto canvas = versions_->canvas(r->commit()->str());
  FBB b;
  std::vector<flatbuffers::Offset<R::VersionCanvasCard>> cards;
  for (const auto& c : canvas.cards)
    cards.push_back(R::CreateVersionCanvasCard(b, b.CreateString(c.id),
                                               b.CreateString(c.kind), c.fan));
  std::vector<flatbuffers::Offset<R::VersionCanvasWire>> wires;
  for (const auto& [from, to] : canvas.wires)
    wires.push_back(R::CreateVersionCanvasWire(b, b.CreateString(from),
                                               b.CreateString(to)));
  auto vc =
      R::CreateVersionCanvas(b, b.CreateVector(cards), b.CreateVector(wires),
                             b.CreateVector(reinterpret_cast<const uint8_t*>(
                                                canvas.layout.data()),
                                            canvas.layout.size()));
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::VersionCanvas,
                         vc.Union()));
}

void Service::version_note(RequestIn& req) {
  const auto* r = project_request(req)->value_as_VersionNoteRequest();
  auto note = versions_->note(r->commit()->str());
  FBB b;
  auto facts = version_facts::changes(note.facts);
  auto n = R::CreateVersionNote(b, R::VersionChanges::Pack(b, &facts),
                                b.CreateString(note.commit));
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::VersionNote,
                         n.Union()));
}

void Service::version_diff(RequestIn& req) {
  const auto* r = project_request(req)->value_as_VersionDiffRequest();
  const auto diff = versions_->diff(
      r->commit()->str(), r->from_commit() ? r->from_commit()->str() : "");
  FBB b;
  const auto value =
      R::CreateVersionDiff(b, b.CreateString(diff.text), diff.truncated);
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::VersionDiff,
                         value.Union()));
}

void Service::version_comments(RequestIn& req) {
  const auto* r = project_request(req)->value_as_VersionCommentsRequest();
  FBB b;
  const auto value = R::CreateVersionComments(
      b, pack_comments(b, versions_->comment_changes(r->commit()->str())));
  auto bytes = wire::reply(b, req.request_id, R::ReplyPayload::VersionComments,
                           value.Union());
  check_frame(bytes.size(), "the version comments");
  reply(req, std::move(bytes));
}

void Service::cache_clear(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ProjectCacheClearRequest();
  std::string op_id = r->op_id()->str();
  std::string digest = request_digest(project_request(req));
  if (replay_or_join(req, op_id, "cache_clear", digest)) return;
  try {
    if (scheduler_->active_status())
      throw Failed(FailureKind::busy,
                   "cache can only be cleared while no run is active");
    if (history_->read(History::has_remote_jobs))
      throw Failed(
          FailureKind::busy,
          "cache cannot be cleared while external job results are pending");
    execution_service_
        ->clear();  // resident workers hold the builds being removed
    {
      std::lock_guard lock(worker_selection_mutex_);
      resident_targets_.clear();
    }
    const auto result = nix_->clear_cache();
    const auto roots = opts_.gcroots_dir.empty()
                           ? store_->state_dir() / "cache" / "gcroots"
                           : opts_.gcroots_dir;
    fs::write_atomic(roots / ".name", opts_.root.filename().string(), 0644);
    FBB b;
    std::vector<flatbuffers::Offset<flatbuffers::String>> del;
    std::vector<flatbuffers::Offset<flatbuffers::String>> ret;
    for (const auto& d : result.deleted) del.push_back(b.CreateString(d));
    for (const auto& d : result.retained) ret.push_back(b.CreateString(d));
    auto cr = R::CreateProjectCacheClearResult(
        b, b.CreateVector(del), b.CreateVector(ret), result.freed_bytes);
    std::string receipt = finish_receipt(
        b, op_id, R::ReceiptValue::ProjectCacheClearResult, cr.Union());
    history_->write_sync([&](sql::Db& db) {
      History::put_receipt(db, op_id, "cache_clear", digest, receipt);
    });
    reply(req, receipt_reply(req.request_id, receipt, false));
    finish_op(op_id, receipt);
  } catch (const Failed& f) {
    store_failure_receipt(op_id, "cache_clear", digest, f.failure);
    throw;
  } catch (const std::exception& e) {
    // Any other error ends the operation too (retryable under the same id).
    auto f = FailureData::make(FailureKind::internal_error, e.what());
    store_failure_receipt(op_id, "cache_clear", digest, f);
    throw Failed(f);
  }
}

void Service::cache_usage(RequestIn& req) {
  R::ProjectCacheUsageResultT result;
  if (cache_usage_)
    result = cache_usage_->snapshot();
  else if (auto bytes = fs::read_file(opts_.usage_file)) {
    flatbuffers::Verifier verifier(
        reinterpret_cast<const uint8_t*>(bytes->data()), bytes->size());
    if (!verifier.VerifyBuffer<R::ProjectCacheUsageResult>(nullptr))
      throw Failed(FailureKind::integrity_error,
                   "invalid cache usage snapshot");
    flatbuffers::GetRoot<R::ProjectCacheUsageResult>(bytes->data())
        ->UnPackTo(&result);
  } else
    result.error = "Measuring cache usage";
  result.current_project =
      opts_.gcroots_dir.empty() ||
              (opts_.usage_roots_dir.empty() && opts_.usage_file.empty())
          ? project_id()
          : opts_.gcroots_dir.filename().string();
  if (auto latest = history_->read(History::latest_run))
    result.protected_run = *latest->status.run_id;
  FBB builder;
  auto value = R::ProjectCacheUsageResult::Pack(builder, &result);
  reply(req,
        wire::reply(builder, req.request_id,
                    R::ReplyPayload::ProjectCacheUsageResult, value.Union()));
}

void Service::scratch_delete(RequestIn& req) {
  const auto* r = project_request(req)->value_as_RunScratchDeleteRequest();
  const auto op_id = r->op_id()->str();
  const auto digest = request_digest(project_request(req));
  if (replay_or_join(req, op_id, "scratch_delete", digest)) return;
  try {
    if (scheduler_->active_status() || history_->read(History::has_remote_jobs))
      throw Failed(FailureKind::busy,
                   "scratch cannot be deleted while a run "
                   "or external results are pending");
    const auto latest = history_->read(History::latest_run);
    const auto protected_path = latest ? "runs/" + *latest->status.run_id : "";
    std::set<std::string> paths;
    // Validate the complete selection before deleting any part of it.
    for (const auto* item : *r->paths()) {
      if (!item)
        throw Failed(FailureKind::invalid_request, "scratch folder is missing");
      const auto path = item->str();
      if (!(path.starts_with("runs/") && crypto::is_run_id(path.substr(5))))
        throw Failed(FailureKind::invalid_request, "invalid scratch folder");
      // Canonical decimal names have no case aliases.
      if (std::ranges::equal(path, protected_path,
                             [](unsigned char a, unsigned char b) {
                               return std::tolower(a) == std::tolower(b);
                             }))
        throw Failed(FailureKind::busy,
                     "the latest run's scratch is kept for reruns");
      paths.insert(path);
    }
    uint64_t freed = 0;
    std::map<std::string, uint64_t> sizes;
    try {
      for (const auto& entry : measure_run_scratch(run_scratch_))
        sizes[entry.path] = entry.bytes.value_or(0);
    } catch (const std::filesystem::filesystem_error&) {
      // Measurement is advisory; removal repairs Card-created permissions.
    }
    std::vector<flatbuffers::Offset<flatbuffers::String>> deleted, retained;
    FBB b;
    for (const auto& path : paths) {
      if (remove_run_scratch(run_scratch_ / path)) {
        deleted.push_back(b.CreateString(path));
        freed += sizes[path];
      } else
        retained.push_back(b.CreateString(path));
    }
    auto result = R::CreateRunScratchDeleteResult(
        b, b.CreateVector(deleted), b.CreateVector(retained), freed);
    auto receipt = finish_receipt(
        b, op_id, R::ReceiptValue::RunScratchDeleteResult, result.Union());
    history_->write_sync([&](sql::Db& db) {
      History::put_receipt(db, op_id, "scratch_delete", digest, receipt);
    });
    reply(req, receipt_reply(req.request_id, receipt, false));
    finish_op(op_id, receipt);
  } catch (const Failed& f) {
    store_failure_receipt(op_id, "scratch_delete", digest, f.failure);
    throw;
  } catch (const std::exception& e) {
    auto failure = FailureData::make(FailureKind::persistence_failed, e.what());
    store_failure_receipt(op_id, "scratch_delete", digest, failure);
    throw Failed(failure);
  }
}

void Service::history_category_delete(RequestIn& req) {
  const auto* r = project_request(req)->value_as_RunCategoryDeleteRequest();
  const auto op_id = r->op_id()->str();
  const auto run_id = r->run_id()->str();
  const auto category = r->category();
  constexpr auto kind = "history_category_delete";
  const auto digest = request_digest(project_request(req));
  if (replay_or_join(req, op_id, kind, digest)) return;
  try {
    if (!crypto::is_run_id(run_id))
      throw Failed(FailureKind::invalid_request, "invalid run id");
    if (category != R::RunRecordCategory::Logs &&
        category != R::RunRecordCategory::Values &&
        category != R::RunRecordCategory::Artifacts)
      throw Failed(FailureKind::invalid_request, "invalid run record category");
    if (scheduler_->active_status() || history_->read(History::has_remote_jobs))
      throw Failed(FailureKind::busy,
                   "history cannot be deleted while a run or external results are pending");
    const auto run = history_->read([&](sql::Db& db) {
      return History::run(db, run_id);
    });
    if (!run) throw Failed(FailureKind::not_found, "unknown run");
    if (!run->status.terminal())
      throw Failed(FailureKind::busy, "the run has not finished");
    FBB b;
    const auto result = R::CreateRunCategoryDeleteResult(
        b, b.CreateString(run_id), category);
    const auto receipt = finish_receipt(
        b, op_id, R::ReceiptValue::RunCategoryDeleteResult, result.Union());
    history_->write_sync([&](sql::Db& db) {
      History::delete_category(db, run_id, category);
      History::put_receipt(db, op_id, kind, digest, receipt);
    });
    if (category == R::RunRecordCategory::Artifacts)
      remove_run_artifacts(run_id);
    Note note;
    note.build = std::make_shared<const Note::Build>([run_id, category](FBB& b) {
      return std::pair(R::NotificationPayload::RunCategoryDeleteResult,
                       R::CreateRunCategoryDeleteResult(
                           b, b.CreateString(run_id), category).Union());
    });
    server_->broadcast(std::move(note));
    reply(req, receipt_reply(req.request_id, receipt, false));
    finish_op(op_id, receipt);
  } catch (const Failed& error) {
    store_failure_receipt(op_id, kind, digest, error.failure);
    throw;
  } catch (const std::exception& error) {
    auto failure = FailureData::make(FailureKind::persistence_failed, error.what());
    store_failure_receipt(op_id, kind, digest, failure);
    throw Failed(failure);
  }
}

void Service::history_delete(RequestIn& req) {
  const auto* r = project_request(req)->value_as_RunHistoryDeleteRequest();
  std::string op_id = r->op_id()->str();
  std::string digest = request_digest(project_request(req));
  if (replay_or_join(req, op_id, "history_delete", digest)) return;
  try {
    // A later run may read files of earlier ones (saved inputs, upstream
    // outputs).
    if (scheduler_->active_status())
      throw Failed(FailureKind::busy,
                   "run history can only be deleted while no run is active");
    std::optional<std::vector<std::string>> run_ids;
    if (r->restrict_to_run_ids()) {
      run_ids.emplace();
      if (r->run_ids())
        for (const auto* id : *r->run_ids()) run_ids->push_back(id->str());
    } else if (r->run_ids() && r->run_ids()->size()) {
      throw Failed(FailureKind::invalid_request,
                   "run_ids requires restrict_to_run_ids");
    }
    auto sizes = [&] {
      uint64_t n = 0;
      std::error_code ec;
      for (const auto& f :
           {history_->path(), stdfs::path(history_->path().string() + "-wal")})
        if (stdfs::is_regular_file(f, ec)) n += stdfs::file_size(f, ec);
      return n;
    };
    uint64_t before = sizes();
    uint64_t files = 0;
    auto ids = history_->delete_runs(r->ended_before(), run_ids);
    for (const auto& id : ids) {
      if (!crypto::is_run_id(id))
        continue;  // a run id from a copy names no folder of ours
      const auto runs = run_scratch_ / "runs";
      const auto scratch = runs / id;
      if (stdfs::is_directory(stdfs::symlink_status(runs)) &&
          stdfs::is_directory(stdfs::symlink_status(scratch))) {
        const auto scratch_size = fs::directory_size(scratch);
        if (remove_run_scratch(scratch)) files += scratch_size;
      }
      files += remove_run_artifacts(id);
    }
    uint64_t after = sizes();
    FBB b;
    auto dr = R::CreateRunHistoryDeleteResult(
        b, ids.size(), (before > after ? before - after : 0) + files);
    std::string receipt = finish_receipt(
        b, op_id, R::ReceiptValue::RunHistoryDeleteResult, dr.Union());
    history_->write_sync([&](sql::Db& db) {
      History::put_receipt(db, op_id, "history_delete", digest, receipt);
    });
    reply(req, receipt_reply(req.request_id, receipt, false));
    finish_op(op_id, receipt);
    log::info("history: deleted " + std::to_string(ids.size()) + " runs");
  } catch (const Failed& f) {
    store_failure_receipt(op_id, "history_delete", digest, f.failure);
    throw;
  } catch (const std::exception& e) {
    auto f = FailureData::make(
        FailureKind::persistence_failed,
        std::string("deleting run history failed: ") + e.what());
    store_failure_receipt(op_id, "history_delete", digest, f);
    throw Failed(f);
  }
}

// ---------------------------------------------------------------------------
// Read pool: documents, files, views

auto Service::project_host_requirements(RequestIn& req) -> void {
  const auto doc = store_->current();
  auto value = host_requirements(*doc->resolution, doc->rev);
  flatbuffers::FlatBufferBuilder b;
  auto result = Tsugumi::RPC::ProjectHostRequirements::Pack(b, &value);
  reply(req, wire::reply(b, req.request_id,
                         Tsugumi::RPC::ReplyPayload::ProjectHostRequirements,
                         result.Union()));
}

void Service::inspect(RequestIn& req) {
  auto doc = store_->current();
  reply(req, snapshot_reply(req.request_id, *doc, std::nullopt, false, ""));
}

void Service::validate(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ProjectValidateRequest();
  auto doc = store_->current();
  check_at_rev(r->at_rev(), *doc);
  if (r->changes() && r->changes()->size() > 0) {
    // The document as it would be after the changes; nothing is written.
    doc = store_->preview(decode_changes(r->changes()));
  }
  auto mode = r->mode();
  if (mode == D::ValidateMode::unspecified)
    mode = D::ValidateMode::executable;
  else if (mode != D::ValidateMode::structure &&
           mode != D::ValidateMode::executable)
    throw Failed(FailureKind::invalid_request, "unknown validate mode");
  bool exec = mode == D::ValidateMode::executable;
  const auto services = remote_targets();
  std::vector<Diag> diags;
  if (!r->targets() || r->targets()->size() == 0) {
    diags = doc->resolution->all_diagnostics(exec);
    for (const auto& node : doc->resolution->nodes)
      if (auto diagnostic = execution_diagnostic(node, services))
        diags.push_back(std::move(*diagnostic));
  } else {
    diags = doc->resolution->diags;
    for (const auto* t : *r->targets()) {
      auto p = wire::card_arg(t, "targets");
      const auto* n = doc->resolution->find(p);
      if (!n)
        throw Failed(FailureKind::not_found, "unknown card " + path_key(p),
                     {{"card", path_key(p)}});
      diags.insert(diags.end(), n->diags.begin(), n->diags.end());
      if (exec)
        diags.insert(diags.end(), n->exec_diags.begin(), n->exec_diags.end());
      if (auto diagnostic = execution_diagnostic(*n, services))
        diags.push_back(std::move(*diagnostic));
    }
  }
  bool valid = std::none_of(diags.begin(), diags.end(),
                            [](const Diag& d) { return d.error; });
  FBB b;
  auto dv = wire::diagnostics(b, diags);
  R::ProjectValidateResultBuilder vb(b);
  vb.add_rev(doc->rev);
  vb.add_valid(valid);
  vb.add_diagnostics(dv);
  auto off = vb.Finish();
  reply(req, wire::reply(b, req.request_id,
                         R::ReplyPayload::ProjectValidateResult, off.Union()));
}

void Service::operation_get(RequestIn& req) {
  const auto* r = project_request(req)->value_as_OperationGetRequest();
  std::string op_id = r->op_id()->str();
  if (!crypto::is_operation_id(op_id))
    throw Failed(FailureKind::invalid_request, "invalid op_id");
  std::optional<std::string> bytes;
  bool shell_receipt = false;
  if (auto rc = store_->receipt(op_id)) {
    if (r->save_intent() &&
        (rc->kind != "save" ||
         rc->digest !=
             crypto::sha256_hex("save-intent\n" + r->save_intent()->str())))
      throw Failed(FailureKind::operation_conflict,
                   "op_id was already used for a different request");
    bytes = rc->bytes;
  }
  if (!bytes) {
    auto row = history_->read(
        [&](sql::Db& db) { return History::receipt(db, op_id); });
    if (row) {
      bytes = row->bytes;
      shell_receipt = row->kind == "shell";
    }
  }
  bool pending = false;
  if (!bytes || shell_receipt) {
    std::lock_guard<std::mutex> l(ops_mu_);
    pending = pending_ops_.count(op_id) > 0;
  }
  if (shell_receipt && pending) bytes.reset();
  reply(req,
        operation_reply(req.request_id, bytes ? &*bytes : nullptr, pending));
}

namespace {

void check_path_arg(std::string& p, bool allow_root) {
  if (allow_root && p.empty()) p = ".";  // "" names the project folder too
  if (!fs::valid_relative_path(p, allow_root))
    throw Failed(FailureKind::invalid_path, "invalid path " + p, {{"path", p}});
}

uint32_t chunk_limit(std::optional<uint32_t> v, uint32_t def, uint32_t lo,
                     uint32_t hi) {
  uint32_t x = v.value_or(def);
  if (x < lo || x > hi)
    throw Failed(FailureKind::invalid_request,
                 "limit must be between " + std::to_string(lo) + " and " +
                     std::to_string(hi));
  return x;
}

}  // namespace

void Service::file_list(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ProjectFileListRequest();
  auto doc = store_->current();
  check_at_rev(r->at_rev(), *doc);
  std::string path = r->path()->str();
  check_path_arg(path, true);
  uint32_t depth = r->depth().value_or(0);
  if (depth > 8)
    throw Failed(FailureKind::invalid_request, "depth must be 0..8");
  bool dirs = r->include_directories().value_or(false);
  uint32_t limit = chunk_limit(r->limit(), 100, 1, 500);
  // Entries visible through File APIs: the confirmed sources and document
  // files.
  std::set<std::string> files;
  for (const auto& [p, _] : doc->sources) files.insert(p);
  files.insert("tsugumi.json");
  if (!doc->layout.empty()) files.insert("layout.bin");
  for (const auto& [id, d] : doc->definitions) {
    files.insert("definitions/" + id + "/_card.json");
    if (d.readme) files.insert("definitions/" + id + "/README.md");
    if (d.comments_json) files.insert("definitions/" + id + "/_comments.json");
  }
  std::string prefix = path == "." ? "" : path + "/";
  std::map<std::string, bool> entries;  // path -> is_directory
  for (const auto& f : files) {
    if (!prefix.empty() && !f.starts_with(prefix)) continue;
    std::string rest = f.substr(prefix.size());
    size_t level = 0;
    size_t pos = 0;
    while (true) {
      size_t slash = rest.find('/', pos);
      if (slash == std::string::npos) {
        if (level <= depth) entries[f] = false;
        break;
      }
      if (dirs && level <= depth)
        entries[prefix + rest.substr(0, slash)] = true;
      ++level;
      pos = slash + 1;
      if (level > depth) break;
    }
  }
  if (!prefix.empty() && entries.empty() && !files.count(path))
    throw Failed(FailureKind::not_found, path + " does not exist",
                 {{"path", path}});
  std::string after;
  if (r->cursor()) after = history_->open_cursor(r->cursor()->str()).second;
  FBB b;
  std::vector<flatbuffers::Offset<R::SourceInfo>> out;
  std::string last;
  bool more = false;
  size_t bytes = 0;
  for (auto it = entries.upper_bound(after); it != entries.end(); ++it) {
    if (out.size() >= limit || bytes > (1u << 20) - 4096) {
      more = true;
      break;
    }
    const auto& [p, is_dir] = *it;
    std::optional<std::string> hash;
    std::optional<uint64_t> size;
    if (!is_dir) {
      if (auto s = doc->sources.find(p); s != doc->sources.end()) {
        hash = s->second.hash;
        size = s->second.size;
      } else if (p == "tsugumi.json") {
        hash = crypto::sha256_hex(doc->project_json);
        size = doc->project_json.size();
      } else if (p == "layout.bin") {
        hash = crypto::sha256_hex(doc->layout);
        size = doc->layout.size();
      } else {
        auto id = p.substr(12, p.find('/', 12) - 12);
        const auto& d = doc->definitions.at(id);
        const auto& text = p.ends_with("_card.json") ? d.card_json
                           : p.ends_with("/README.md") ? *d.readme
                                                       : *d.comments_json;
        hash = crypto::sha256_hex(text);
        size = text.size();
      }
    }
    out.push_back(wire::source_info(
        b, p, is_dir ? D::SourceKind::directory : D::SourceKind::file, true,
        hash, size));
    bytes += p.size() + 120;
    last = p;
  }
  auto ev = b.CreateVector(out);
  auto cur = more ? b.CreateString(history_->make_cursor(0, last))
                  : flatbuffers::Offset<flatbuffers::String>();
  R::SourcePageBuilder pb(b);
  pb.add_rev(doc->rev);
  pb.add_entries(ev);
  if (more) pb.add_next_cursor(cur);
  auto off = pb.Finish();
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::SourcePage,
                         off.Union()));
}

namespace {

// Returns the confirmed bytes of a document or source file, or nullopt.
std::optional<std::string> confirmed_bytes(const Document& doc,
                                           const Store& store,
                                           const std::string& p) {
  if (p == "tsugumi.json") return doc.project_json;
  if (p == "layout.bin")
    return doc.layout.empty() ? std::nullopt
                              : std::optional<std::string>(doc.layout);
  if (Document::is_document_file(p)) {
    auto id = p.substr(12, p.find('/', 12) - 12);
    auto it = doc.definitions.find(id);
    if (it == doc.definitions.end()) return std::nullopt;
    if (p.ends_with("_card.json")) return it->second.card_json;
    if (p.ends_with("/README.md")) return it->second.readme;
    return it->second.comments_json;
  }
  if (!doc.sources.count(p)) return std::nullopt;
  return store.read_source(doc, p);
}

}  // namespace

void Service::file_stat(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ProjectFileStatRequest();
  auto doc = store_->current();
  check_at_rev(r->at_rev(), *doc);
  std::string path = r->path()->str();
  check_path_arg(path, true);
  FBB b;
  flatbuffers::Offset<R::SourceInfo> info;
  bool is_dir = path == ".";
  if (!is_dir) {
    std::string prefix = path + "/";
    for (const auto& [p, _] : doc->sources)
      if (p.starts_with(prefix)) is_dir = true;
    if (path == "definitions" || (path.starts_with("definitions/") &&
                                  doc->definitions.count(path.substr(12))))
      is_dir = true;
  }
  if (is_dir) {
    info = wire::source_info(b, path, D::SourceKind::directory, true,
                             std::nullopt, std::nullopt);
  } else if (auto bytes = confirmed_bytes(*doc, *store_, path)) {
    info = wire::source_info(b, path, D::SourceKind::file, true,
                             crypto::sha256_hex(*bytes), bytes->size());
  } else {
    info = wire::source_info(b, path, D::SourceKind::file, false, std::nullopt,
                             std::nullopt);
  }
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::SourceInfo,
                         info.Union()));
}

void Service::file_read(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ProjectFileReadRequest();
  auto doc = store_->current();
  check_at_rev(r->at_rev(), *doc);
  std::string path = r->path()->str();
  check_path_arg(path, false);
  uint32_t limit = chunk_limit(r->limit_bytes(), 262144, 1, 1u << 20);
  std::optional<std::string> bytes = confirmed_bytes(*doc, *store_, path);
  if (!bytes)
    throw Failed(FailureKind::not_found, path + " does not exist",
                 {{"path", path}});
  std::string hash = crypto::sha256_hex(*bytes);
  if (r->if_hash() && r->if_hash()->str() != hash)
    throw Failed(FailureKind::hash_conflict, path + " does not match if_hash",
                 {{"path", path},
                  {"expected_hash", r->if_hash()->str()},
                  {"actual_hash", hash}});
  uint64_t offset = r->offset().value_or(0);
  reply(req, wire::file_page(req.request_id, path.substr(path.rfind('/') + 1),
                             wire::page_of(*bytes, offset, limit), offset,
                             bytes->size(), hash));
}

void Service::nix_path_lock_source(RequestIn& req) {
  const auto url =
      project_request(req)->value_as_NixPathLockSourceRequest()->url()->str();
  const auto locked = nix_->lock_source(url);
  FBB b;
  const auto source = R::CreateNixPathLockedSource(
      b, locked.rev.empty() ? 0 : b.CreateString(locked.rev),
      b.CreateString(locked.nar_hash));
  reply(req, wire::reply(b, req.request_id,
                         R::ReplyPayload::NixPathLockedSource, source.Union()));
}

void Service::rewrite_param_literal_request(RequestIn& req) {
  const auto* request =
      project_request(req)->value_as_RewriteParamLiteralRequest();
  const auto doc = store_->current();
  check_at_rev(request->at_rev(), *doc);
  const auto card = wire::card_arg(request->card());
  const auto* node = doc->resolution->find(card);
  if (!node || node->is_virtual)
    throw Failed(FailureKind::not_found, "unknown Card " + path_key(card));
  const auto path = node->def_dir + "params.nix";
  const auto source = doc->sources.find(path);
  if (source == doc->sources.end())
    throw Failed(FailureKind::not_found, path + " does not exist");
  if (source->second.size > kMaxDocumentJsonBytes ||
      request->literal()->size() > kMaxDocumentJsonBytes)
    throw Failed(FailureKind::invalid_request, path + " exceeds 8 MiB");
  const auto bytes = store_->read_source(*doc, path);
  std::string rewritten;
  try {
    rewritten =
        rewrite_param_literal(path, bytes, request->name()->string_view(),
                              request->literal()->string_view());
  } catch (const std::exception& error) {
    throw Failed(FailureKind::validation_failed, error.what(),
                 {{"path", path}});
  }
  if (rewritten.size() > kMaxDocumentJsonBytes)
    throw Failed(FailureKind::invalid_request, path + " would exceed 8 MiB");
  FBB b;
  const auto result = R::CreateRewrittenParamLiteral(
      b, b.CreateString(path), b.CreateString(source->second.hash),
      b.CreateString(rewritten));
  reply(req,
        wire::reply(b, req.request_id, R::ReplyPayload::RewrittenParamLiteral,
                    result.Union()));
}

void Service::input_types(RequestIn& req) {
  const auto* request = project_request(req)->value_as_InputTypesRequest();
  auto doc = store_->current();
  check_at_rev(request->at_rev(), *doc);
  const auto card = wire::card_arg(request->card());
  const auto& graph = *doc->resolution;
  if (!graph.find(card))
    throw Failed(FailureKind::not_found, "unknown Card " + path_key(card));
  FBB b;
  std::vector<flatbuffers::Offset<R::InputType>> inputs;
  auto add = [&](const std::string& name, const Node* source,
                 const std::string& port, const types::PortType* type) {
    inputs.push_back(R::CreateInputType(
        b, b.CreateString(name),
        source ? wire::path(b, source->path)
               : flatbuffers::Offset<D::CardPath>(),
        source ? b.CreateString(port)
               : flatbuffers::Offset<flatbuffers::String>(),
        type ? wire::resolved_port(b, *type) : flatbuffers::Offset<D::Port>()));
  };
  try {
    const std::optional<std::string> output =
        request->output_port() ? std::optional{request->output_port()->str()}
                               : std::nullopt;
    for (const auto& end : tsugumi::input_types(graph, card, output))
      add(end.port, end.source, end.source_port, end.type);
  } catch (const DocumentError& error) {
    throw Failed(FailureKind::not_found, error.what());
  }
  auto result = R::CreateInputTypesResult(b, doc->rev, b.CreateVector(inputs));
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::InputTypesResult,
                         result.Union()));
}

void Service::output_types(RequestIn& req) {
  const auto* r = project_request(req)->value_as_OutputTypesRequest();
  std::string text = r->text()->str();
  std::string root;
  std::string err;
  auto bfbs = types::schema_bfbs(text, &root, &err);
  std::optional<std::string> changed;
  if (bfbs && r->port() && r->type_name()) {
    changed = types::set_catalog_port(
        text, r->port()->str(), r->type_name()->str(),
        &err);
    if (changed) bfbs = types::schema_bfbs(*changed, &root, &err);
  }
  FBB b;
  auto bv =
      b.CreateVector(reinterpret_cast<const uint8_t*>(bfbs ? bfbs->data() : ""),
                     bfbs ? bfbs->size() : 0);
  auto rt = root.empty() ? flatbuffers::Offset<flatbuffers::String>()
                         : b.CreateString(root);
  auto tx = changed ? b.CreateString(*changed)
                    : flatbuffers::Offset<flatbuffers::String>();
  auto er = err.empty() ? flatbuffers::Offset<flatbuffers::String>()
                        : b.CreateString(err);
  auto res = R::CreateOutputTypesResult(b, bv, rt, tx, er);
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::OutputTypesResult,
                         res.Union()));
}

void Service::view_save(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ViewSaveRequest();
  std::string view_id = r->view_id()->str();
  if (!crypto::is_uuid(view_id))
    throw Failed(FailureKind::invalid_request, "view_id must be a UUID");
  if (!r->base_view_rev())
    throw Failed(FailureKind::invalid_request, "base_view_rev is required");
  const auto& view = *r->view();
  if (view.size() > kMaxViewBytes)
    throw Failed(FailureKind::resource_limit, "view exceeds 1 MiB");
  // The clients own scope, selection and camera semantics.
  flatbuffers::Verifier verifier(view.data(), view.size());
  if (!verifier.VerifyBuffer<R::ClientView>(nullptr))
    throw Failed(FailureKind::invalid_request, "invalid client view FlatBuffer");
  std::lock_guard<std::mutex> l(view_mu_);
  auto path = store_->state_dir() / "view";
  Tsugumi::State::ViewsT file;
  if (auto bytes = fs::read_file(path))
    if (auto decoded = flat::decode<Tsugumi::State::Views>(*bytes))
      file = std::move(*decoded);
  uint64_t current = 0;
  Tsugumi::State::ViewT* existing = nullptr;
  for (auto& entry : file.views)
    if (entry->id == view_id) existing = entry.get();
  if (existing) current = existing->rev;
  if (*r->base_view_rev() != current) {
    FailureData f = FailureData::make(FailureKind::revision_conflict,
                                      "the view was saved by another window");
    f.current_rev = current;
    throw Failed(f);
  }
  if (existing) {
    existing->rev = current + 1;
    existing->view.assign(view.begin(), view.end());
  } else {
    auto e = std::make_unique<Tsugumi::State::ViewT>();
    e->id = view_id;
    e->rev = current + 1;
    e->view.assign(view.begin(), view.end());
    file.views.push_back(std::move(e));
  }
  fs::write_atomic(path, flat::encode<Tsugumi::State::Views>(file), 0600);
  FBB b;
  auto off = R::CreateViewSaveResult(b, b.CreateString(view_id), current + 1);
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::ViewSaveResult,
                         off.Union()));
}

namespace {

auto parse_view_uuid(std::string_view text) -> ViewUuid {
  ViewUuid result{};
  size_t i = 0;
  for (const char c : text) {
    if (c == '-') continue;
    auto& byte = result[i / 2];
    byte =
        static_cast<uint8_t>((byte << 4) | (c <= '9' ? c - '0' : c - 'a' + 10));
    ++i;
  }
  return result;
}

auto view_text(const ViewUuid& uuid) -> std::string {
  auto text = crypto::hex(uuid.data(), uuid.size());
  for (const auto offset : {20, 16, 12, 8}) text.insert(offset, 1, '-');
  return text;
}

auto view_opened_note(std::string session, CardPath card, std::string name,
                      std::string type, uint32_t mode, uint32_t width,
                      uint32_t height, uint32_t flags) -> Note {
  Note note;
  note.kind = Note::Kind::ViewControl;
  note.approx_bytes +=
      path_key(card).size() + name.size() + type.size() + session.size();
  note.build = std::make_shared<const Note::Build>(
      [session = std::move(session), card = std::move(card),
       name = std::move(name), type = std::move(type), mode, width, height,
       flags](FBB& b) {
        const auto value =
            R::CreateViewOpened(b, b.CreateString(session), wire::path(b, card),
                                b.CreateString(name), b.CreateString(type),
                                mode, width, height, flags);
        return std::make_pair(R::NotificationPayload::ViewOpened,
                              value.Union());
      });
  return note;
}

auto view_recipient_note(std::string session, std::string card, bool registered)
    -> Note {
  Note note;
  note.kind = Note::Kind::ViewControl;
  note.approx_bytes += session.size() + card.size();
  note.build = std::make_shared<const Note::Build>(
      [session = std::move(session), card = std::move(card),
       registered](FBB& b) {
        const auto value = R::CreateViewRecipientChanged(
            b, b.CreateString(session), b.CreateString(card), registered);
        return std::make_pair(R::NotificationPayload::ViewRecipientChanged,
                              value.Union());
      });
  return note;
}

}  // namespace

Service::ViewBlob::ViewBlob(int fd, uint64_t size, std::string path)
    : fd(fd), size(size), path(std::move(path)) {}

Service::ViewBlob::~ViewBlob() { ::close(fd); }

auto Service::ViewBlob::open(const std::string& path, const stdfs::path& root)
    -> std::shared_ptr<const ViewBlob> {
  if (!stdfs::path(path).is_absolute()) return {};
  const int fd =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
  if (fd < 0) return {};
  const auto close = [](const int* fd) { ::close(*fd); };
  std::unique_ptr<const int, decltype(close)> guard(&fd, close);
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0)
    return {};
  // Check the resolved descriptor, so a declaration can never serve a file
  // reached through a symlink out of the store.
#ifdef F_GETPATH
  char resolved[PATH_MAX];
  if (::fcntl(fd, F_GETPATH, resolved) != 0) return {};
#else
  std::error_code ec;
  const auto resolved =
      stdfs::read_symlink("/proc/self/fd/" + std::to_string(fd), ec);
  if (ec) return {};
#endif
  std::error_code store_error;
  const auto store = stdfs::canonical(root, store_error).string();
  if (store_error || !std::string_view(stdfs::path(resolved).native())
                          .starts_with(store + "/"))
    return {};
  auto blob = std::make_shared<const ViewBlob>(fd, st.st_size, path);
  guard.release();
  return blob;
}

auto Service::ViewBlob::read(uint64_t offset, uint64_t limit) const
    -> std::string {
  if (offset > size)
    throw Failed(FailureKind::invalid_range, "offset is past the end");
  std::string bytes(std::min(limit, size - offset), '\0');
  for (size_t got = 0; got < bytes.size();) {
    const auto count = ::pread(fd, bytes.data() + got, bytes.size() - got,
                               static_cast<off_t>(offset + got));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0)
      throw Failed(FailureKind::not_found,
                   "the pinned view file could not be read");
    got += static_cast<size_t>(count);
  }
  return bytes;
}

auto Service::view_uuid(const flatbuffers::String& value) -> ViewUuid {
  const auto text = value.string_view();
  if (!crypto::is_uuid(text) ||
      text.find_first_of("ABCDEF") != std::string_view::npos)
    throw Failed(FailureKind::invalid_request,
                 "view IDs must use canonical UUID text");
  return parse_view_uuid(text);
}

auto Service::view_control(
    const ViewOrigin& origin, const std::vector<schema::CardView>& views,
    const std::weak_ptr<WorkerProc::ViewReceiver>& endpoint,
    const WorkerViewRequest& request) -> WorkerViewReply {
  WorkerViewReply reply{.request_id = request.request_id};
  const auto receiver = endpoint.lock();
  if (!request.request_id || !receiver) return reply;
  switch (request.kind) {
    case ViewControlKind::open: {
      std::unique_lock lock(subs_mu_);
      ViewKey key{origin, request.view};
      const auto reuse = [&] {
        if (const auto existing = open_views_.find(key);
            existing != open_views_.end()) {
          reply.session = existing->second;
          auto& view = view_sessions_.at(reply.session);
          if (present_view(reply.session, view))
            view.presentation_boundary = {endpoint, request.request_id};
          reply.route =
              add_view_route(reply.session, view, origin.card, endpoint);
          reply.status = 0;
          return true;
        }
        return false;
      };
      if (reuse()) return reply;
      lock.unlock();
      if (request.view.empty() || request.mode > TSUGUMI_VIEW_FULLSCREEN ||
          (request.flags &
           ~(TSUGUMI_VIEW_HIDE_CURSOR | TSUGUMI_VIEW_LOCK_POINTER)) ||
          (request.width == 0) != (request.height == 0) ||
          (request.mode != TSUGUMI_VIEW_WINDOW && request.width != 0))
        return reply;
      const auto matches = [&](const auto& view) {
        return *view.name == request.view;
      };
      const auto found = std::ranges::find_if(views, matches);
      if (found == views.end() ||
          std::ranges::find_if(
              std::ranges::subrange(std::next(found), views.end()), matches) !=
              views.end() ||
          (*found->type != "html" && *found->type != "dawn"))
        return reply;
      auto blob = ViewBlob::open(
          *found->blob, packed_builds_ ? packed_builds_->resources
                                       : stdfs::path(NixStore::store_dir()));
      lock.lock();
      if (reuse() || !blob) return reply;

      do {
        reply.session = parse_view_uuid(crypto::uuid4());
      } while (view_sessions_.contains(reply.session));
      ViewSession opening{
          std::nullopt,
          {},
          open_views_.end(),
          std::move(blob),
          view_opened_note(view_text(reply.session), origin.card, request.view,
                           *found->type, request.mode, request.width,
                           request.height, request.flags),
          std::nullopt,
          {}};
      const auto session_entry =
          view_sessions_.emplace(reply.session, std::move(opening)).first;
      auto& view = session_entry->second;
      try {
        view.key = open_views_.emplace(std::move(key), reply.session).first;
      } catch (...) {
        view_sessions_.erase(session_entry);
        throw;
      }
      present_view(reply.session, view);
      reply.route = add_view_route(reply.session, view, origin.card, endpoint);
      reply.status = 0;
      return reply;
    }
    case ViewControlKind::close: {
      if (request.session == ViewUuid{}) return reply;
      std::lock_guard lock(subs_mu_);
      const auto found = view_sessions_.find(request.session);
      if (found != view_sessions_.end()) {
        end_view(found->first, found->second);
        view_sessions_.erase(found);
      }
      reply.status = 0;
      return reply;
    }
    case ViewControlKind::register_route: {
      if (request.session == ViewUuid{}) return reply;
      std::lock_guard lock(subs_mu_);
      const auto found = view_sessions_.find(request.session);
      if (found == view_sessions_.end()) {
        reply.status = TSUGUMI_VIEW_WINDOW_CLOSED;
        return reply;
      }
      reply.route =
          add_view_route(request.session, found->second, origin.card, endpoint);
      reply.status = 0;
      return reply;
    }
    case ViewControlKind::unregister_route: {
      std::lock_guard lock(subs_mu_);
      for (auto& [session, view] : view_sessions_) {
        const auto found = view.routes.find(request.route);
        if (found == view.routes.end()) continue;
        if (found->second.endpoint.lock() == receiver) {
          remove_view_route(session, view, found);
          reply.status = 0;
        }
        break;
      }
      return reply;
    }
  }
  return reply;
}

auto Service::present_view(const ViewUuid& session, ViewSession& view) -> bool {
  if (!view.pending_open || !ui_owner_) return false;
  const auto subscriber = subscriptions_.find(*ui_owner_);
  if (subscriber == subscriptions_.end()) return false;
  const auto id = view_text(session);
  view.subscription = subscriber->second;
  server_->open_view(subscriber->first, view.subscription, id,
                     std::move(*view.pending_open));
  view.conn = subscriber->first;
  view.pending_open.reset();

  std::set<std::string_view> recipients;
  for (const auto& [_, route] : view.routes)
    if (recipients.insert(route.card).second)
      server_->view_note(*view.conn, view.subscription,
                         view_recipient_note(id, route.card, true));
  return true;
}

auto Service::add_view_route(
    const ViewUuid& session, ViewSession& view, const CardPath& card,
    const std::weak_ptr<WorkerProc::ViewReceiver>& endpoint) -> ViewUuid {
  ViewUuid route;
  do {
    route = parse_view_uuid(crypto::uuid4());
  } while (view.routes.contains(route));
  const auto receiver = endpoint.lock();
  if (!receiver) return route;
  {
    std::lock_guard lock(receiver->mutex);
    if (!receiver->worker && !receiver->remote) return route;
    if (!receiver->on_close)
      receiver->on_close = [this, endpoint] { remove_view_routes(endpoint); };
  }
  const auto address = path_key(card);
  const bool first = std::ranges::none_of(view.routes, [&](const auto& entry) {
    return entry.second.card == address;
  });
  view.routes.emplace(route, ViewSession::Route{address, endpoint});
  if (first && view.conn)
    server_->view_note(*view.conn, view.subscription,
                       view_recipient_note(view_text(session), address, true));
  return route;
}

auto Service::remove_view_route(
    const ViewUuid& session, ViewSession& view,
    std::map<ViewUuid, ViewSession::Route>::iterator route) -> void {
  const auto address = std::move(route->second.card);
  view.routes.erase(route);
  if (view.conn && std::ranges::none_of(view.routes, [&](const auto& entry) {
        return entry.second.card == address;
      }))
    server_->view_note(*view.conn, view.subscription,
                       view_recipient_note(view_text(session), address, false));
}

auto Service::remove_view_routes(
    const std::weak_ptr<WorkerProc::ViewReceiver>& endpoint) -> void {
  std::lock_guard lock(subs_mu_);
  for (auto& [session, view] : view_sessions_) {
    for (auto it = view.routes.begin(); it != view.routes.end();) {
      auto route = it++;
      const auto& owner = route->second.endpoint;
      if (!owner.owner_before(endpoint) && !endpoint.owner_before(owner))
        remove_view_route(session, view, route);
    }
  }
}

auto Service::display(WorkerDisplayData display,
                      WorkerProc::DisplayCredit credit) -> void {
  std::lock_guard lock(subs_mu_);
  const auto found = view_sessions_.find(display.session);
  if (found == view_sessions_.end() || !found->second.conn) {
    if (credit) credit();
    return;
  }
  // The reopening worker can have display frames queued ahead of
  // presentation. Its view control request may reach the relay first.
  if (const auto& boundary = found->second.presentation_boundary;
      boundary && display.view_request_id < boundary->request_id &&
      !boundary->endpoint.owner_before(display.source) &&
      !display.source.owner_before(boundary->endpoint)) {
    if (credit) credit();
    return;
  }
  server_->display(*found->second.conn, view_text(display.session),
                   std::move(display.bytes), std::move(credit));
}

auto Service::owned_view(const RequestIn& req, const ViewUuid& session)
    -> ViewSession* {
  const auto found = view_sessions_.find(session);
  if (found == view_sessions_.end()) return nullptr;
  const auto& view = found->second;
  const auto subscriber = subscriptions_.find(req.conn);
  if (view.conn != req.conn || subscriber == subscriptions_.end() ||
      subscriber->second != view.subscription)
    throw Failed(FailureKind::permission_denied,
                 "the view belongs to another subscription");
  return &found->second;
}

auto Service::end_view(const ViewUuid& session, const ViewSession& view)
    -> void {
  if (view.conn) {
    const auto id = view_text(session);
    server_->close_view(*view.conn, id);
    Note note;
    note.kind = Note::Kind::ViewControl;
    note.approx_bytes += id.size();
    note.build = std::make_shared<const Note::Build>([id](FBB& b) {
      const auto value = R::CreateViewClosed(b, b.CreateString(id));
      return std::make_pair(R::NotificationPayload::ViewClosed, value.Union());
    });
    server_->view_note(*view.conn, view.subscription, std::move(note));
  }
  for (const auto& [route, target] : view.routes)
    if (auto receiver = target.endpoint.lock())
      receiver->send({ViewEventKind::close, route, session, {}});
  open_views_.erase(view.key);
}

auto Service::end_views(ConnId conn) -> void {
  std::erase_if(view_sessions_, [&](auto& entry) {
    if (entry.second.conn != conn) return false;
    end_view(entry.first, entry.second);
    return true;
  });
}

void Service::reply_view_ack(const RequestIn& req) {
  FBB b;
  const auto ack = R::CreateAck(b);
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::Ack, ack.Union()));
}

void Service::view_send(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ViewSendRequest();
  const auto session = view_uuid(*r->session());
  const auto to = r->to()->string_view();
  const auto* bytes = r->bytes();
  if (bytes->size() > TSUGUMI_MESSAGE_MAX)
    throw Failed(FailureKind::invalid_request,
                 "view message exceeds the size limit");
  {
    std::lock_guard lock(subs_mu_);
    auto* view = owned_view(req, session);
    if (!view) throw Failed(FailureKind::not_found, "unknown view session");
    std::set<std::shared_ptr<WorkerProc::ViewReceiver>> offered;
    bool accepted = false;
    bool full = false;
    WorkerViewEvent event{ViewEventKind::message, {}, session,
                          std::string(bytes->begin(), bytes->end())};
    for (const auto& [route, target] : view->routes) {
      if (to != "*" && to != target.card) continue;
      const auto receiver = target.endpoint.lock();
      if (!receiver || !offered.insert(receiver).second) continue;
      event.route = route;
      const auto delivery = receiver->send(event);
      accepted |= delivery == WorkerProc::ViewDelivery::accepted;
      full |= delivery == WorkerProc::ViewDelivery::full;
    }
    if (full)
      throw Failed(FailureKind::busy, "a Card cannot accept a view message");
    if (!accepted)
      throw Failed(FailureKind::invalid_request,
                   "the address has no live Card registered on this window");
  }
  reply_view_ack(req);
}

void Service::view_credit(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ViewCreditRequest();
  const auto session = view_uuid(*r->session());
  if (!r->credit_id() || r->bytes() > TSUGUMI_MESSAGE_MAX)
    throw Failed(FailureKind::invalid_request, "invalid view credit");
  {
    std::lock_guard lock(subs_mu_);
    if (const auto* view = owned_view(req, session)) {
      if (view->subscription != r->subscription_id()->string_view())
        throw Failed(FailureKind::permission_denied,
                     "invalid presenting subscription");
      server_->return_view_credit(req.conn, view_text(session), *r->credit_id(),
                                  r->bytes());
    }
  }
  reply_view_ack(req);
}

void Service::view_close(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ViewCloseRequest();
  const auto session = view_uuid(*r->session());
  {
    std::lock_guard lock(subs_mu_);
    if (auto* view = owned_view(req, session)) {
      end_view(session, *view);
      view_sessions_.erase(session);
    }
  }
  reply_view_ack(req);
}

}  // namespace tsugumi
