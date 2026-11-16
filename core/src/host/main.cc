#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <thread>

#include "agent/json.hh"
#include "host/service.hh"
#include "host/state_format.hh"
#include "util/crypto.hh"
#include "util/fs.hh"
#include "util/log.hh"

namespace tsugumi::host {
namespace json = agent::json;
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
}  // namespace

auto RunHost(int argc, char** argv) -> int {
  try {
    HostOptions options;
    ServerOptions listener;
    listener.protocol = ServerOptions::Protocol::Agent;
    listener.tcp_host = "127.0.0.1";
    listener.tcp_port = 0;
    listener.allow_missing_origin = true;
    std::filesystem::path broker_dir;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      auto next = [&]() -> std::string {
        if (++i == argc) throw std::runtime_error(arg + " requires a value");
        return argv[i];
      };
      if (arg == "--state-dir")
        options.state_dir = next();
      else if (arg == "--unix")
        listener.unix_path = next();
      else if (arg == "--vmhost")
        options.vm.binary = next();
      else if (arg == "--vm-support")
        options.vm.support = next();
      else if (arg == "--vm-image")
        options.vm.image = next();
      else if (arg == "--vm-source")
        options.vm.source = next();
      else if (arg == "--vm-source-entry")
        options.vm.source_entries.push_back(next());
      else if (arg == "--vm-cpus")
        options.vm.cpus = std::stoul(next());
      else if (arg == "--vm-memory")
        options.vm.memory = std::stoul(next());
      else if (arg == "--vm-store")
        options.vm.store = std::stoul(next());
      else if (arg == "--tsugumid")
        options.environment.tsugumid = next();
      else if (arg == "--worker")
        options.environment.worker = next();
      else if (arg == "--sdk-dir")
        options.environment.sdk = next();
      else if (arg == "--environment-dir")
        options.environment.directory = next();
      else if (arg == "--socket-dir")
        broker_dir = next();
      else if (arg == "--usage-scratch-dir")
        options.environment.run_scratch_directory = next();
      else if (arg == "--demos-dir")
        options.demos_dir = next();
      else if (arg == "--allow-origin")
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
    PrepareStateDirectory(options.state_dir);
    auto lock = fs::FileLock::try_acquire(options.state_dir / "host.lock");
    if (!lock) return 4;
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
    if (options.environment.sdk.empty())
      options.environment.sdk = binary_dir.parent_path() / "share/tsugumi/sdk";
    if (options.environment.directory.empty())
      options.environment.directory = options.state_dir / "environments";
    options.environment.socket_directory = broker_dir / "env";
    if (!options.vm.binary.empty()) {
      if (options.vm.support.empty() || options.vm.image.empty() ||
          options.vm.source.empty())
        throw std::runtime_error(
            "VM helper support, image and source paths are required");
      options.environment.directory = options.vm.support / "environments";
      options.environment.start_timeout = std::chrono::seconds(90);
    }
    options.broker_socket = (broker_dir / "s").string();
    Service host(options);
    listener.idle_timeout = std::chrono::seconds(30);
    Server server(listener, &host);
    auto host_listener = listener;
    host_listener.protocol = ServerOptions::Protocol::Host;
    host_listener.unix_path += ".host";
    if (host_listener.tcp_port >= 0) host_listener.tcp_port = 0;
    HostContract host_contract(host);
    Server host_server(host_listener, &host_contract);
    host.Attach(&server, &host_server);
    server.start();
    host_server.start();
    const auto endpoint = listener.tcp_port < 0
                              ? ""
                              : "ws://" + listener.tcp_host + ":" +
                                    std::to_string(server.bound_tcp_port()) +
                                    "/rpc";
    fs::write_atomic(
        options.state_dir / "host.ready",
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
            {"pid", std::to_string(::getpid())},
            {"protocol", "1"}}),
        0600);
    std::jthread idle_monitor([&](std::stop_token stop) {
      auto since = std::chrono::steady_clock::now();
      while (!stop.stop_requested()) {
        if (!host.CanDrain()) since = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - since >= std::chrono::seconds(30)) host.DrainIdle();
        if (server.idle_expired()) {
          ::kill(::getpid(), SIGTERM);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
    });
    int signal = 0;
    sigwait(&signals, &signal);
    idle_monitor.request_stop();
    // Keep the guest stop bounded even if draining a host request hangs.
    // A second termination signal also ends hostd; vmhost observes Watch EOF
    // and independently completes its bounded guest shutdown.
    std::thread([signals]() mutable {
      int again = 0;
      sigwait(&signals, &again);
      std::_Exit(128 + again);
    }).detach();
    std::jthread deadline([](std::stop_token stop) {
      for (int i = 0; i < 450 && !stop.stop_requested(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (!stop.stop_requested()) std::_Exit(1);
    });
    fs::remove_file_durable(options.state_dir / "host.ready");
    host.Shutdown();
    server.stop();
    host_server.stop();
    deadline.request_stop();
    return 0;
  } catch (const StateFormatError& error) {
    log::error(error.what());
    return 5;
  } catch (const std::exception& error) {
    log::error(std::string("Host service: ") + error.what());
    return 1;
  }
}

}  // namespace tsugumi::host

auto main(int argc, char** argv) -> int {
  return tsugumi::host::RunHost(argc, argv);
}
