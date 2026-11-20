#include "service.hh"

#include <flatbuffers/reflection.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>

#include "execution_service.hh"
#include "nix.hh"
#include "permissions.hh"
#include "remote_execution_service.hh"
#include "run_scratch.hh"
#include "runtime_schema.hh"
#include "scheduler.hh"
#include "tsugumi/document/host_requirements.hh"
#include "tsugumi/document/input_types.hh"
#include "tsugumi/protocol/trace_codec.hh"
#include "tsugumi/runtime_support/cache_usage.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/log.hh"
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
    if (auto query = url.find_first_of("?#"); query != std::string::npos)
      url.resize(query);
    auto scheme = url.find("://");
    // Native hosts can also use file and local stores. Their paths can expose
    // private filesystem locations and do not identify a remote cache.
    if (!url.starts_with("http://") && !url.starts_with("https://") &&
        !url.starts_with("s3://"))
      continue;
    auto host = scheme + 3;
    auto slash = url.find('/', host);
    auto at =
        url.rfind('@', slash == std::string::npos ? std::string::npos : slash);
    if (at != std::string::npos && at >= host) url.erase(host, at + 1 - host);
    if (url.size() == host || url[host] == '/') continue;
    bool printable = !url.empty() && url.size() <= 2048 &&
                     std::all_of(url.begin(), url.end(), [](unsigned char c) {
                       return c > 32 && c != 127;
                     });
    if (printable && urls.size() < 64 &&
        std::find(urls.begin(), urls.end(), url) == urls.end())
      urls.push_back(url);
  }
  return urls;
}

}  // namespace

Service::Service(ServiceOptions opts) : opts_(std::move(opts)) {
  mutation_lane_ = std::make_unique<Lane>(1);
  read_pool_ = std::make_unique<Lane>(6);
  shell_lane_ = std::make_unique<Lane>(2);
}

Service::~Service() { shutdown(); }

