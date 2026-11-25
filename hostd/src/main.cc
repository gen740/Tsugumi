#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

#include "host/execution_broker.hh"
#include "host/process_lock.hh"
#include "host/service.hh"
#include "host/store.hh"
#include "host/web_pages.hh"
#include "host/wire_endpoint.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/log.hh"
#include "tsugumi/util/sqlite.hh"

namespace tsugumi::host {
namespace json = ::tsugumi::json;
namespace {
class HostContract final : public RequestSink {
 public:
  explicit HostContract(Service& host) : host_(host) {}
  auto hello(const RequestIn& r) -> std::pair<std::string, bool> override {
    return host_.hello(r);
  }
  auto dispatch(RequestIn r) -> void override { host_.dispatch(std::move(r)); }
  auto disconnected(ConnId connection) -> void override {
    host_.host_disconnected(connection);
  }

 private:
  Service& host_;
};

class StateDirectory {
 public:
  explicit StateDirectory(const std::filesystem::path& path)
      : path_(path),
        fd_(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {
    if (fd_ < 0) throw std::runtime_error("Cannot open host state directory");
  }
  StateDirectory(const StateDirectory&) = delete;
  auto operator=(const StateDirectory&) -> StateDirectory& = delete;
  ~StateDirectory() { ::close(fd_); }

  auto IsCurrent() const -> bool {
    struct stat original{}, current{};
    return ::fstat(fd_, &original) == 0 &&
           ::stat(path_.c_str(), &current) == 0 &&
           original.st_dev == current.st_dev &&
           original.st_ino == current.st_ino;
  }

  auto Remove(const std::filesystem::path& file) const -> void {
    if (!IsCurrent()) return;
    // Even if the path is renamed after the check, unlink in the opened
    // directory. A new host at the original path owns different files.
    if (::unlinkat(fd_, file.filename().c_str(), 0) != 0 && errno != ENOENT)
      throw std::runtime_error("Cannot remove " + file.string());
    ::fsync(fd_);
  }

 private:
  const std::filesystem::path& path_;
  int fd_;
};
}  // namespace

auto RunHost(int argc, char** argv) -> int {
  try {
    HostOptions options;
    ServerOptions listener;
    AgentEndpoint(listener);
    listener.tcp_host = "127.0.0.1";
    listener.tcp_port = 0;
    listener.allow_missing_origin = true;
    std::filesystem::path broker_dir;
    bool ensure = false;
    std::filesystem::path move_aside;
    int found_format = 0;
    std::filesystem::path web_root;
    int web_port = 8780;
    bool web_port_chosen = false;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      auto next = [&]() -> std::string {
        if (++i == argc) throw std::runtime_error(arg + " requires a value");
        return argv[i];
      };
      if (arg == "--ensure")
        ensure = true;
      else if (arg == "--move-state-aside") {
        move_aside = next();
        const auto found = next();
        const auto [end, error] = std::from_chars(
            found.data(), found.data() + found.size(), found_format);
        if (end != found.data() + found.size() || error != std::errc{})
          throw std::runtime_error("Invalid state file format");
      } else if (arg == "--state-dir")
        options.state_dir = next();
      else if (arg == "--application-name")
        options.application = next();
      else if (arg == "--unix")
        listener.unix_path = next();
      else if (arg == "--vmhost")
        options.vm.binary = next();
      else if (arg == "--vm-support")
        options.vm.support = next();
      else if (arg == "--vm-image")
        options.vm.image = next();
      else if (arg == "--tsugumid")
        options.environment.tsugumid = next();
      else if (arg == "--worker")
        options.environment.worker = next();
      else if (arg == "--nix-dir")
        options.environment.nix_dir = next();
      else if (arg == "--environment-dir")
        options.environment.directory = next();
      else if (arg == "--socket-dir")
        broker_dir = next();
      else if (arg == "--usage-scratch-dir")
        options.environment.run_scratch_directory = next();
      else if (arg == "--repo-root")
        options.repo_root = next();
      else if (arg == "--demos-dir")
        options.demos_dir = next();
      else if (arg == "--web-root")
        web_root = next();
      else if (arg == "--web-port") {
        web_port = std::stoi(next());
        web_port_chosen = true;
      } else if (arg == "--allow-origin")
        listener.allowed_origins.push_back(next());
      else if (arg == "--listen") {
        const auto address = next();
        const auto colon = address.rfind(':');
        if (colon == std::string::npos)
          throw std::runtime_error("--listen requires HOST:PORT");
        listener.tcp_host = address.substr(0, colon);
        listener.tcp_port = std::stoi(address.substr(colon + 1));
      } else if (arg == "--no-websocket")
        listener.tcp_port = -1;
      else
        throw std::runtime_error("unknown option " + arg);
    }
    if (options.state_dir.empty())
      throw std::runtime_error("--state-dir is required");
    std::filesystem::create_directories(options.state_dir);
    ::chmod(options.state_dir.c_str(), 0700);
    const StateDirectory state(options.state_dir);
    // The short-lived startup lock is released both on publication and on exit.
    auto starting = std::optional{
        fs::FileLock::acquire(options.state_dir / "host.starting")};
    std::optional<LockHolder> holder;
    auto lock = ProcessLock::TryAcquire(options.state_dir / "host.lock",
                                        options.application, holder);
    if (!state.IsCurrent())
      throw std::runtime_error(
          "The host state directory moved. Retry startup.");
    const auto ready_file = options.state_dir / "host.ready";
    const auto failure_file = options.state_dir / "host.startup-error";
    if (!lock) {
      const auto message = holder ? holder->Description() +
                                        " holds the host state directory. "
                                        "Quit it and retry."
                                  : "Another Tsugumi host holds the state "
                                    "directory. Quit that host and retry.";
      if (!move_aside.empty()) throw std::runtime_error(message);
      if (!ensure) return 4;
      if (const auto ready = fs::read_file(ready_file))
        std::cout << *ready << std::endl;
      else if (const auto failure = fs::read_file(failure_file))
        std::cout << *failure << std::endl;
      else {
        json::Members diagnostic{{"code", json::Quote("host_state_locked")},
                                 {"message", json::Quote(message)}};
        if (holder)
          diagnostic.emplace(
              "lock_holder",
              json::Encode(json::Members{
                  {"pid", std::to_string(holder->pid)},
                  {"application", json::Quote(holder->application)}}));
        std::cout << json::Encode(diagnostic) << std::endl;
        return 1;
      }
      return 0;
    }
    if (!move_aside.empty()) {
      const auto dir = std::filesystem::canonical(options.state_dir);
      int expected = 0;
      if (move_aside == dir / "host.sqlite")
        expected = HostStore::kHostFormatVersion;
      else if (move_aside == dir / "execution.sqlite")
        expected = ExecutionBroker::kExecutionFormatVersion;
      else
        throw std::runtime_error("Unknown host state file");
      sql::move_aside(sql::FormatMismatch(move_aside, found_format, expected));
      return 0;
    }
    state.Remove(ready_file);
    state.Remove(failure_file);
    if (listener.unix_path.empty())
      listener.unix_path = (options.state_dir / "agent.sock").string();
    options.user_token = crypto::random_token(32);
    options.external_token = crypto::random_token(32);
    options.server_id = crypto::uuid4();
    ::signal(SIGPIPE, SIG_IGN);
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGHUP);
    sigaddset(&signals, SIGUSR1);
    ::pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    // Detached project daemons must reconnect to the same broker after a
    // hostd restart, even when its UI RPC sockets get a new temp directory.
    if (broker_dir.empty()) {
      const auto* root = std::getenv("TSUGUMI_SOCKET_DIR");
      const auto key =
          crypto::sha256_hex(
              std::filesystem::canonical(options.state_dir).string())
              .substr(0, 16);
      broker_dir =
          std::filesystem::path(root ? root : "/tmp") /
          ((root ? "e-" : "tsugumi-exec-" + std::to_string(::getuid()) + "-") +
           key);
    }
    if (::mkdir(broker_dir.c_str(), 0700) != 0 && errno != EEXIST)
      throw std::runtime_error("Cannot create execution broker directory");
    struct stat broker_stat{};
    if (::lstat(broker_dir.c_str(), &broker_stat) != 0 ||
        !S_ISDIR(broker_stat.st_mode) || broker_stat.st_uid != ::getuid() ||
        (broker_stat.st_mode & 0077))
      throw std::runtime_error("Execution broker directory must be private");
    const auto binary_dir = std::filesystem::absolute(argv[0]).parent_path();
    if (options.environment.tsugumid.empty())
      options.environment.tsugumid = binary_dir / "tsugumid";
    if (options.environment.worker.empty())
      options.environment.worker = binary_dir / "tsugumi-worker";
    if (options.environment.nix_dir.empty())
      options.environment.nix_dir = binary_dir.parent_path() / "share/tsugumi/nix";
    if (options.environment.directory.empty())
      options.environment.directory = options.state_dir / "environments";
    options.environment.socket_directory = broker_dir / "env";
    if (!options.vm.binary.empty()) {
      if (options.vm.support.empty() || options.vm.image.empty())
        throw std::runtime_error(
            "VM helper support and image paths are required");
      options.environment.directory = options.vm.support / "environments";
    }
    options.broker_socket = (broker_dir / "s").string();
    if (web_root.empty())
      web_root = binary_dir.parent_path() / "share/tsugumi/ui/web/dist/public";
    const bool have_web = std::filesystem::exists(web_root / "index.html");
    const auto web_key = crypto::random_token(32);
    std::unique_ptr<Service> service;
    bool reported = false;
    while (!service) {
      try {
        service = std::make_unique<Service>(options);
      } catch (const sql::FormatMismatch& error) {
        log::error(error.what());
        const auto token = crypto::random_token(32);
        json::Members diagnostic{
            {"state_format_path", json::Quote(error.path.string())},
            {"message", json::Quote(error.what())},
            {"found_format", std::to_string(error.found)},
            {"recovery_token", json::Quote(token)}};
        if (!have_web) {
          if (ensure) std::cout << json::Encode(diagnostic) << std::endl;
          return 1;
        }
        std::mutex recovery_mutex;
        bool moved = false;
        const auto main_thread = pthread_self();
        WebEdgeOptions edge_options;
        edge_options.root = web_root;
        edge_options.port = web_port;
        edge_options.port_chosen = web_port_chosen;
        edge_options.key = web_key;
        edge_options.recovery = json::Encode(diagnostic);
        edge_options.recovery_token = token;
        edge_options.move_state_aside = [&] {
          std::lock_guard guard(recovery_mutex);
          if (moved) return;
          sql::move_aside(error);
          moved = true;
        };
        edge_options.state_moved = [main_thread] {
          pthread_kill(main_thread, SIGUSR1);
        };
        WebEdge recovery(std::move(edge_options), nullptr, nullptr);
        recovery.start();
        web_port = recovery.port();
        web_port_chosen = true;
        const auto url = recovery.page_url();
        diagnostic.emplace("recovery_url", json::Quote(url));
        diagnostic.emplace("pid", std::to_string(::getpid()));
        const auto result = json::Encode(diagnostic);
        fs::write_atomic(failure_file, result, 0600);
        starting.reset();
        if (ensure && !reported) std::cout << result << std::endl;
        if (!ensure)
          std::cout << "Tsugumi state recovery: " << url << std::endl;
        reported = true;
        int signal = 0;
        do {
          sigwait(&signals, &signal);
          if (signal != SIGUSR1) break;
          std::lock_guard guard(recovery_mutex);
          if (moved) break;
        } while (true);
        if (!state.IsCurrent()) return 1;
        starting.emplace(
            fs::FileLock::acquire(options.state_dir / "host.starting"));
        state.Remove(failure_file);
        recovery.stop();
        if (signal != SIGUSR1 || !state.IsCurrent()) return 1;
      }
    }
    auto& host = *service;
    TransportServer server(listener, &host);
    auto host_listener = listener;
    HostEndpoint(host_listener);
    host_listener.unix_path += ".host";
    if (host_listener.tcp_port >= 0) host_listener.tcp_port = 0;
    HostContract host_contract(host);
    TransportServer host_server(host_listener, &host_contract);
    host.Attach(&server, &host_server);
    server.start();
    host_server.start();
    // The Web UI is served whenever its built page is in place: the one given,
    // else an installation's share/tsugumi/ui/web/dist/public.
    if (have_web)
      host.web().Serve(web_root, web_port, web_port_chosen, web_key);
    else
      log::info("No Web UI at " + web_root.string() + "; not serving it");
    const auto endpoint = listener.tcp_port < 0
                              ? ""
                              : "ws://" + listener.tcp_host + ":" +
                                    std::to_string(server.bound_tcp_port()) +
                                    "/rpc";
    if (!state.IsCurrent())
      throw std::runtime_error(
          "The host state directory moved. Retry startup.");
    fs::write_atomic(
        ready_file,
        json::Encode(json::Members{
            {"endpoint", json::Quote(endpoint)},
            {"unix_socket", json::Quote(listener.unix_path)},
            {"host_socket", json::Quote(host_listener.unix_path)},
            {"execution_broker", json::Quote("unix:" + options.broker_socket)},
            {"host_endpoint",
             json::Quote(
                 host_listener.tcp_port < 0
                     ? ""
                     : "ws://" + host_listener.tcp_host + ":" +
                           std::to_string(host_server.bound_tcp_port()) +
                           "/rpc")},
            {"user_token", json::Quote(options.user_token)},
            {"external_token", json::Quote(options.external_token)},
            {"server_id", json::Quote(options.server_id)},
            {"web_url", json::Quote(host.web().Url())},
            {"pid", std::to_string(::getpid())}}),
        0600);
    starting.reset();
    if (ensure && !reported)
      std::cout << fs::read_file_or_throw(ready_file) << std::endl;
    int signal = 0;
    do {
      sigwait(&signals, &signal);
    } while (signal == SIGUSR1);
    state.Remove(ready_file);
    host.Shutdown();
    server.stop();
    host_server.stop();
    return 0;
  } catch (const std::exception& error) {
    log::error(std::string("Host service: ") + error.what());
    return 1;
  }
}

}  // namespace tsugumi::host

auto main(int argc, char** argv) -> int {
  return tsugumi::host::RunHost(argc, argv);
}
