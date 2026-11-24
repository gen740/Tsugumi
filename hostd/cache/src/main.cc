// tsugumi-cached: Tsugumi's own Nix binary cache server.
//
//   tsugumi-cached [serve] [--state-dir DIR] [--host ADDR] [--port N]
//                  [--socket PATH] [--name NAME]... [--authorized-keys FILE]
//                  [--store-dir DIR]
//   tsugumi-cached public-key [--state-dir DIR]
//   tsugumi-cached usage [--state-dir DIR] [--json]
//
// Every connection is mutual TLS 1.3. The client presents an Ed25519
// certificate whose public key is listed in the authorized keys file, the
// same `ed25519:` keys as Execution Services registrations. See
// docs/BinaryCacheServer.md.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <nix/store/globals.hh>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "tsugumi/cache/cache.hh"
#include "tsugumi/cache/http.hh"
#include "tsugumi/rpc/peer_tls.hh"
#include "tsugumi/rpc/unix_socket.hh"
#include "tsugumi/util/fs.hh"

namespace {
namespace stdfs = std::filesystem;
namespace cache = tsugumi::cache;
namespace peer_tls = tsugumi::peer_tls;
namespace fs = tsugumi::fs;

constexpr size_t kConnections = 64;
// A stalled client releases its connection; Nix's transfers stream steadily.
constexpr int kSocketTimeoutSeconds = 300;

std::atomic<bool> stopping{false};
auto Stop(int) -> void { stopping = true; }

struct Fd {
  int value = -1;
  explicit Fd(int fd) : value(fd) {}
  ~Fd() {
    if (value >= 0) ::close(value);
  }
  Fd(const Fd&) = delete;
  auto operator=(const Fd&) -> Fd& = delete;
};

struct Config {
  std::string command = "serve";
  stdfs::path state;
  std::string host = "127.0.0.1";
  int port = 8766;
  stdfs::path socket;
  std::vector<std::string> names;
  stdfs::path authorized_keys;
  std::string store_dir = "/nix/store";
  bool json = false;
};

// The files of one cache, all below its state folder.
struct Layout {
  explicit Layout(stdfs::path state) : state(std::move(state)) {}
  stdfs::path state;
  auto signing_key() const { return state / "signing-key"; }
  auto signing_public_key() const { return state / "signing-key.pub"; }
  auto identity() const { return state / "tls-identity.pem"; }
  auto certificate() const { return state / "tls-certificate.pem"; }
  auto root() const { return state / "binary-cache"; }
  auto uploads() const { return state / "uploads"; }
  // The --socket path this state's server listens on.
  auto socket_record() const { return state / "server.socket"; }
};

auto DefaultState() -> stdfs::path {
  if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg)
    return stdfs::path(xdg) / "tsugumi/cache";
  const char* home = std::getenv("HOME");
  if (!home || !*home) throw std::runtime_error("Set --state-dir or HOME");
  return stdfs::path(home) / ".local/state/tsugumi/cache";
}

auto PrivateDirectory(const stdfs::path& path) -> void {
  stdfs::create_directories(path);
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != ::geteuid())
    throw std::runtime_error("State folder must be an owned folder: " +
                             path.string());
  if (st.st_mode & 0077) ::chmod(path.c_str(), 0700);
}

auto WriteIfChanged(const stdfs::path& file, const std::string& text) -> void {
  if (fs::read_file(file) != text) fs::write_atomic(file, text, 0644);
}

// The signing key and TLS identity, created on first use.
auto Keys(const Layout& layout, const std::vector<std::string>& names)
    -> std::pair<std::string, std::unique_ptr<peer_tls::Identity>> {
  auto secret = cache::LoadOrCreateSecretKey(layout.signing_key());
  WriteIfChanged(layout.signing_public_key(),
                 cache::PublicKeyOf(secret) + "\n");
  auto identity =
      std::make_unique<peer_tls::Identity>(layout.identity(), names);
  WriteIfChanged(layout.certificate(), identity->certificate_pem());
  return {std::move(secret), std::move(identity)};
}

// Whether something accepts connections on a Unix socket; anything but a
// refused connection counts as in use.
auto Listening(const sockaddr_un& address) -> bool {
  const Fd probe(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (probe.value < 0) return true;
  return ::connect(probe.value, reinterpret_cast<const sockaddr*>(&address),
                   sizeof address) == 0 ||
         errno != ECONNREFUSED;
}

// Called with the state lock held. An existing socket is removed only when
// this state's server left it behind (it is the recorded path) and nothing
// listens on it any more.
auto UnixListener(const stdfs::path& path, const stdfs::path& record) -> int {
  if (!path.is_absolute())
    throw std::runtime_error("--socket must be an absolute path");
  const auto address = tsugumi::unix_socket::Address(path.string());
  struct stat st{};
  if (::lstat(path.c_str(), &st) == 0) {
    if (!S_ISSOCK(st.st_mode) || st.st_uid != ::geteuid() ||
        fs::read_file(record) != path.string() || Listening(address))
      throw std::runtime_error("--socket names a path in use: " +
                               path.string());
    ::unlink(path.c_str());
  }
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 ||
      ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) !=
          0 ||
      ::listen(fd, 64) != 0)
    throw std::runtime_error("Cannot listen on " + path.string());
  ::chmod(path.c_str(), 0600);
  fs::write_atomic(record, path.string(), 0600);
  return fd;
}