void Service::open() {
  store_ = std::make_unique<Store>(StoreOptions{opts_.root, opts_.fault});
  store_->open();
  history_ = std::make_unique<History>(store_->state_dir() / "runs.sqlite",
                                       opts_.fault);
  versions_ = std::make_unique<Versions>(opts_.root);
  // Local folders (never the project share): the evaluator's inputs by
  // content and the workers' own folders.
  auto scratch =
      opts_.scratch_dir.empty()
          ? stdfs::temp_directory_path() / ("tsugumi-" + store_->project_id())
          : opts_.scratch_dir;
  nix_ = std::make_unique<NixRunner>(*this, scratch / "nix");
  substituters_ = configured_substituters(opts_.substituters_file);
  for (const char* d : {"runs", "inputs"})
    stdfs::create_directories(store_->state_dir() / d);
  run_scratch_ = disk_scratch_path(opts_.run_scratch_dir.empty()
                                       ? default_run_scratch(opts_.root)
                                       : opts_.run_scratch_dir);
  execution_service_ =
      std::make_unique<LocalExecutionService>(*this, scratch / "workers");
  remote_execution_ =
      std::make_unique<RemoteExecutionService>(*this, remote_jobs_);
  scheduler_ = std::make_unique<Scheduler>(*this);
  scheduler_->recover_interrupted();
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

auto Service::file_roots() const -> std::vector<stdfs::path> {
  return {store_->state_dir() / "runs", store_->state_dir() / "inputs",
          run_scratch_ / "runs"};
}

auto Service::keep_file_value(
    const Value& value, const stdfs::path& directory,
    const std::atomic<bool>& stop,
    const std::function<void(std::string_view, uint64_t, uint64_t)>& progress)
    -> Value {
  if (value.kind == PortKind::FlatBuffer) return value;
  const auto source = path_value(value);
  if (!source)
    throw Failed(FailureKind::output_invalid, "file value has no path");
  for (const auto& root : file_roots()) {
    const auto rel =
        stdfs::path(*source).lexically_normal().lexically_relative(root);
    if (!fs::valid_relative_path(rel.generic_string())) continue;
    const auto check = [&] {
      if (stop.load())
        throw Failed(FailureKind::cancelled,
                     "keeping file value was cancelled");
    };
    const auto destination = directory / rel.filename();
    const auto copy = [&](const stdfs::path& from, const stdfs::path& to) {
      check();
      const auto total = stdfs::file_size(root / from);
      uint64_t done = 0;
      if (progress) progress(from.generic_string(), 0, total);
      fs::copy_beneath(root, from, to, [&](std::string_view chunk) {
        check();
        done += chunk.size();
        if (progress) progress(from.generic_string(), done, total);
      });
    };
    if (value.kind == PortKind::File)
      copy(rel, destination);
    else {
      const auto listing = fs::list_beneath(root, rel);
      if (!listing.links.empty() || !listing.special.empty())
        throw Failed(FailureKind::output_invalid,
                     "kept directory contains a link or special file");
      stdfs::create_directories(destination);
      for (const auto& child : listing.directories) {
        check();
        stdfs::create_directories(destination / child);
      }
      for (const auto& child : listing.files)
        copy(rel / child, destination / child);
    }
    check();
    return with_path(value, destination.string());
  }
  throw Failed(FailureKind::output_invalid,
               "file value is outside the run's folders: " + *source);
}

auto Service::execution_service(const std::string& id) -> ExecutionService& {
  // Any other id names a registered execution daemon; its backend checks that
  // the service is available when an invocation is prepared.
  if (id == kLocalExecutionService) return *execution_service_;
  return *remote_execution_;
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

auto Service::prepare_execution(const Node& node, const stdfs::path& directory,
                                BuildRequest& build)
    -> std::unique_ptr<PreparedExecution> {
  auto targets = remote_targets();
  const auto card = path_key(node.path);
  std::unique_lock lock(worker_selection_mutex_);
  if (!node.settings.reuse_worker) resident_targets_.erase(card);
  std::string selected;
  for (;;) {
    std::vector<WorkerCandidate> candidates;
    for (const auto& target : targets) {
      const auto capacity =
          target.id == "local" ? opts_.env_cpus : target.max_jobs;
      candidates.push_back(
          {target.id, target.grade,
           target.available &&
               (!capacity || worker_slots_[target.id] < capacity)});
    }
    const auto resident = resident_targets_.find(card);
    const auto resident_id =
        resident == resident_targets_.end() ? std::string{} : resident->second;
    for (;;) {
      const auto choice =
          select_worker(candidates, node.settings.worker_condition.get(),
                        node.settings.execution_service, resident_id);
      if (!choice.matches)
        throw Failed(FailureKind::invalid_request,
                     "no registered worker satisfies workerCondition");
      if (!choice.index) break;
      const auto target = targets[*choice.index];
      if (target.id != "local") {
        lock.unlock();
        RemoteProbe live;
        try {
          live = remote_jobs_.probe(target.id, target.server_id);
        } catch (...) {
          lock.lock();
          throw;
        }
        lock.lock();
        if (live.active_jobs >= live.max_jobs ||
            worker_slots_[target.id] >= live.max_jobs) {
          candidates[*choice.index].free = false;
          continue;
        }
      }
      selected = target.id;
      break;
    }
    if (!selected.empty()) break;
    if (build.cancel && build.cancel->load())
      throw Failed(FailureKind::cancelled, "worker selection cancelled");
    worker_selection_cv_.wait_for(lock, std::chrono::milliseconds(100));
    lock.unlock();
    targets = remote_targets();
    lock.lock();
  }
  ++worker_slots_[selected];
  lock.unlock();
  auto release = [this, selected] {
    std::lock_guard guard(worker_selection_mutex_);
    --worker_slots_[selected];
    worker_selection_cv_.notify_all();
  };
  try {
    Node chosen = node;
    chosen.settings.execution_service = selected;
    auto execution =
        execution_service(selected).prepare(chosen, directory, build);
    if (node.settings.reuse_worker) {
      std::lock_guard guard(worker_selection_mutex_);
      resident_targets_[card] = selected;
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

auto Service::remote_targets() const -> std::vector<RemoteTarget> {
  try {
    auto targets = remote_jobs_.catalog();
    auto local =
        std::ranges::find(targets, kLocalExecutionService, &RemoteTarget::id);
    if (local == targets.end()) {
      RemoteTarget target;
      target.id = kLocalExecutionService;
      targets.insert(targets.begin(), std::move(target));
      local = targets.begin();
    }
    local->available = true;
    local->native_system = native_system();
    local->systems = NixRunner::execution_systems();
    return targets;
  } catch (const std::exception&) {
    RemoteTarget local;
    local.id = kLocalExecutionService;
    local.available = true;
    local.native_system = native_system();
    local.systems = NixRunner::execution_systems();
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
  if (node.kind != Template::Code) return std::nullopt;
  const auto error = [&](std::string message) -> Diag {
    return {true, "invalid_setting", std::move(message), node.path, {}, {}};
  };
  const auto unavailable = [&](const RemoteTarget& target) {
    return error(target.unavailable_reason.empty()
                     ? "execution service " + target.id + " is not available"
                     : target.unavailable_reason);
  };
  if (settings.execution_service == "auto") {
    const RemoteTarget* blocked = nullptr;
    for (const auto& target : targets) {
      if (settings.worker_condition &&
          !settings.worker_condition->matches(target.grade))
        continue;
      if (target.available) return std::nullopt;
      if (!blocked) blocked = &target;
    }
    if (blocked) return unavailable(*blocked);
    return Diag{true,
                "invalid_setting",
                "no registered worker satisfies workerCondition",
                node.path,
                {},
                {}};
  }
  const auto target =
      std::ranges::find(targets, settings.execution_service, &RemoteTarget::id);
  if (target == targets.end() ||
      (settings.worker_condition &&
       !settings.worker_condition->matches(target->grade)))
    return error("execution service " + settings.execution_service +
                 " is not available");
  if (!target->available) return unavailable(*target);
  // Cached capabilities describe the last observation, not a reason to refuse
  // a run. RemoteExecutionService probes the worker before choosing a system
  // or checking resources.
  return std::nullopt;
}

void Service::shutdown() {
  remote_jobs_.stop();
  {
    std::lock_guard lock(cancellations_mu_);
    shell_stopping_ = true;
    // Accepted saves drain with the mutation lane. Only a peer disconnect
    // cancels its save; stopping the service must not discard accepted edits.
    for (const auto& [key, cancelled] : shell_cancellations_)
      cancelled->store(true);
  }
  if (shell_lane_) shell_lane_->stop();
  if (scheduler_) scheduler_->shutdown();
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
                                   "OperationGet",
                                   "ProjectFileList",
                                   "ProjectFileStat",
                                   "ProjectFileRead",
                                   "OutputTypes",
                                   "InputTypes",
                                   "CardSDKPrepare",
                                   "CardSDKStatus",
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
                                   "RunRecord",
                                   "RunValueRead",
                                   "ArtifactList",
                                   "ArtifactRead",
                                   "ModuleRead",
                                   "LogRange",
                                   "ViewSetVisible",
                                   "ViewSave",
                                   "NixSourceResolve",
                                   "VersionInit",
                                   "VersionMark",
                                   "VersionCheckout",
                                   "VersionGraph",
                                   "VersionCanvas",
                                   "VersionNote",
                                   "VersionDiff",
                                   "VersionComments",
                                   "VersionSummary"};
  // Every Display kind of the protocol.
  for (auto v : D::EnumValuesDisplayValue())
    if (v != D::DisplayValue::NONE)
      caps.push_back(std::string("display:") + D::EnumNameDisplayValue(v));
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
  return {
      wire::reply(b, in.request_id, R::ReplyPayload::HelloResult, r.Union()),
      true};
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
  switch (request->value_type()) {
    case P::CoreTraceRequest: {
      namespace T = Tsugumi::Trace;
      if (!trace::LaunchEnabled())
        return fail(req, FailureData::make(
                             FailureKind::permission_denied,
                             "Project core was started without RPC tracing"));
      const auto* debug = request->value_as_CoreTraceRequest();
      if (req.websocket) {
        const auto* path = std::getenv("TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE");
        const auto capability = path ? fs::read_file(path) : std::nullopt;
        if (!capability || capability->empty() || !debug->capability() ||
            !crypto::equal_secret(debug->capability()->string_view(), *capability))
          return fail(req, FailureData::make(FailureKind::permission_denied,
                                            "RPC capture requires local host access"));
      }
      if (trace_owner_ && trace_owner_ != req.conn)
        return fail(req, FailureData::make(FailureKind::busy,
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
      reply(req, wire::reply(b, req.request_id,
                             R::ReplyPayload::TraceSnapshot, value.Union()));
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
    case P::RunHistoryDeleteRequest:
      return on_lane(m, std::move(req), &Service::history_delete);
    case P::RunRequest:
      return on_lane(m, std::move(req), &Service::run);
    case P::CardSDKPrepareRequest:
      return on_lane(m, std::move(req), &Service::sdk_prepare);
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
    case P::VersionSummaryRequest:
      return on_lane(m, std::move(req), &Service::version_summary);
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
    case P::NixSourceResolveRequest:
      return on_lane(r, std::move(req), &Service::nix_source_resolve);
    case P::CardSDKStatusRequest:
      return on_lane(r, std::move(req), &Service::sdk_status);
    case P::RunPlanRequest:
      return on_lane(r, std::move(req), &Service::run_plan);
    case P::RunCancelRequest:
      return on_lane(r, std::move(req), &Service::run_cancel);
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
    case P::ModuleReadRequest:
      return on_lane(r, std::move(req), &Service::module_read);
    case P::ArtifactReadRequest:
      return on_lane(r, std::move(req), &Service::artifact_read);
    case P::LogRangeRequest:
      return on_lane(r, std::move(req), &Service::log_range);
    case P::ViewSetVisibleRequest:
      return on_lane(r, std::move(req), &Service::view_set_visible);
    case P::ViewSaveRequest:
      return on_lane(r, std::move(req), &Service::view_save);
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
    case R::ReceiptValue::CardSDKPrepareResult:
      type = R::ReplyPayload::CardSDKPrepareResult;
      tname = "Tsugumi.RPC.CardSDKPrepareResult";
      break;
    case R::ReceiptValue::RunScratchDeleteResult:
      type = R::ReplyPayload::RunScratchDeleteResult;
      tname = "Tsugumi.RPC.RunScratchDeleteResult";
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
  if (kind == "shell") {
    // A running shell has a durable uncertainty receipt. In this process,
    // duplicate callers wait for its final result instead of reading that
    // marker.
    std::lock_guard lock(ops_mu_);
    if (auto it = pending_ops_.find(op_id); it != pending_ops_.end()) {
      if (it->second.digest != kind + ":" + digest)
        throw Failed(FailureKind::operation_conflict,
                     "op_id is pending for a different request");
      it->second.waiters.emplace_back(req.conn, req.request_id);
      return true;
    }
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
  std::lock_guard<std::mutex> l(ops_mu_);
  auto it = pending_ops_.find(op_id);
  if (it != pending_ops_.end()) {
    if (it->second.digest != kind + ":" + digest)
      throw Failed(FailureKind::operation_conflict,
                   "op_id is pending for a different request",
                   {{"op_id", op_id}});
    it->second.waiters.emplace_back(req.conn, req.request_id);
    return true;
  }
  pending_ops_[op_id] = Pending{kind + ":" + digest, {}};
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
                                  e.size, e.document_json.get()));
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
  auto text = fs::read_file(store_->state_dir() / "view.json");
  if (!text) return std::nullopt;
  schema::ViewFile file;
  if (schema::decode(*text, file)) return std::nullopt;
  for (const auto& [id, entry] : file.views->entries())
    if (id == view_id) return ViewRecord{*entry.rev, *entry.view};
  return std::nullopt;
}

auto Service::busy() -> bool {
  return !mutation_lane_->idle() || !read_pool_->idle() ||
         !shell_lane_->idle() || scheduler_->active_status().has_value();
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
  auto lj = b.CreateString(doc.layout_json);
  std::vector<flatbuffers::Offset<R::ExecutionService>> services;
  for (const auto& target : targets)
    services.push_back(R::CreateExecutionService(
        b, b.CreateString(target.id), b.CreateString(target.native_system),
        b.CreateVectorOfStrings(target.systems),
        target.id == "local" ? R::ExecutionServiceKind::local
                             : R::ExecutionServiceKind::remote_daemon,
        target.available, target.resource_limits,
        target.max_jobs ? flatbuffers::Optional(target.max_jobs)
                        : flatbuffers::nullopt,
        target.grade));
  auto execution_services = b.CreateVector(services);
  flatbuffers::Offset<flatbuffers::String> view;
  flatbuffers::Offset<flatbuffers::String> vid;
  std::optional<uint64_t> vrev;
  if (view_id) {
    vid = b.CreateString(*view_id);
    if (record) {
      view = b.CreateString(record->view);
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
  sb.add_layout_json(lj);
  sb.add_definitions(defs_v);
  sb.add_sources(src);
  sb.add_resolution(res);
  sb.add_diagnostics(diags);
  sb.add_run(run);
  sb.add_execution_services(execution_services);
  if (view_id) {
    if (!view.IsNull()) sb.add_view_json(view);
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
  auto lj = d.layout_changed ? b.CreateString(doc.layout_json)
                             : flatbuffers::Offset<flatbuffers::String>();
  R::DocumentDeltaBuilder db(b);
  db.add_base_rev(d.base_rev);
  db.add_rev(d.rev);
  db.add_execution_rev(d.execution_rev);
  db.add_op_id(op);
  db.add_origin(origin);
  if (d.project_changed) db.add_project_json(pj);
  if (d.layout_changed) db.add_layout_json(lj);
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
    subscriptions_[req.conn] = sub;
  }
  server_->subscribe(req.conn, sub, std::move(bytes));
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
      case R::DocumentChangeValue::SetLayout:
        ch.kind = ChangeIn::Kind::SetLayout;
        ch.json = c->value_as_SetLayout()->json()->str();
        break;
      case R::DocumentChangeValue::PutDefinition: {
        const auto* p = c->value_as_PutDefinition();
        ch.kind = ChangeIn::Kind::PutDefinition;
        ch.id = p->id()->str();
        ch.json = p->card_json()->str();
        // An empty optional document is one not given (a model fills in every
        // field). Neither file is ever empty. The _meta.json stays: it may hold
        // the user's intent, and leaving a field blank must not remove that.
        if (p->meta_json() && p->meta_json()->size() > 0)
          ch.meta_json = p->meta_json()->str();
        ch.keep_meta =
            p->keep_meta() || (p->meta_json() && p->meta_json()->size() == 0);
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
    if (staged.effective &&
        staged.doc->execution_fingerprint != base->execution_fingerprint) {
      if (auto active = scheduler_->active_status())
        throw Failed(FailureKind::busy,
                     "a run is in progress; computational changes are rejected "
                     "until it ends",
                     {{"run_id", *active->run_id}});
    }
    const auto response = prepare_save_reply(staged, *base, r->operation_data());
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

void Service::complete_save(RequestIn& req, const std::string& op_id,
                            const std::string& kind, const std::string& digest,
                            const Document& base, Store::Staged& staged,
                            const PreparedSaveReply& response,
                            const std::function<void()>& check_cancelled) {
  store_->commit(staged, kind, digest,
                 [&](const Delta*) { return response.receipt; },
                 check_cancelled);
  if (staged.delta.doc) scheduler_->document_changed(base, *staged.delta.doc);
  reply(req, receipt_reply(req.request_id, response.receipt, false));
  if (staged.delta.doc)
    publish_delta(staged.delta, response.notification_bytes);
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
                           const std::string& record_json) {
  if (!s.commit || !s.run_id || !s.terminal()) return;
  VersionRunNote n;
  n.run_id = *s.run_id;
  n.state = Tsugumi::Data::EnumNameRunState(s.state);
  int64_t end = s.ended_at.value_or(0);
  int64_t start = s.started_at.value_or(s.accepted_at.value_or(end));
  n.ended = Versions::iso_utc(end);
  n.seconds = end > start ? static_cast<double>(end - start) / 1000.0 : 0;
  n.record_json = record_json;
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
    });
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
        c.by.empty() ? 0 : b.CreateString(c.by), b.CreateVectorOfStrings(c.cards)));
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
      const auto facts = version_facts::run(n.record_json);
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
  auto bytes = wire::reply(b, req.request_id, R::ReplyPayload::VersionGraph,
                           vg.Union());
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
                             b.CreateString(canvas.layout_json));
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::VersionCanvas,
                         vc.Union()));
}

void Service::version_note(RequestIn& req) {
  const auto* r = project_request(req)->value_as_VersionNoteRequest();
  auto note = versions_->note(r->commit()->str());
  FBB b;
  auto facts = version_facts::changes(note.record_json);
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
  const auto value = R::CreateVersionDiff(b, b.CreateString(diff.text),
                                         diff.truncated);
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

void Service::version_summary(RequestIn& req) {
  const auto* r = project_request(req)->value_as_VersionSummaryRequest();
  const auto text = versions_->summary(
      r->commit()->str(), r->english(),
      r->text() ? std::optional(r->text()->str()) : std::nullopt);
  FBB b;
  const auto value = R::CreateVersionSummary(b, b.CreateString(text));
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::VersionSummary,
                         value.Union()));
}

void Service::cache_clear(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ProjectCacheClearRequest();
  std::string op_id = r->op_id()->str();
  std::string digest = request_digest(project_request(req));
  if (replay_or_join(req, op_id, "cache_clear", digest)) return;
  try {
    if (scheduler_->active_status() || nix_->sdk_jobs_active())
      throw Failed(
          FailureKind::busy,
          "cache can only be cleared while no run or SDK job is active");
    if (history_->read(History::has_remote_jobs))
      throw Failed(
          FailureKind::busy,
          "cache cannot be cleared while external job results are pending");
    execution_service_
        ->clear();  // resident workers hold the builds being removed
    auto result = nix_->clear_cache();
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
    const auto scratch_before = fs::directory_size(run_scratch_ / "runs");
    uint64_t files = 0;
    auto ids = history_->delete_runs(r->ended_before(), run_ids);
    for (const auto& id : ids)
      if (crypto::is_run_id(id)) remove_run_scratch(run_scratch_ / "runs" / id);
    const auto scratch_after = fs::directory_size(run_scratch_ / "runs");
    files +=
        scratch_before > scratch_after ? scratch_before - scratch_after : 0;
    // The runs' kept files: recorded values and declared artifacts.
    for (const auto& id : ids) {
      if (!crypto::is_run_id(id))
        continue;  // a run id from a copy names no folder of ours
      auto dir = store_->state_dir() / "runs" / id;
      std::error_code ec;
      if (!stdfs::is_directory(dir, ec)) continue;
      for (auto it = stdfs::recursive_directory_iterator(dir, ec);
           !ec && it != stdfs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) files += it->file_size(ec);
      stdfs::remove_all(dir, ec);
      if (ec)
        log::warn("history: cannot remove " + dir.string() + ": " +
                  ec.message());
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
  reply(req, operation_reply(req.request_id, bytes ? &*bytes : nullptr,
                             pending));
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
  files.insert("layout.json");
  for (const auto& [id, d] : doc->definitions) {
    files.insert("definitions/" + id + "/_card.json");
    if (d.meta_json) files.insert("definitions/" + id + "/_meta.json");
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
      } else if (p == "layout.json") {
        hash = crypto::sha256_hex(doc->layout_json);
        size = doc->layout_json.size();
      } else {
        auto id = p.substr(12, p.find('/', 12) - 12);
        const auto& d = doc->definitions.at(id);
        const auto& text =
            p.ends_with("_card.json") ? d.card_json : *d.meta_json;
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
  if (p == "layout.json") return doc.layout_json;
  if (Document::is_document_file(p)) {
    auto id = p.substr(12, p.find('/', 12) - 12);
    auto it = doc.definitions.find(id);
    if (it == doc.definitions.end()) return std::nullopt;
    if (p.ends_with("_card.json")) return it->second.card_json;
    return it->second.meta_json;
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
  } else if (auto sdk = nix_->sdk_file(path)) {
    info = wire::source_info(b, path, D::SourceKind::file, true,
                             crypto::sha256_hex(*sdk), sdk->size());
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
    bytes = nix_->sdk_file(path, doc->rev);  // throws when the SDK rev is stale
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

void Service::nix_source_resolve(RequestIn& req) {
  auto info = nix_->resolve_flake(
      project_request(req)->value_as_NixSourceResolveRequest()->ref()->str());
  FBB b;
  std::vector<flatbuffers::Offset<flatbuffers::String>> packages;
  for (const auto& p : info.packages) packages.push_back(b.CreateString(p));
  auto ref = b.CreateString(info.ref);
  auto rev = b.CreateString(info.rev);
  auto hash = b.CreateString(info.nar_hash);
  auto res =
      R::CreateNixSourceResolved(b, ref, rev, hash, b.CreateVector(packages));
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::NixSourceResolved,
                         res.Union()));
}

void Service::input_types(RequestIn& req) {
  const auto* request = project_request(req)->value_as_InputTypesRequest();
  auto doc = store_->current();
  check_at_rev(request->at_rev(), *doc);
  const auto card = wire::card_arg(request->card());
  const auto& graph = *doc->resolution;
  const auto* node = graph.find(card);
  if (!node)
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
        type ? wire::port(b, *type) : flatbuffers::Offset<D::Port>()));
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
  auto result = R::CreateInputTypesResult(
      b, doc->rev, b.CreateString(node->settings.language),
      b.CreateVector(inputs));
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
        r->kind() == D::PortKind::unspecified ? D::PortKind::FlatBuffer
                                              : r->kind(),
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
  const auto view = r->view_json()->str();
  if (view.size() > kMaxViewBytes)
    throw Failed(FailureKind::resource_limit, "view exceeds 1 MiB");
  // Validate JSON syntax only; scope, selection and camera belong to clients.
  schema::Json opaque;
  if (auto error = schema::decode(view, opaque))
    throw Failed(FailureKind::invalid_request, error->message);
  std::lock_guard<std::mutex> l(view_mu_);
  auto path = store_->state_dir() / "view.json";
  schema::ViewFile file;
  if (auto text = fs::read_file(path)) {
    if (schema::decode(*text, file)) file = schema::ViewFile{};
  }
  uint64_t current = 0;
  schema::ViewEntry* existing = nullptr;
  for (auto& [id, entry] : file.views->entries())
    if (id == view_id) existing = &entry;
  if (existing) current = *existing->rev;
  if (*r->base_view_rev() != current) {
    FailureData f = FailureData::make(FailureKind::revision_conflict,
                                      "the view was saved by another window");
    f.current_rev = current;
    throw Failed(f);
  }
  if (existing) {
    *existing->rev = current + 1;
    *existing->view = view;
  } else {
    schema::ViewEntry e;
    *e.rev = current + 1;
    *e.view = view;
    file.views->emplace_back(view_id, std::move(e));
  }
  fs::write_atomic(path, schema::encode(file), 0600);
  FBB b;
  auto off = R::CreateViewSaveResult(b, b.CreateString(view_id), current + 1);
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::ViewSaveResult,
                         off.Union()));
}

void Service::view_set_visible(RequestIn& req) {
  const auto* r = project_request(req)->value_as_ViewSetVisibleRequest();
  std::set<std::string> cards;
  for (const auto* c : *r->cards())
    cards.insert(path_key(wire::card_arg(c, "cards")));
  bool enabled = r->enabled().value_or(true);
  FBB b;
  auto ack = R::CreateAck(b);
  std::string ack_bytes =
      wire::reply(b, req.request_id, R::ReplyPayload::Ack, ack.Union());
  std::vector<Note> initial;
  if (enabled)
    for (const auto& key : cards)
      if (auto n = scheduler_->latest_display_note(key))
        initial.push_back(std::move(*n));
  server_->set_visible(req.conn, r->subscription_id()->str(), std::move(cards),
                       enabled, std::move(ack_bytes), std::move(initial));
}

}  // namespace tsugumi
