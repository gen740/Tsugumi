#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "schema.hh"
#include "service.hh"
#include "tsugumi/core/daemon.hh"
#include "util/crypto.hh"
#include "util/fs.hh"
#include "util/log.hh"

namespace tsugumi::core {

namespace stdfs = std::filesystem;

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
  stdfs::path sdk_dir;
  stdfs::path worker;
  uint32_t cpus = 2;
  stdfs::path gcroots_dir;
  stdfs::path history_dir;
  stdfs::path host_folders;
  stdfs::path chat_folders;
  std::string fault;
};

void usage() {
  std::fprintf(stderr,
               "usage: tsugumid --project DIR [--unix PATH] [--listen HOST:PORT] [--control-dir DIR]\n"
               "                [--allow-origin ORIGIN]... [--allow-missing-origin] [--sdk-dir DIR]\n"
               "                [--worker PATH] [--cpus N] [--gcroots-dir DIR] [--history-dir DIR]\n"
               "                [--host-folders DIR] [--chat-folders DIR]\n");
}

std::optional<Options> parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
      return argv[++i];
    };
    if (a == "--project") o.project = next();
    else if (a == "--unix") o.unix_socket = next();
    else if (a == "--listen-unix-ws") o.ws_unix_socket = next();
    else if (a == "--listen") {
      std::string v = next();
      auto colon = v.rfind(':');
      if (colon == std::string::npos) throw std::runtime_error("--listen needs HOST:PORT");
      o.listen_host = v.substr(0, colon);
      o.listen_port = std::stoi(v.substr(colon + 1));
    } else if (a == "--control-dir") o.control_dir = next();
    else if (a == "--allow-origin") o.origins.push_back(next());
    else if (a == "--allow-missing-origin") o.allow_missing_origin = true;
    else if (a == "--sdk-dir") o.sdk_dir = next();
    else if (a == "--worker") o.worker = next();
    else if (a == "--cpus") o.cpus = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--gcroots-dir") o.gcroots_dir = next();
    else if (a == "--history-dir") o.history_dir = next();
    else if (a == "--host-folders") o.host_folders = next();
    else if (a == "--chat-folders") o.chat_folders = next();
    else if (a == "--help") return std::nullopt;
    else throw std::runtime_error("unknown option " + a);
  }
  if (o.project.empty()) throw std::runtime_error("--project is required");
  // Fault injection exists for the persistence tests only and needs an explicit opt-in.
  if (const char* f = std::getenv("TSUGUMI_FAULT"); f && std::getenv("TSUGUMI_ENABLE_FAULTS")) o.fault = f;
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
    log::error("project " + opt.project.string() + " is not an initialized Tsugumi project");
    return 2;
  }
  auto bin = exe_dir(argv[0]);
  if (opt.worker.empty()) opt.worker = bin / "tsugumi-worker";
  if (opt.sdk_dir.empty()) opt.sdk_dir = bin.parent_path() / "share" / "tsugumi" / "sdk";

  // Ownership: a fixed lock file, exclusive and non-blocking, held for life.
  auto state = root / ".tsugumi";
  stdfs::create_directories(state);
  ::chmod(state.c_str(), 0700);
  auto lock_path = state / "tsugumid.lock";
  std::optional<fs::FileLock> lock;
  try {
    if (auto l = fs::FileLock::try_acquire(lock_path)) lock.emplace(std::move(*l));
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
    bool enforced = fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    if (fd >= 0) ::close(fd);
    if (!enforced) {
      log::error("file locks are not enforced on this storage; refusing to serve " + root.string());
      return 5;
    }
  }

  ServiceOptions so;
  so.root = root;
  so.token = crypto::random_token(32);
  so.server_instance_id = crypto::uuid4();
  so.grants_file = opt.control_dir.empty() ? stdfs::path() : opt.control_dir / "grants.json";
  so.attached_file = opt.control_dir.empty() ? stdfs::path() : opt.control_dir / "attached.json";
  so.sdk_dir = opt.sdk_dir;
  so.worker_binary = opt.worker;
  so.fault = opt.fault;
  so.env_cpus = opt.cpus;
  so.gcroots_dir = opt.gcroots_dir;
  so.history_dir = opt.history_dir;
  so.host_folders = opt.host_folders;
  so.chat_folders = opt.chat_folders;
  // Block termination signals before threads start; they are taken with sigwait.
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGINT);
  ::pthread_sigmask(SIG_BLOCK, &set, nullptr);

  Service service(so);
  try {
    service.open();
  } catch (const std::exception& e) {
    log::error(std::string("cannot open the project: ") + e.what());
    return 6;
  }
  ServerOptions sv;
  sv.unix_path = opt.unix_socket;
  sv.ws_unix_path = opt.ws_unix_socket;
  sv.tcp_host = opt.listen_host;
  sv.tcp_port = opt.listen_port;
  sv.allowed_origins = opt.origins;
  if (!opt.control_dir.empty()) sv.origins_file = (opt.control_dir / "origins.json").string();
  sv.allow_missing_origin = opt.allow_missing_origin;
  Server server(sv, &service);
  service.attach(&server);
  try {
    server.start();
  } catch (const std::exception& e) {
    log::error(std::string("cannot start the RPC server: ") + e.what());
    return 7;
  }
  std::string endpoint =
      opt.listen_port >= 0 ? "ws://" + opt.listen_host + ":" + std::to_string(server.bound_tcp_port()) + "/rpc" : "";
  if (!opt.control_dir.empty()) {
    schema::ServerReady ready;
    *ready.endpoint = endpoint;
    *ready.token = so.token;
    *ready.protocol = 1;
    *ready.project_id = service.project_id();
    *ready.server_instance_id = so.server_instance_id;
    *ready.unix_socket = opt.unix_socket;
    *ready.pid = ::getpid();
    stdfs::create_directories(opt.control_dir);
    fs::write_atomic(opt.control_dir / "server.ready", schema::encode(ready), 0600);
  }
  log::info("ready project_id=" + service.project_id() + (endpoint.empty() ? "" : " endpoint=" + endpoint) +
            (opt.unix_socket.empty() ? "" : " unix=" + opt.unix_socket));
  int sig = 0;
  sigwait(&set, &sig);
  log::info(std::string("stopping on ") + (sig == SIGTERM ? "SIGTERM" : "SIGINT"));
  if (!opt.control_dir.empty()) ::unlink((opt.control_dir / "server.ready").c_str());
  service.shutdown();
  server.stop();
  return 0;
}

}  // namespace tsugumi::core
