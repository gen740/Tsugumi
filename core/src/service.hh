// Request handling for one project. Owns the lanes, the document store, the
// history and the run scheduler. Implements every request of tsugumi.fbs that
// is listed in capabilities(); anything else answers `unsupported`.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include "history.hh"
#include "lanes.hh"
#include "remote_job.hh"
#include "schema.hh"
#include "server.hh"
#include "store.hh"
#include "versions.hh"
#include "wire.hh"

namespace tsugumi {

class Scheduler;
class NixRunner;
class ExecutionService;
class NativeExecutionService;
class RemoteExecutionService;

struct ServiceOptions {
  std::filesystem::path root;
  std::string token;                 // required on WebSocket connections
  std::string server_instance_id;
  std::filesystem::path grants_file;  // launcher-managed import grants (JSON)
  // Launcher-managed list of the folders attached to a chat (JSON), for a
  // tsugumid on the host (no chat_folders): only these are read.
  std::filesystem::path attached_file;
  std::filesystem::path sdk_dir;      // installed SDK (share/tsugumi/sdk)
  std::filesystem::path worker_binary;
  std::string fault;                  // test-only fault injection point
  uint32_t env_cpus = 2;              // execution slot limits of the environment
  // Where build results are registered as Nix GC roots. Empty: the build
  // cache in the project. The shared VM keeps them in the store
  // (/nix/var/nix/gcroots/...), visible to the one nix-daemon.
  std::filesystem::path gcroots_dir;
  // Local folder for the workers' own work/tmp/scripts folders (never the
  // project share). Empty: <system temp>/tsugumi-<project id>.
  std::filesystem::path scratch_dir;
  // Folder of the run history database (history.sqlite). Empty: the
  // project's .tsugumi. The shared VM keeps it on the VM's disk; finished
  // runs are then also kept in the project (.tsugumi/runs.sqlite).
  std::filesystem::path history_dir;
  bool archive_incompatible_history = false;
  // Folders of the Mac the app shares with this project, one per path at
  // <host_folders>/<sha256(path)[:16]> (the shared VM). Empty: the paths as
  // they are (a tsugumid on the Mac itself).
  std::filesystem::path host_folders;
  // Folders attached to a chat, the same way (<chat_folders>/<tag>); read
  // only through AttachedFolderRead, never by a Card. Empty: the paths as
  // they are.
  std::filesystem::path chat_folders;
  bool project_shell = false;  // enabled by the isolated project launcher
  // The binary caches of the Nix daemon that builds for this project, one URL
  // per line, written by a launcher whose daemon reads settings this process
  // cannot see (the shared VM). Empty: this process's own Nix configuration.
  std::filesystem::path substituters_file;
  Limits limits;
};

class Service : public RequestSink {
 public:
  explicit Service(ServiceOptions opts);
  ~Service() override;

  void open();  // recovers state; must succeed before the server starts
  void attach(Server* server) { server_ = server; }
  auto recover_remote_jobs() -> void;
  void shutdown();  // interrupts runs and stops lanes
  std::string project_id() const { return store_->project_id(); }

  std::pair<std::string, bool> hello(const RequestIn& req) override;
  void dispatch(RequestIn req) override;
  void disconnected(ConnId conn) override;
  // Whether the connection's SessionHello asked for English.
  bool english(ConnId conn);

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
  // The backend that runs the Cards naming execution service `id`.
  auto execution_service(const std::string& id) -> ExecutionService&;
  // The daemon backend also recovers its jobs.
  auto remote_execution() -> RemoteExecutionService&;
  auto remote_targets() const -> std::vector<RemoteTarget>;
  static auto execution_diagnostic(const Node& node, const std::vector<RemoteTarget>& targets) -> std::optional<Diag>;
  // A run finished: its record goes to the project's copy soon.
  void schedule_export();
  // A run ended: its result goes to the note of the version it started from.
  void note_run_end(const RunStatusData& s, const std::string& summary);

 private:
  void reply(const RequestIn& req, std::string bytes);
  void fail(const RequestIn& req, const FailureData& f);
  // Runs `fn` on a lane and converts exceptions into Failure replies.
  void on_lane(Lane& lane, RequestIn req, void (Service::*fn)(RequestIn&));

