// Request handling for one project. Owns the lanes, the document store, the
// history and the run scheduler. Implements every request of tsugumi.fbs that
// is listed in capabilities(); anything else answers `unsupported`.
#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include "history.hpp"
#include "lanes.hpp"
#include "schema.hpp"
#include "server.hpp"
#include "store.hpp"
#include "wire.hpp"

namespace tsugumi {

class Scheduler;
class NixRunner;

struct ServiceOptions {
  std::filesystem::path root;
  std::string token;                 // required on WebSocket connections
  std::string server_instance_id;
  std::filesystem::path grants_file;  // launcher-managed import grants (JSON)
  std::filesystem::path sdk_dir;      // installed SDK (share/tsugumi/sdk)
  std::filesystem::path worker_binary;
  std::string fault;                  // test-only fault injection point
  uint32_t env_cpus = 2;              // execution slot limits of the environment
  bool own_nix_store = false;         // true inside the container: CacheClear may run nix GC
  Limits limits;
};

class Service : public RequestSink {
 public:
  explicit Service(ServiceOptions opts);
  ~Service() override;

  void open();  // recovers state; must succeed before the server starts
  void attach(Server* server) { server_ = server; }
  void shutdown();  // interrupts runs and stops lanes
  std::string project_id() const { return store_->project_id(); }

  std::pair<std::string, bool> hello(const RequestIn& req) override;
  void dispatch(RequestIn req) override;
  void disconnected(ConnId conn) override;

  static std::vector<std::string> capabilities();

  // Used by the scheduler.
  Server* server() const { return server_; }
  History& history() { return *history_; }
  Store& store() { return *store_; }
  const ServiceOptions& options() const { return opts_; }
  NixRunner& nix() { return *nix_; }

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
  void run(RequestIn& req);
  void sdk_prepare(RequestIn& req);
  // Read pool handlers.
  void close_project(RequestIn& req);
  void inspect(RequestIn& req);
  void validate(RequestIn& req);
  void operation_get(RequestIn& req);
  void file_list(RequestIn& req);
  void file_stat(RequestIn& req);
  void file_read(RequestIn& req);
  void file_format(RequestIn& req);
  void sdk_status(RequestIn& req);
  void run_plan(RequestIn& req);
  void run_cancel(RequestIn& req);
  void run_status(RequestIn& req);
  void run_history(RequestIn& req);
  void run_invocations(RequestIn& req);
  void run_record(RequestIn& req);
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
  std::unique_ptr<Store> store_;
  std::unique_ptr<History> history_;
  std::unique_ptr<NixRunner> nix_;
  std::unique_ptr<Scheduler> scheduler_;
  std::unique_ptr<Lane> mutation_lane_;
  std::unique_ptr<Lane> read_pool_;
  Server* server_ = nullptr;

  std::mutex ops_mu_;
  struct Pending {
    std::string digest;
    std::vector<std::pair<ConnId, uint64_t>> waiters;
  };
  std::map<std::string, Pending> pending_ops_;

  std::mutex subs_mu_;
  std::map<ConnId, std::string> subscriptions_;  // conn -> subscription id

  std::mutex view_mu_;
  std::string cursor_secret_;
};

// Canonical request digests (all arguments except request_id, defaults expanded).
std::string request_digest(const Tsugumi::RPC::Request* req);

}  // namespace tsugumi
