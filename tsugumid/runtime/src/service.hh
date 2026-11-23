// Request handling for one project. Owns the lanes, the document store, the
// history and the run scheduler. Implements every request of tsugumi.fbs that
// is listed in capabilities(); anything else answers `unsupported`.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>

#include "history.hh"
#include "remote_job.hh"
#include "server.hh"
#include "store.hh"
#include "tsugumi/rpc/lanes.hh"
#include "tsugumi/util/json.hh"
#include "versions.hh"
#include "wire.hh"

namespace tsugumi {

class Scheduler;
class NixRunner;
class ExecutionService;
class PreparedExecution;
struct BuildRequest;
class LocalExecutionService;
class RemoteExecutionService;
class CacheUsage;

struct ServiceOptions {
  std::filesystem::path root;
  std::string token;  // required on WebSocket connections
  std::string server_instance_id;
  std::filesystem::path
      control_dir;                // launcher-managed files hidden from workers
  std::filesystem::path
      nix_dir;  // installed Card evaluator (share/tsugumi/nix)
  std::filesystem::path worker_binary;
  std::filesystem::path workerd_binary;
  std::string fault;      // test-only fault injection point
  uint32_t env_cpus = 2;  // execution slot limits of the environment
  // Where build results are registered as Nix GC roots. Empty: the build
  // cache in the project. The shared VM keeps them in the store
  // (/nix/var/nix/gcroots/...), visible to the one nix-daemon.
  std::filesystem::path gcroots_dir;
  // Local folder for the workers' own work/tmp/scripts folders (never the
  // project share). Empty: <system temp>/tsugumi-<project id>.
  std::filesystem::path scratch_dir;
  // Disk-backed, private scratch for invocation outputs. Never kept history.
  std::filesystem::path run_scratch_dir;
  std::filesystem::path usage_roots_dir;
  std::filesystem::path usage_scratch_dir;
  // The VM supervisor publishes one shared measurement for all projects.
  std::filesystem::path usage_file;
  // Folders of the Mac the app shares with this project, one per path at
  // <host_folders>/<sha256(path)[:16]> (the shared VM). Empty: the paths as
  // they are (a tsugumid on the Mac itself).
  std::filesystem::path host_folders;
  bool project_shell = false;  // enabled by the isolated project launcher
  // The binary caches of the Nix daemon that builds for this project, one URL
  // per line, written by a launcher whose daemon reads settings this process
  // cannot see (the shared VM). Empty: this process's own Nix configuration.
  std::filesystem::path substituters_file;
  // Builds made ahead (--prepare-cards output). When set, no Card is
  // evaluated: each uses its prepared build, and a Card without one, or
  // whose definition changed since, fails.
  std::filesystem::path prepared_cards;
  TransportLimits limits;
};

class Service : public RequestSink {
 public:
  explicit Service(ServiceOptions opts);
  ~Service() override;

  void open();  // recovers state; must succeed before the server starts
  void attach(Server* server) { server_ = server; }
  auto start_remote_cleanup() -> void;
  auto busy() -> bool;
  void shutdown();  // interrupts runs and stops lanes
  std::string project_id() const { return store_->project_id(); }

  std::pair<std::string, bool> hello(const RequestIn& req) override;
  void dispatch(RequestIn req) override;
  void disconnected(ConnId conn) override;
  // Whether the connection's SessionHello asked for English.

  static std::vector<std::string> capabilities();