  // Mutation lane handlers.
  void open_project(RequestIn& req);
  void save(RequestIn& req);
  void reload(RequestIn& req);
  void import_definitions(RequestIn& req);
  void cache_clear(RequestIn& req);
  void history_delete(RequestIn& req);
  void run(RequestIn& req);
  void sdk_prepare(RequestIn& req);
  void version_init(RequestIn& req);
  void version_mark(RequestIn& req);
  void version_checkout(RequestIn& req);
  // Read pool handlers.
  void version_graph(RequestIn& req);
  void version_canvas(RequestIn& req);
  void version_diff(RequestIn& req);
  void version_summary(RequestIn& req);
  void close_project(RequestIn& req);
  void inspect(RequestIn& req);
  void validate(RequestIn& req);
  void operation_get(RequestIn& req);
  void file_list(RequestIn& req);
  void file_stat(RequestIn& req);
  void file_read(RequestIn& req);
  void file_format(RequestIn& req);
  void output_types(RequestIn& req);
  void nix_source_resolve(RequestIn& req);
  void attached_folder_read(RequestIn& req);
  void sdk_status(RequestIn& req);
  void run_plan(RequestIn& req);
  void run_cancel(RequestIn& req);
  void run_status(RequestIn& req);
  void run_history(RequestIn& req);
  void run_invocations(RequestIn& req);
  void run_latest_invocations(RequestIn& req);
  auto dispatch_shell(RequestIn req) -> void;
  auto project_shell(RequestIn& req, const std::atomic<bool>& cancelled) -> void;
  void run_record(RequestIn& req);
  auto run_environments(RequestIn& req) -> void;
  auto run_environment_paths(RequestIn& req) -> void;
  void run_value_read(RequestIn& req);
  void artifact_list(RequestIn& req);
  void artifact_read(RequestIn& req);
  void log_range(RequestIn& req);
  void view_set_visible(RequestIn& req);
  void view_save(RequestIn& req);

  // Receipt handling shared by mutating requests. Returns true when the
  // request was answered from an existing receipt (or joined a pending one).
  bool replay_or_join(RequestIn& req, const std::string& op_id, const std::string& kind,
                      const std::string& digest);
  void finish_op(const std::string& op_id, const std::string& receipt_bytes);
  std::string receipt_reply(uint64_t request_id, const std::string& receipt_bytes, bool replayed);
  void store_failure_receipt(const std::string& op_id, const std::string& kind,
                             const std::string& digest, const FailureData& f);

  std::string snapshot_reply(uint64_t request_id, const Document& doc,
                             const std::optional<std::string>& view_id, bool open,
                             const std::string& sub_id);
  flatbuffers::Offset<Tsugumi::RPC::ProjectSnapshot> snapshot(flatbuffers::FlatBufferBuilder& b,
                                                              const Document& doc,
                                                              const std::optional<std::string>& view_id);
  void publish_delta(const Delta& delta);
  void check_at_rev(std::optional<uint64_t> at_rev, const Document& doc) const;
  RunStatusData current_run_status();

  struct ViewRecord {
    uint64_t rev = 0;
    schema::ViewStateJson view;
  };
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
  std::unique_ptr<NixRunner> nix_;
  std::unique_ptr<NativeExecutionService> execution_service_;
  RemoteJobClient remote_jobs_;
  std::unique_ptr<RemoteExecutionService> remote_execution_;
  std::unique_ptr<Scheduler> scheduler_;
  std::unique_ptr<Lane> mutation_lane_;
  std::unique_ptr<Lane> read_pool_;
  std::unique_ptr<Lane> shell_lane_;
  std::mutex shell_mu_;
  bool shell_stopping_ = false;
  std::map<std::pair<ConnId, uint64_t>, std::shared_ptr<std::atomic<bool>>> shell_cancellations_;
  Server* server_ = nullptr;

  std::mutex ops_mu_;
  struct Pending {
    std::string digest;
    std::vector<std::pair<ConnId, uint64_t>> waiters;
  };
  std::map<std::string, Pending> pending_ops_;

  std::mutex subs_mu_;
  std::map<ConnId, std::string> subscriptions_;  // conn -> subscription id
  std::map<ConnId, bool> english_;              // conn -> its SessionHello's language is English

  std::mutex view_mu_;
  std::string cursor_secret_;

  // The project's copy of finished runs (.tsugumi/runs.sqlite), written
  // shortly after runs end when the project keeps its run history.
  std::filesystem::path runs_copy() const { return store_->state_dir() / "runs.sqlite"; }
  void export_loop();
  void export_now();
  std::mutex export_mu_;
  std::condition_variable export_cv_;
  std::optional<std::chrono::steady_clock::time_point> export_first_;
  std::optional<std::chrono::steady_clock::time_point> export_last_;
  bool export_stop_ = false;
  std::thread exporter_;
};

// Canonical request digests (all arguments except request_id, defaults expanded).
std::string request_digest(const Tsugumi::RPC::Request* req);

}  // namespace tsugumi