auto TcpListener(Config& config) -> int {
  sockaddr_in v4{};
  sockaddr_in6 v6{};
  v4.sin_family = AF_INET;
  v4.sin_port = htons(static_cast<uint16_t>(config.port));
  v6.sin6_family = AF_INET6;
  v6.sin6_port = htons(static_cast<uint16_t>(config.port));
  const bool is_v4 = ::inet_pton(AF_INET, config.host.c_str(), &v4.sin_addr);
  const bool is_v6 =
      !is_v4 && ::inet_pton(AF_INET6, config.host.c_str(), &v6.sin6_addr);
  if (!is_v4 && !is_v6)
    throw std::runtime_error("--host must be an IP literal");
  const int fd = ::socket(is_v4 ? AF_INET : AF_INET6, SOCK_STREAM, 0);
  const int one = 1;
  if (fd < 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 ||
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
      ::bind(fd,
             is_v4 ? reinterpret_cast<sockaddr*>(&v4)
                   : reinterpret_cast<sockaddr*>(&v6),
             is_v4 ? sizeof v4 : sizeof v6) != 0 ||
      ::listen(fd, 64) != 0)
    throw std::runtime_error("Cannot listen on " + config.host + ":" +
                             std::to_string(config.port));
  sockaddr_storage bound{};
  socklen_t size = sizeof bound;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &size) != 0)
    throw std::runtime_error("Cannot read the listener's port");
  config.port =
      ntohs(is_v4 ? reinterpret_cast<sockaddr_in*>(&bound)->sin_port
                  : reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
  return fd;
}

class TlsConnection : public cache::Connection {
 public:
  explicit TlsConnection(peer_tls::Stream& stream) : stream_(stream) {}
  auto ReadSome(size_t limit) -> std::string override {
    return stream_.ReadSome(limit);
  }
  auto Write(std::string_view bytes) -> void override { stream_.Write(bytes); }

 private:
  peer_tls::Stream& stream_;
};