  // Used by the scheduler.
  Server* server() const { return server_; }
  History& history() { return *history_; }
  Store& store() { return *store_; }
  const ServiceOptions& options() const { return opts_; }
  // The binary caches configured for this project's builds (URLs without
  // credentials or query strings), fixed when the project opens.
  const std::vector<std::string>& substituters() const { return substituters_; }
  NixRunner& nix() { return *nix_; }
  // Builds every Code Card the way a run would, without running any
  // (tsugumid --prepare-cards): one JSON line per Card. False when one failed.
  auto prepare_cards(std::ostream& out) -> bool;
  auto run_scratch() const -> const std::filesystem::path& {
    return run_scratch_;
  }
  auto file_roots() const -> std::vector<std::filesystem::path>;
  auto keep_file_value(const Value& value,
                       const std::filesystem::path& directory,
                       const std::atomic<bool>& stop,
                       const std::function<void(std::string_view, uint64_t,
                                                uint64_t)>& progress = {})
      -> Value;
  // The backend that runs the Cards naming execution service `id`.
  auto execution_service(const std::string& id) -> ExecutionService&;
  auto invalidate_resident(const std::string& card) -> void;
  auto prepare_execution(const Node& node,
                         const std::filesystem::path& directory,
                         BuildRequest& build)
      -> std::unique_ptr<PreparedExecution>;
  // The remote backend uses the same core lifetime as local execution.
  auto remote_execution() -> RemoteExecutionService&;
  auto remote_targets(std::stop_token stop = {}) const
      -> std::vector<RemoteTarget>;
  // The values of a Card's declared secrets for one invocation, from hostd.
  // Throws Failed (before anything of the Card runs) when one is missing.
  auto card_secrets(const Node& node, std::stop_token stop = {}) const
      -> std::vector<std::pair<std::string, std::string>>;
  static auto execution_diagnostic(const Node& node,
                                   const std::vector<RemoteTarget>& targets)
      -> std::optional<Diag>;
  // A run finished: its record goes to the project's copy soon.
  // A run ended: its result goes to the note of the version it started from.
  void note_run_end(const RunStatusData& s, const std::string& facts);
  // The views a Card declares (name -> blob), set with the full list each
  // time CardViewsUpdated is sent; an empty list clears them. CardViewRead
  // serves only these files.
  struct DeclaredView {
    std::string name;
    std::string blob;
  };
  auto set_card_views(const CardPath& card, std::vector<DeclaredView> views)
      -> void;

 private:
  void reply(const RequestIn& req, std::string bytes);
  void fail(const RequestIn& req, const FailureData& f);
  // Runs `fn` on a lane and converts exceptions into Failure replies.
  void on_lane(Lane& lane, RequestIn req, void (Service::*fn)(RequestIn&));

  // Mutation lane handlers.
  void open_project(RequestIn& req);
  void save(RequestIn& req);
  auto card_id(RequestIn& req) -> void;
  void reload(RequestIn& req);
  struct PreparedSaveReply {
    std::string receipt;
    size_t notification_bytes = 0;
  };
  auto prepare_save_reply(Store::Staged& staged, const Document& base,
                          const flatbuffers::Vector<uint8_t>* operation_data =
                              nullptr) -> PreparedSaveReply;
  void complete_save(
      RequestIn& req, const std::string& op_id, const std::string& kind,
      const std::string& digest, const Document& base, Store::Staged& staged,
      const PreparedSaveReply& response,
      const std::function<void()>& check_cancelled = [] {});
  auto version_operation(RequestIn& req,
                         const std::function<std::string()>& operation) -> void;
  void cache_clear(RequestIn& req);
  void cache_usage(RequestIn& req);
  void scratch_delete(RequestIn& req);
  void history_delete(RequestIn& req);
  void history_category_delete(RequestIn& req);
  void history_file_references(RequestIn& req);
  void run(RequestIn& req);
  void version_init(RequestIn& req);
  void version_mark(RequestIn& req);
  void version_checkout(RequestIn& req);
  // Read pool handlers.
  void version_graph(RequestIn& req);
  void version_canvas(RequestIn& req);
  void version_note(RequestIn& req);
  void version_diff(RequestIn& req);
  void version_comments(RequestIn& req);
  auto project_host_requirements(RequestIn& req) -> void;
  void inspect(RequestIn& req);
  void validate(RequestIn& req);
  void operation_get(RequestIn& req);
  void file_list(RequestIn& req);
  void file_stat(RequestIn& req);
  void file_read(RequestIn& req);
  void output_types(RequestIn& req);
  void input_types(RequestIn& req);
  void nix_path_lock_source(RequestIn& req);
  void rewrite_param_literal_request(RequestIn& req);
  void evaluate_nix(RequestIn& req);
  void run_plan(RequestIn& req);
  void run_cancel(RequestIn& req);
  void run_status(RequestIn& req);
  void run_history(RequestIn& req);
  void run_invocations(RequestIn& req);
  void run_definition_paths(RequestIn& req);
  void run_latest_invocations(RequestIn& req);
  auto dispatch_shell(RequestIn req) -> void;
  auto project_shell(RequestIn& req, const std::atomic<bool>& cancelled)
      -> void;
  void run_record(RequestIn& req);
  auto run_environments(RequestIn& req) -> void;
  auto run_environment_paths(RequestIn& req) -> void;
  auto run_environment_uses(RequestIn& req) -> void;
  void run_value_read(RequestIn& req);
  void artifact_list(RequestIn& req);
  void artifact_read(RequestIn& req);
  void log_range(RequestIn& req);
  void view_set_visible(RequestIn& req);
  void card_view_read(RequestIn& req);
  void view_save(RequestIn& req);

