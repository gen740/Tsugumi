#include "execution_service.hh"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <system_error>

#include "service.hh"
#include "store.hh"
#include "util/crypto.hh"

#ifndef TSUGUMI_CARD_PATH
#define TSUGUMI_CARD_PATH "/usr/bin:/bin"
#endif
#ifndef TSUGUMI_BASH
#define TSUGUMI_BASH "/bin/bash"
#endif

namespace tsugumi {

namespace {

// The unprivileged identity for Card processes.
constexpr uid_t kCardUid = 65534;
// The descriptor slot through which a call's out/ folder reaches the Card.
constexpr int32_t kOutSlot = 900;

}  // namespace

LocalExecutionService::LocalExecutionService(Service& service, std::filesystem::path scratch_root)
    : service_(service), workers_(std::move(scratch_root)) {}

auto LocalExecutionService::clear() -> void { workers_.clear(); }

auto LocalExecutionService::run(const LocalExecutionRequest& request, std::optional<bool>& worker_reused,
                                std::optional<std::pair<std::string, std::string>>& out_alias) -> WorkerOutcome {
  const Node& node = request.node;
  const CardBuild& build = request.build;

  // Cards run as nobody: switched to by root (container), or mapped in
  // bubblewrap's user namespace when tsugumid is not root (shared VM).
  std::optional<uid_t> uid;
  if (::geteuid() == 0 || !std::string(TSUGUMI_BWRAP).empty()) uid = kCardUid;
  WorkerSpawn sp;
  // A package of another system is loaded by the worker its build made for
  // that system, which the kernel runs under the emulator.
  sp.worker_binary = build.worker.empty() ? service_.options().worker_binary.string() : build.worker;
  sp.bash = TSUGUMI_BASH;
  sp.build = build;
  sp.sandbox = node.settings.sandbox;
  sp.uid = uid;
  sp.gid = uid;
  // Every File / Directory input lives in these trees: one set of binds
  // serves any input, so the worker is reused whatever the paths.
  const std::filesystem::path state_dir = service_.store().state_dir();
  sp.ro_paths = {(state_dir / "runs").string(), (state_dir / "inputs").string()};
  // Another system's binaries run with the emulator the kernel opened when it
  // was registered; Rosetta also reads its own folder while it runs.
  if (const auto& e = build.emulator; !e.empty() && !e.starts_with("/nix/store/"))
    sp.ro_paths.push_back(std::filesystem::path(e).parent_path().string());
  if (!service_.options().grants_file.empty())
    sp.hidden_paths.push_back(service_.options().grants_file.parent_path().string());
  sp.hidden_paths.push_back(state_dir.string());
  sp.network = node.settings.network;
  const std::string card_key = path_key(node.path);
  sp.card = card_key;
  for (const auto& f : node.settings.host_folders) {
    std::string source = f.path;
    if (const auto& root = service_.options().host_folders; !root.empty())
      source = (root / crypto::sha256_hex(f.path).substr(0, 16)).string();
    std::error_code ec;
    if (!std::filesystem::is_directory(source, ec))
      throw Failed(FailureKind::not_found,
                   "the folder " + f.path + " (/host/" + f.name +
                       ") is not available here: it is missing on this Mac, or the project must be opened again to share it",
                   {{"path", f.path}});
    sp.binds.push_back({source, "/host/" + f.name, f.write});
  }

  // The resident worker of this Card, if it runs the same build under the same conditions.
  std::string key = build.package.value_or("") + "|" + build.bash.value_or("") + "|" + build.pre_run + "|" +
                    build.post_run + "|" + (sp.sandbox ? "s" : "u") + "|" + (uid ? std::to_string(*uid) : "-") +
                    "|" + (sp.network ? "n" : "-") + "|" + sp.worker_binary + "|" + build.key +
                    "|" + std::to_string(build.implementation_index);
  for (const auto& b : sp.binds) key += "|" + b.source + ">" + b.target + (b.write ? "w" : "r");
  const bool reuse = node.settings.reuse_worker;
  const auto worker_card = card_key + "|" + std::to_string(build.implementation_index);
  auto lease = workers_.acquire(worker_card, key, sp, reuse);
  WorkerProc& worker = *lease.worker;
  worker_reused = lease.reused;

  // Folders: without filesystem: true, work/ and tmp/ are the worker's own
  // (local, emptied between calls); out/ exists only for Cards with File or
  // Directory outputs and stays (downstream Cards and artifacts use it).
  bool needs_out = false;
  for (const auto& o : node.outputs) needs_out |= o.kind != PortKind::FlatBuffer;
  const std::filesystem::path& dir = request.directory;
  const std::string work = worker.work_dir();
  const std::string tmp = worker.tmp_dir();
  std::string out;
  if (needs_out) {
    std::filesystem::path d = dir / "out";
    std::filesystem::create_directories(d);
    if (::geteuid() == 0 && worker_uid_on_host()) ::chown(d.c_str(), kCardUid, kCardUid);
    out = d.string();
  }

  // Under bubblewrap the call's output folder reaches the Card as a
  // descriptor (/proc/self/fd/<slot>); the run trees are bound read-only.
  // Working folders are never passed that way (a current directory outside
  // the sandbox has no path): with filesystem: true they are copied into
  // the run after the call.
  std::vector<int> fds;
  std::vector<int32_t> slots;
  struct Close {
    std::vector<int>& fds;
    ~Close() {
      for (int f : fds) ::close(f);
    }
  } close_fds{fds};
  out_alias.reset();
  if (!out.empty() && worker.passes_directories()) {
    int fd = ::open(out.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) throw Failed(FailureKind::worker_failed, "cannot open " + out);
    fds.push_back(fd);
    slots.push_back(kOutSlot);
    std::string alias = "/proc/self/fd/" + std::to_string(kOutSlot);
    out_alias = std::make_pair(alias, out);
    out = alias;
  }

  auto ptype = NixRunner::params_type(node);
  std::string params = NixRunner::params_buffer(node);
  std::vector<std::pair<std::string, std::string>> env = {
      {"PATH", TSUGUMI_CARD_PATH},
      {"HOME", work},
      {"LANG", "C.UTF-8"},
      {"TSUGUMI_RUN_ID", request.run_id},
      {"TSUGUMI_CARD", card_key},
      {"TSUGUMI_INVOCATION", std::to_string(request.invocation)},
      {"TSUGUMI_PARAMS_JSON", NixRunner::params_json(node, false)},
      {"TSUGUMI_WORKER", sp.worker_binary},
  };
  std::optional<uint64_t> deadline_ms;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  if (node.settings.timeout_seconds) {
    deadline_ms = *node.settings.timeout_seconds * 1000;
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(*deadline_ms + 500);
  }
  uint64_t call_id = worker.next_call_id();
  std::string frame = worker_call_frame(call_id, request.run_id, node.path, request.invocation, request.inputs,
                                        ptype->bfbs, ptype->root_type, params, node.outputs, work, tmp, out, env,
                                        deadline_ms, slots);
  // Logs and displays of this call belong to this run and invocation.
  worker.set_logs(request.log);
  int64_t t0 = now_ms();
  WorkerOutcome outcome = worker.call(frame, call_id, fds, &request.cancel, deadline, request.display);
  request.on_timing(t0, now_ms());

  if (node.settings.filesystem) {
    // Kept for inspection in the run (CacheClear removes it).
    std::error_code ec;
    for (const auto& [from, to] : {std::pair{worker.work_dir(), dir / "work"}, std::pair{worker.tmp_dir(), dir / "tmp"}}) {
      std::filesystem::create_directories(to, ec);
      std::filesystem::copy(from, to,
                            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing |
                                std::filesystem::copy_options::copy_symlinks,
                            ec);
    }
  }
  worker.clear_scratch();
  // Kept only after a clean call: a failed, cancelled or timed out worker
  // (and anything it started) ends.
  workers_.release(worker_card, key, std::move(lease.worker), reuse && !outcome.failure);
  return outcome;
}

}  // namespace tsugumi