auto Serve(Config config) -> int {
  const Layout layout(config.state);
  PrivateDirectory(layout.state);
  Fd lock(::open((layout.state / "server.lock").c_str(),
                 O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (lock.value < 0 || ::flock(lock.value, LOCK_EX | LOCK_NB) != 0)
    throw std::runtime_error("Another cache server owns " +
                             layout.state.string());
  // Names a client may use in the URL; localhost is where a relayed
  // connection arrives on a worker.
  auto names = config.names;
  for (const std::string_view name : {"localhost", "127.0.0.1", "[::1]"})
    if (std::ranges::find(names, name) == names.end()) names.emplace_back(name);
  auto [secret, identity] = Keys(layout, names);
  if (config.authorized_keys.empty())
    config.authorized_keys = layout.state / "authorized_keys";
  if (!stdfs::exists(config.authorized_keys))
    fs::write_atomic(config.authorized_keys,
                     "# One ed25519: SPKI key per line.\n", 0600);
  cache::Cache store(layout.root(), layout.uploads(), secret, config.store_dir);
  peer_tls::Context tls(*identity, true, [&](std::string_view key) {
    return peer_tls::Authorized(config.authorized_keys, key);
  });

  Fd tcp(TcpListener(config));
  std::optional<Fd> unix;
  if (!config.socket.empty())
    unix.emplace(UnixListener(config.socket, layout.socket_record()));
  ::signal(SIGTERM, Stop);
  ::signal(SIGINT, Stop);
  ::signal(SIGPIPE, SIG_IGN);

  const auto host = config.host.find(':') == config.host.npos
                        ? config.host
                        : "[" + config.host + "]";
  std::cout << "tsugumi-cached: serving " << layout.root().string() << '\n'
            << "url: https://" << host << ":" << config.port << '\n'
            << "signing public key: " << store.public_key() << '\n'
            << "tls public key: " << identity->public_key() << '\n'
            << "certificate: " << layout.certificate().string() << '\n'
            << "authorized keys: " << config.authorized_keys.string() << '\n';
  if (unix) std::cout << "socket: " << config.socket.string() << '\n';
  std::cout << std::flush;

  std::mutex mu;
  std::set<int> open;
  std::atomic<size_t> active{0};
  struct Client {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<Client> clients;
  while (!stopping) {
    std::erase_if(clients, [](Client& client) {
      if (!client.done->load()) return false;
      client.thread.join();
      return true;
    });
    std::vector<pollfd> listeners{{tcp.value, POLLIN, 0}};
    if (unix) listeners.push_back({unix->value, POLLIN, 0});
    if (::poll(listeners.data(), listeners.size(), 200) <= 0) continue;
    for (const auto& listener : listeners) {
      if (!(listener.revents & POLLIN)) continue;
      const int fd = ::accept(listener.fd, nullptr, nullptr);
      if (fd < 0) continue;
      if (active.load() >= kConnections) {
        ::close(fd);
        continue;
      }
      ++active;
      {
        std::lock_guard guard(mu);
        open.insert(fd);
      }
      auto done = std::make_shared<std::atomic<bool>>(false);
      clients.push_back({std::thread([&, fd, done] {
                           try {
                             peer_tls::ConfigureSocket(fd,
                                                       kSocketTimeoutSeconds);
                             peer_tls::Stream stream(tls, fd, true);
                             TlsConnection connection(stream);
                             cache::Serve(store, connection, stream.peer_key());
                           } catch (const std::exception&) {
                             // A refused handshake or lost client: its upload,
                             // if any, was dropped without becoming visible.
                           }
                           {
                             std::lock_guard guard(mu);
                             open.erase(fd);
                           }
                           ::close(fd);
                           --active;
                           done->store(true);
                         }),
                         done});
    }
  }
  {
    // Unblock every connection; an upload in progress is dropped.
    std::lock_guard guard(mu);
    for (const int fd : open) ::shutdown(fd, SHUT_RDWR);
  }
  for (auto& client : clients) client.thread.join();
  if (unix) {
    ::unlink(config.socket.c_str());
    ::unlink(layout.socket_record().c_str());
  }
  return 0;
}

auto PrintUsage(const Config& config) -> int {
  const Layout layout(config.state);
  const auto usage = cache::Measure(layout.root(), layout.uploads());
  if (config.json) {
    std::cout << "{\"bytes\":" << usage.bytes << ",\"paths\":" << usage.paths
              << ",\"nars\":" << usage.nars
              << ",\"incompleteFiles\":" << usage.incomplete_files
              << ",\"incompleteBytes\":" << usage.incomplete_bytes << "}\n";
  } else {
    std::cout << "cache folder: " << layout.root().string() << '\n'
              << "size: " << usage.bytes << " bytes\n"
              << "store paths: " << usage.paths << '\n'
              << "NAR files: " << usage.nars << '\n';
    if (usage.incomplete_files)
      std::cout << "unfinished uploads: " << usage.incomplete_files << " ("
                << usage.incomplete_bytes << " bytes in "
                << layout.uploads().string() << ", never served)\n";
  }
  return 0;
}

// Shows the keys the server created; it never creates or rewrites a file.
auto PrintKeys(const Config& config) -> int {
  const Layout layout(config.state);
  auto signing = fs::read_file(layout.signing_public_key());
  if (!signing || !stdfs::exists(layout.identity()))
    throw std::runtime_error("No keys in " + layout.state.string() +
                             " yet: start the server once");
  while (!signing->empty() && signing->back() == '\n') signing->pop_back();
  const peer_tls::Identity identity(layout.identity());
  std::cout << "signing public key: " << *signing << '\n'
            << "tls public key: " << identity.public_key() << '\n';
  return 0;
}

auto Parse(int argc, char** argv) -> Config {
  Config config;
  int i = 1;
  if (i < argc && argv[i][0] != '-') config.command = argv[i++];
  for (; i < argc; ++i) {
    const std::string_view option = argv[i];
    if (option == "--json") {
      config.json = true;
      continue;
    }
    if (i + 1 >= argc)
      throw std::runtime_error("Missing value for " + std::string(option));
    const std::string value = argv[++i];
    if (option == "--state-dir")
      config.state = value;
    else if (option == "--host")
      config.host = value;
    else if (option == "--port")
      config.port = std::stoi(value);
    else if (option == "--socket")
      config.socket = value;
    else if (option == "--name")
      config.names.push_back(value);
    else if (option == "--authorized-keys")
      config.authorized_keys = value;
    else if (option == "--store-dir")
      config.store_dir = value;
    else
      throw std::runtime_error("Unknown option " + std::string(option));
  }
  if (config.state.empty()) config.state = DefaultState();
  if (config.port < 0 || config.port > 65535)
    throw std::runtime_error("--port must be 0-65535");
  return config;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  ::umask(0077);
  try {
    const auto config = Parse(argc, argv);
    nix::initLibStore(false);
    if (config.command == "serve") return Serve(config);
    if (config.command == "usage") return PrintUsage(config);
    if (config.command == "public-key") return PrintKeys(config);
    throw std::runtime_error("Unknown command " + config.command);
  } catch (const std::exception& error) {
    std::cerr << "tsugumi-cached: " << error.what() << '\n';
    return 1;
  }
}