  // Receipt handling shared by mutating requests. Returns true when the
  // request was answered from an existing receipt (or joined a pending one).
  bool replay_or_join(RequestIn& req, const std::string& op_id,
                      const std::string& kind, const std::string& digest);
  void finish_op(const std::string& op_id, const std::string& receipt_bytes);
  std::string receipt_reply(uint64_t request_id,
                            const std::string& receipt_bytes, bool replayed);
  void store_failure_receipt(const std::string& op_id, const std::string& kind,
                             const std::string& digest, const FailureData& f);

  struct ViewRecord {
    uint64_t rev = 0;
    std::string view;
  };
  std::string snapshot_reply(uint64_t request_id, const Document& doc,
                             const std::optional<std::string>& view_id,
                             bool open, const std::string& sub_id,
                             const ViewRecord* view = nullptr);
  flatbuffers::Offset<Tsugumi::RPC::ProjectSnapshot> snapshot(
      flatbuffers::FlatBufferBuilder& b, const Document& doc,
      const std::optional<std::string>& view_id, const ViewRecord* view);
  void check_snapshot(const Document& doc);
  void publish_delta(const Delta& delta, size_t notification_bytes);
  auto refresh_card_views(DocumentPtr doc) -> void;
  void check_at_rev(std::optional<uint64_t> at_rev, const Document& doc) const;
  RunStatusData current_run_status();

  std::optional<ViewRecord> load_view(const std::string& view_id);

  ServiceOptions opts_;
  std::vector<std::string> substituters_;
  std::unique_ptr<Store> store_;
  std::unique_ptr<History> history_;
  std::unique_ptr<Versions> versions_;
  // Artifact files whose content was checked against their hash, by path:
  // (size, modification time) when checked. Later pages read their range only.
  std::mutex verified_mu_;
  std::map<std::string, std::pair<uint64_t, int64_t>> verified_;
  std::mutex view_files_mu_;
  std::map<std::string, std::vector<DeclaredView>> view_files_;  // path_key
  std::unique_ptr<NixRunner> nix_;
  std::filesystem::path run_scratch_;
  std::unique_ptr<CacheUsage> cache_usage_;
  std::mutex worker_selection_mutex_;
  std::condition_variable worker_selection_cv_;
  std::map<std::string, uint32_t> worker_slots_;
  std::map<std::string, std::string> resident_targets_;
  std::unique_ptr<LocalExecutionService> execution_service_;
  RemoteJobClient remote_jobs_;
  std::unique_ptr<RemoteExecutionService> remote_execution_;
  std::unique_ptr<Scheduler> scheduler_;
  std::unique_ptr<Lane> mutation_lane_;
  std::unique_ptr<Lane> read_pool_;
  std::unique_ptr<Lane> cancel_lane_;
  std::unique_ptr<Lane> shell_lane_;
  std::unique_ptr<Lane> card_views_lane_;
  std::stop_source card_views_shutdown_;
  std::mutex card_views_build_mu_;
  // Owned by the mutation lane. A new execution revision cancels older work.
  std::optional<uint64_t> card_views_rev_;
  std::stop_source card_views_cancel_;
  std::map<std::string, std::optional<Note>> card_views_;
  std::mutex cancellations_mu_;
  bool shell_stopping_ = false;
  std::map<std::pair<ConnId, uint64_t>, std::shared_ptr<std::atomic<bool>>>
      shell_cancellations_;
  std::map<std::pair<ConnId, uint64_t>, std::shared_ptr<std::atomic<bool>>>
      save_cancellations_;
  Server* server_ = nullptr;

  std::mutex ops_mu_;
  struct Pending {
    std::string digest;
    std::vector<std::pair<ConnId, uint64_t>> waiters;
  };
  std::map<std::string, Pending> pending_ops_;

  std::mutex subs_mu_;
  std::set<ConnId> ui_connections_;
  std::optional<ConnId> ui_owner_;
  // Owned by the transport loop, alongside debug dispatch and disconnect.
  std::optional<ConnId> trace_owner_;
  std::map<ConnId, std::string> subscriptions_;  // conn -> subscription id
  std::map<ConnId, std::set<std::string>> visible_cards_;

  std::mutex view_mu_;

  // The project's copy of finished runs (.tsugumi/runs.sqlite), written
  // shortly after runs end when the project keeps its run history.
};

// Canonical request digests (all arguments except request_id, defaults
// expanded).
std::string request_digest(const Tsugumi::RPC::Request* req);

}  // namespace tsugumi
