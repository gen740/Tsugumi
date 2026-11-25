#include "tsugumi/core/daemon.hh"

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "service.hh"
#include "tsugumi/protocol/runtime_state_generated.h"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/flat_file.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/log.hh"

namespace tsugumi::core {

namespace stdfs = std::filesystem;
namespace S = Tsugumi::State;

namespace {

struct Options {
  stdfs::path project;
  std::string unix_socket;
  std::string ws_unix_socket;
  std::string listen_host = "127.0.0.1";
  int listen_port = -1;
  stdfs::path control_dir;
  std::vector<std::string> origins;
  bool allow_missing_origin = false;
  stdfs::path nix_dir;
  stdfs::path worker;
  stdfs::path workerd;
  uint32_t cpus = 2;
  stdfs::path gcroots_dir;
  stdfs::path run_scratch_dir;
  stdfs::path scratch_dir;
  stdfs::path usage_roots_dir;
  stdfs::path usage_scratch_dir;
  stdfs::path usage_file;
  stdfs::path host_folders;
  bool project_shell = false;
  stdfs::path substituters_file;
  stdfs::path prepare_cards;
  stdfs::path prepared_cards;
  std::string fault;
};

void usage() {
  std::fprintf(
      stderr,
      "usage: tsugumid --project DIR [--unix PATH] [--listen HOST:PORT] "
      "[--control-dir DIR]\n"
      "                [--allow-origin ORIGIN]... [--allow-missing-origin] "
      "[--nix-dir DIR]\n"
      "                [--worker PATH] [--workerd PATH] [--cpus N] "
      "[--gcroots-dir DIR]\n"
      "                [--scratch-dir DIR] [--run-scratch-dir DIR] "
      "[--usage-roots-dir DIR] [--usage-scratch-dir DIR]\n"
      "                [--usage-file FILE]\n"
      "                [--host-folders DIR] [--substituters-file FILE]\n"
      "       tsugumid --project DIR --prepare-cards FILE [options]\n"
      "                builds every Code Card without running any, writes one\n"
      "                JSON line per Card to FILE and exits\n"
      "       --prepared-cards FILE  runs those builds; evaluates no Card\n");
}

std::optional<Options> parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
      return argv[++i];
    };
    if (a == "--project")
      o.project = next();
    else if (a == "--unix")
      o.unix_socket = next();
    else if (a == "--listen-unix-ws")
      o.ws_unix_socket = next();
    else if (a == "--listen") {
      std::string v = next();
      auto colon = v.rfind(':');
      if (colon == std::string::npos)
        throw std::runtime_error("--listen needs HOST:PORT");
      o.listen_host = v.substr(0, colon);
      o.listen_port = std::stoi(v.substr(colon + 1));
    } else if (a == "--control-dir")
      o.control_dir = next();
    else if (a == "--allow-origin")
      o.origins.push_back(next());
    else if (a == "--allow-missing-origin")
      o.allow_missing_origin = true;
    else if (a == "--nix-dir")
      o.nix_dir = next();
    else if (a == "--worker")
      o.worker = next();
    else if (a == "--workerd")
      o.workerd = next();
    else if (a == "--cpus")
      o.cpus = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--gcroots-dir")
      o.gcroots_dir = next();
    else if (a == "--run-scratch-dir")
      o.run_scratch_dir = next();
    else if (a == "--scratch-dir")
      o.scratch_dir = next();
    else if (a == "--usage-roots-dir")
      o.usage_roots_dir = next();
    else if (a == "--usage-scratch-dir")
      o.usage_scratch_dir = next();
    else if (a == "--usage-file")
      o.usage_file = next();
    else if (a == "--host-folders")
      o.host_folders = next();
    else if (a == "--project-shell")
      o.project_shell = true;
    else if (a == "--substituters-file")
      o.substituters_file = next();
    else if (a == "--prepare-cards")
      o.prepare_cards = next();
    else if (a == "--prepared-cards")
      o.prepared_cards = next();
    else if (a == "--help")
      return std::nullopt;
    else
      throw std::runtime_error("unknown option " + a);
  }
  if (o.project.empty()) throw std::runtime_error("--project is required");
  // Fault injection exists for the persistence tests only and needs an explicit
  // opt-in.
  if (const char* f = std::getenv("TSUGUMI_FAULT");
      f && std::getenv("TSUGUMI_ENABLE_FAULTS"))
    o.fault = f;
  return o;
}

stdfs::path exe_dir(const char* argv0) {
  std::error_code ec;
#if defined(__linux__)
  auto p = stdfs::read_symlink("/proc/self/exe", ec);
  if (!ec) return p.parent_path();
#endif
  auto p2 = stdfs::canonical(argv0, ec);
  return ec ? stdfs::current_path() : p2.parent_path();
}

}  // namespace

int run_daemon(int argc, char** argv) {
  Options opt;
  try {
    auto parsed = parse(argc, argv);
    if (!parsed) {
      usage();
      return 0;
    }
    opt = *parsed;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "tsugumid: %s\n", e.what());
    usage();
    return 2;
  }
  ::signal(SIGPIPE, SIG_IGN);
  std::error_code ec;
  auto root = stdfs::canonical(opt.project, ec);
  if (ec || !stdfs::is_regular_file(root / "tsugumi.json")) {
    log::error("project " + opt.project.string() +
               " is not an initialized Tsugumi project");
    return 2;
  }
  auto bin = exe_dir(argv[0]);
  if (opt.worker.empty()) opt.worker = bin / "tsugumi-worker";
  if (opt.workerd.empty()) {
    if (const auto* path = std::getenv("TSUGUMI_WORKERD"))
      opt.workerd = path;
    else
      opt.workerd = opt.worker.parent_path() / "tsugumi-workerd";
  }
  if (opt.nix_dir.empty())
    opt.nix_dir = bin.parent_path() / "share" / "tsugumi" / "nix";

  // Ownership: a fixed lock file, exclusive and non-blocking, held for life.
  auto state = root / ".tsugumi";
  stdfs::create_directories(state);
  ::chmod(state.c_str(), 0700);
  auto lock_path = state / "tsugumid.lock";
  std::optional<fs::FileLock> lock;
  try {
    if (auto l = fs::FileLock::try_acquire(lock_path))
      lock.emplace(std::move(*l));
  } catch (const std::exception& e) {
    log::error(std::string("cannot open the project lock: ") + e.what());
    return 3;
  }
  if (!lock) {
    log::error("another tsugumid already owns " + root.string());
    return 4;
  }
  // Refuse storage where the advisory lock is not actually enforced.
  {
    int fd = ::open(lock_path.c_str(), O_RDWR | O_CLOEXEC);
    bool enforced =
        fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    if (fd >= 0) ::close(fd);
    if (!enforced) {
      log::error(
          "file locks are not enforced on this storage; refusing to serve " +
          root.string());
      return 5;
    }
  }

  ServiceOptions so;
  so.root = root;
  so.token = crypto::random_token(32);
  so.server_instance_id = crypto::uuid4();
  so.control_dir = opt.control_dir;
  so.nix_dir = opt.nix_dir;
  so.worker_binary = opt.worker;
  so.workerd_binary = opt.workerd;
  so.fault = opt.fault;
  so.env_cpus = opt.cpus;
  so.gcroots_dir = opt.gcroots_dir;
  so.run_scratch_dir = opt.run_scratch_dir;
  so.scratch_dir = opt.scratch_dir;
  so.usage_roots_dir = opt.usage_roots_dir;
  so.usage_scratch_dir = opt.usage_scratch_dir;
  so.usage_file = opt.usage_file;
  so.host_folders = opt.host_folders;
  so.project_shell = opt.project_shell;
  so.substituters_file = opt.substituters_file;
  so.prepared_cards = opt.prepared_cards;
  if (!so.scratch_dir.empty()) {
    // Evaluator downloads and subprocess temporaries belong to the selected
    // work area too, rather than a second implicit global temporary root.
    const auto temporary = so.scratch_dir / "tmp";
    stdfs::create_directories(temporary);
    const auto resolved = stdfs::canonical(temporary).string();
    if (::setenv("TMPDIR", resolved.c_str(), 1) != 0)
      throw std::runtime_error("cannot select evaluator temporary directory");
  }
  // Block termination signals before threads start; they are taken with
  // sigwait.
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGINT);
  ::pthread_sigmask(SIG_BLOCK, &set, nullptr);

  if (!opt.control_dir.empty()) {
    stdfs::create_directories(opt.control_dir);
    stdfs::remove(opt.control_dir / "server.startup-error");
  }
  Service service(so);
  const auto report_startup_failure = [&](S::StartupErrorT error,
                                          const std::exception& cause) {
    error.message = cause.what();
    if (!opt.control_dir.empty())
      fs::write_atomic(opt.control_dir / "server.startup-error",
                       flat::encode<S::StartupError>(error), 0600);
    log::error(std::string("cannot open the project: ") + cause.what());
    return 6;
  };
  try {
    service.open();
  } catch (const sql::FormatMismatch& e) {
    S::StartupErrorT error;
    error.kind = e.path.filename() == "runs.sqlite" ? "incompatible_history"
                                                    : "incompatible_state";
    error.found = std::to_string(e.found);
    error.expected = std::to_string(e.expected);
    error.source = "local";
    error.path = e.path.string();
    return report_startup_failure(std::move(error), e);
  } catch (const std::exception& e) {
    S::StartupErrorT error;
    const auto* failure = dynamic_cast<const Failed*>(&e);
    error.kind = "project_open_failed";
    if (failure && failure->failure.kind == FailureKind::resource_limit)
      error.kind = "resource_limit";
    if (failure && failure->failure.kind == FailureKind::invalid_document)
      error.kind = "invalid_document";
    return report_startup_failure(std::move(error), e);
  }
  if (!opt.prepare_cards.empty()) {
    std::ofstream out(opt.prepare_cards, std::ios::trunc);
    if (!out) {
      log::error("cannot write " + opt.prepare_cards.string());
      return 2;
    }
    const bool built = service.prepare_cards(out);
    service.shutdown();
    return built ? 0 : 1;
  }
  ServerOptions sv;
  sv.unix_path = opt.unix_socket;
  sv.ws_unix_path = opt.ws_unix_socket;
  sv.tcp_host = opt.listen_host;
  sv.tcp_port = opt.listen_port;
  sv.allowed_origins = opt.origins;
  // The launcher writes control/origins (protocol/runtime_state.fbs) to allow
  // another UI's origin while the project stays open.
  if (!opt.control_dir.empty())
    sv.origin_allowed = [file = opt.control_dir / "origins"](
                            const std::string& origin) {
      const auto bytes = fs::read_file(file);
      const auto list =
          bytes ? flat::decode<S::Origins>(*bytes) : std::nullopt;
      return list && std::find(list->origins.begin(), list->origins.end(),
                               origin) != list->origins.end();
    };
  sv.allow_missing_origin = opt.allow_missing_origin;
  ProjectEndpoint(sv);
  Server server(sv, &service);
  service.attach(&server);
  try {
    server.start();
    service.start_remote_cleanup();
  } catch (const std::exception& e) {
    log::error(std::string("cannot start the RPC server: ") + e.what());
    return 7;
  }
  std::string endpoint = opt.listen_port >= 0
                             ? "ws://" + opt.listen_host + ":" +
                                   std::to_string(server.bound_tcp_port()) +
                                   "/rpc"
                             : "";
  if (!opt.control_dir.empty()) {
    S::ServerReadyT ready;
    ready.endpoint = endpoint;
    ready.token = so.token;
    ready.protocol = 1;
    ready.project_id = service.project_id();
    ready.unix_socket = opt.unix_socket;
    ready.pid = ::getpid();
    stdfs::create_directories(opt.control_dir);
    fs::write_atomic(opt.control_dir / "server.ready",
                     flat::encode<S::ServerReady>(ready), 0600);
  }
  log::info("ready project_id=" + service.project_id() +
            (endpoint.empty() ? "" : " endpoint=" + endpoint) +
            (opt.unix_socket.empty() ? "" : " unix=" + opt.unix_socket));
  int sig = 0;
  sigwait(&set, &sig);
  log::info(std::string("stopping on ") +
            (sig == SIGTERM ? "SIGTERM" : "SIGINT"));
  if (!opt.control_dir.empty())
    ::unlink((opt.control_dir / "server.ready").c_str());
  service.shutdown();
  server.stop();
  return 0;
}

}  // namespace tsugumi::core
