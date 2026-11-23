#include "tsugumi/protocol/runtime_state_generated.h"
#include "tsugumi/util/flat_file.hh"
#include "tsugumi/util/json_object.hh"
#include "agent_fixture_server.hh"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/inotify.h>
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <hosq/hosq.hh>
#include <hosq/json/compact_writer.hh>
#include <hosq/json/dom.hh>
#include <iterator>
#include <stdexcept>

extern char** environ;

namespace tsugumi::test {
namespace {

struct Descriptor {
  int fd;
  ~Descriptor() {
    if (fd >= 0) ::close(fd);
  }
};

auto WaitForEvent(const std::filesystem::path& directory, int pid,
                  const std::function<bool()>& done, int timeout = -1) -> bool {
#ifdef __APPLE__
  Descriptor queue{::kqueue()};
  Descriptor watched{directory.empty()
                         ? -1
                         : ::open(directory.c_str(), O_EVTONLY | O_CLOEXEC)};
  if (queue.fd < 0 || (!directory.empty() && watched.fd < 0))
    throw std::runtime_error("cannot open fixture event sources");
  struct kevent registrations[2];
  int count = 0;
  if (watched.fd >= 0) {
    EV_SET(&registrations[count], watched.fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, nullptr);
    ++count;
  }
  if (pid > 0) {
    EV_SET(&registrations[count], pid, EVFILT_PROC, EV_ADD | EV_ONESHOT,
           NOTE_EXIT, 0, nullptr);
    ++count;
  }
  if (::kevent(queue.fd, registrations, count, nullptr, 0, nullptr) < 0) {
    if (errno == ESRCH) return done();
    throw std::runtime_error("cannot register fixture events");
  }
  const timespec limit{timeout / 1000, (timeout % 1000) * 1000000L};
  while (!done()) {
    struct kevent event{};
    const auto received = ::kevent(queue.fd, nullptr, 0, &event, 1,
                                   timeout < 0 ? nullptr : &limit);
    if (received < 0 && errno == EINTR) continue;
    if (received < 0) throw std::runtime_error("cannot read fixture event");
    if (received == 0 || event.filter == EVFILT_PROC) return done();
  }
#else
  Descriptor process{pid > 0 ? static_cast<int>(::syscall(SYS_pidfd_open, pid, 0))
                             : -1};
  if (pid > 0 && process.fd < 0) {
    if (errno == ESRCH) return done();
    throw std::runtime_error("cannot watch fixture process");
  }
  Descriptor files{directory.empty() ? -1 : ::inotify_init1(IN_CLOEXEC)};
  if (!directory.empty() &&
      (files.fd < 0 ||
       ::inotify_add_watch(files.fd, directory.c_str(),
                           IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE |
                               IN_DELETE | IN_MOVED_FROM) < 0))
    throw std::runtime_error("cannot watch fixture directory");
  pollfd fds[]{{process.fd, POLLIN, 0}, {files.fd, POLLIN, 0}};
  while (!done()) {
    const auto received = ::poll(fds, 2, timeout);
    if (received < 0 && errno == EINTR) continue;
    if (received < 0) throw std::runtime_error("cannot read fixture event");
    if (received == 0 || fds[0].revents) return done();
    char bytes[4096];
    if (::read(files.fd, bytes, sizeof bytes) < 0 && errno != EINTR)
      throw std::runtime_error("cannot read fixture file event");
  }
#endif
  return true;
}

auto Parse(std::string_view text) -> hosq::json::GenericDom<> {
  hosq::json::GenericDom<> json;
  if (auto error = json.assign(text))
    throw std::runtime_error("Invalid JSON from RPC peer: " +
                             std::string(error.detail));
  return json;
}

auto Send(int socket, std::string_view bytes) -> bool {
  while (!bytes.empty()) {
#ifdef MSG_NOSIGNAL
    const auto sent = ::send(socket, bytes.data(), bytes.size(), MSG_NOSIGNAL);
#else
    const auto sent = ::send(socket, bytes.data(), bytes.size(), 0);
#endif
    if (sent < 0 && errno == EINTR) continue;
    if (sent <= 0) return false;
    bytes.remove_prefix(static_cast<size_t>(sent));
  }
  return true;
}

auto Event(std::string_view json) -> std::string {
  return "data: " + std::string(json) + "\n\n";
}

}  // namespace

ChildProcess::~ChildProcess() { Stop(); }

auto ChildProcess::Start(const std::vector<std::string>& arguments,
                         const std::filesystem::path& log) -> void {
  if (pid_ != -1) throw std::runtime_error("child process already started");
  if (arguments.empty())
    throw std::runtime_error("child process needs an executable");
  std::filesystem::create_directories(log.parent_path());
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                   O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(),
                                   O_CREAT | O_WRONLY | O_TRUNC, 0600);
  posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
  std::vector<char*> argv;
  for (const auto& argument : arguments)
    argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
  pid_t pid = -1;
  const auto result =
      posix_spawn(&pid, argv.front(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (result)
    throw std::runtime_error("cannot start child process: " +
                             std::string(std::strerror(result)));
  pid_ = pid;
}

auto ChildProcess::Alive() -> bool {
  if (pid_ == -1) return false;
  int status = 0;
  const auto found = ::waitpid(pid_, &status, WNOHANG);
  if (found == 0) return true;
  if (found == -1 && errno == EINTR) return true;
  pid_ = -1;
  return false;
}

auto ChildProcess::Stop(bool crash) -> void {
  if (!Alive()) return;
  ::kill(pid_, crash ? SIGKILL : SIGTERM);
  if (!crash) {
    // Escalate if a broken fixture does not honor graceful shutdown.
    try {
      (void)WaitForEvent({}, pid_, [&] { return !Alive(); }, 5000);
    } catch (const std::exception&) {
      // The destructor still owns this process if the OS cannot watch it.
    }
  }
  if (pid_ != -1) {
    ::kill(pid_, SIGKILL);
    int status = 0;
    while (::waitpid(pid_, &status, 0) == -1 && errno == EINTR) {
    }
    pid_ = -1;
  }
}

TemporaryDirectory::TemporaryDirectory() {
  // Keep Unix socket names below sockaddr_un.sun_path on macOS.
  std::string pattern =
      (std::filesystem::path(std::getenv("TSUGUMI_SOCKET_DIR")
                                 ? std::getenv("TSUGUMI_SOCKET_DIR")
                                 : "/tmp") /
       "th-XXXXXX")
          .string();
  if (!::mkdtemp(pattern.data()))
    throw std::runtime_error("cannot create temporary project");
  path = pattern;
}

TemporaryDirectory::~TemporaryDirectory() {
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
}

auto Write(const std::filesystem::path& path, std::string_view text) -> void {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file << text;
  if (!file) throw std::runtime_error("cannot write test resource");
}

auto Read(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

auto WaitForPath(const std::filesystem::path& path,
                 const std::function<bool()>& ready, int pid) -> bool {
  auto directory = path.parent_path();
  while (!std::filesystem::exists(directory)) directory = directory.parent_path();
  while (true) {
    auto next = directory;
    const auto done = [&] {
      if (ready()) return true;
      next = path.parent_path();
      while (!std::filesystem::exists(next)) next = next.parent_path();
      return next != directory;
    };
    if (!WaitForEvent(directory, pid, done)) return false;
    if (next == directory) return true;
    directory = std::move(next);
  }
}

auto Readiness(ChildProcess& process, const std::filesystem::path& path,
               const std::filesystem::path& log) -> std::string {
  if (WaitForPath(path, [&] { return std::filesystem::is_regular_file(path); },
                  process.Pid())) {
    const auto bytes = Read(path);
    if (path.filename() != "server.ready") return bytes;
    const auto ready = flat::decode<Tsugumi::State::ServerReady>(bytes);
    if (!ready) throw std::runtime_error("invalid project readiness");
    return json::Encode(json::Members{
        {"endpoint", json::Quote(ready->endpoint)},
        {"token", json::Quote(ready->token)},
        {"project_id", json::Quote(ready->project_id)},
        {"unix_socket", json::Quote(ready->unix_socket)},
        {"protocol", std::to_string(ready->protocol)},
        {"pid", std::to_string(ready->pid)},
    });
  }
  throw std::runtime_error("daemon exited during startup:\n" + Read(log));
}

ModelServer::ModelServer() {
  socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket_ < 0)
    throw std::runtime_error("cannot create model fixture socket");
  ::fcntl(socket_, F_SETFD, FD_CLOEXEC);
  ::fcntl(socket_, F_SETFL, O_NONBLOCK);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof address) ||
      ::listen(socket_, 16)) {
    ::close(socket_);
    throw std::runtime_error("cannot listen for model requests");
  }
  socklen_t length = sizeof address;
  ::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length);
  port_ = ntohs(address.sin_port);
  thread_ = std::jthread([this](std::stop_token stop) { Serve(stop); });
}

ModelServer::~ModelServer() {
  thread_.request_stop();
  changed_.notify_all();
  {
    std::lock_guard lock(mutex_);
    if (peer_ >= 0) ::shutdown(peer_, SHUT_RDWR);
  }
  if (thread_.joinable()) thread_.join();
  ::close(socket_);
}

auto ModelServer::URL() const -> std::string {
  return "http://127.0.0.1:" + std::to_string(port_);
}

auto ModelServer::Queue(std::string stream, bool hold, bool fragment) -> void {
  std::lock_guard lock(mutex_);
  responses_.push_back({std::move(stream), hold, fragment});
  changed_.notify_all();
}

auto ModelServer::QueueHTTP(std::string body, int status) -> void {
  std::lock_guard lock(mutex_);
  responses_.push_back(
      {std::move(body), false, false, status, "application/json"});
  changed_.notify_all();
}

auto ModelServer::WaitForRequests(size_t count, std::stop_token stop) -> bool {
  std::unique_lock lock(mutex_);
  return changed_.wait(lock, stop, [&] { return requests_.size() >= count; });
}

// A negative assertion has no provider-side arrival event to wait for.
// Allow an unexpected asynchronous request to reach the provider double.
auto ModelServer::WaitForRequests(size_t count,
                                  std::chrono::milliseconds timeout) -> bool {
  std::unique_lock lock(mutex_);
  return changed_.wait_for(lock, timeout,
                           [&] { return requests_.size() >= count; });
}

auto ModelServer::Requests() -> std::vector<std::string> {
  std::lock_guard lock(mutex_);
  return requests_;
}

auto ModelServer::Headers() -> std::vector<std::string> {
  std::lock_guard lock(mutex_);
  return headers_;
}

auto ModelServer::Release() -> void {
  std::lock_guard lock(mutex_);
  released_ = true;
  changed_.notify_all();
}

auto ModelServer::Serve(std::stop_token stop) -> void {
  int wake[2];
  if (::pipe(wake) != 0) throw std::runtime_error("cannot create model stop pipe");
  Descriptor stopped{wake[0]}, signal{wake[1]};
  for (const auto fd : wake) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  // Darwin cannot shut down a listening socket to interrupt accept.
  const std::stop_callback canceled(stop, [&] {
    const char byte = 0;
    (void)::write(signal.fd, &byte, 1);
  });
  while (!stop.stop_requested()) {
    pollfd sources[]{{socket_, POLLIN, 0}, {stopped.fd, POLLIN, 0}};
    const auto ready = ::poll(sources, 2, -1);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0 || sources[1].revents) return;
    const auto socket = ::accept(socket_, nullptr, nullptr);
    if (socket < 0) continue;
    {
      std::lock_guard lock(mutex_);
      if (stop.stop_requested()) {
        ::close(socket);
        return;
      }
      peer_ = socket;
    }
    ::fcntl(socket, F_SETFD, FD_CLOEXEC);
    ::fcntl(socket, F_SETFL, 0);
#ifdef SO_NOSIGPIPE
    int yes = 1;
    ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
    try {
      Handle(socket, stop);
    } catch (...) {
    }
    std::lock_guard lock(mutex_);
    peer_ = -1;
    ::close(socket);
  }
}

auto ModelServer::Handle(int socket, std::stop_token stop) -> void {
  std::string request;
  size_t body_start = std::string::npos;
  size_t body_size = 0;
  while (!stop.stop_requested()) {
    char bytes[8192];
    const auto count = ::recv(socket, bytes, sizeof bytes, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return;
    request.append(bytes, static_cast<size_t>(count));
    if (request.size() > (16u << 20)) return;
    if (body_start == std::string::npos) {
      const auto at = request.find("\r\n\r\n");
      if (at == std::string::npos) continue;
      body_start = at + 4;
      auto headers = request.substr(0, at);
      std::transform(headers.begin(), headers.end(), headers.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      const auto length = headers.find("content-length:");
      if (length != std::string::npos) {
        auto value = std::string_view(headers).substr(length + 15);
        while (value.starts_with(' ')) value.remove_prefix(1);
        std::from_chars(value.data(), value.data() + value.size(), body_size);
      }
    }
    if (request.size() >= body_start + body_size) break;
  }
  if (stop.stop_requested()) return;
  Response response;
  {
    std::unique_lock lock(mutex_);
    requests_.push_back(request.substr(body_start, body_size));
    headers_.push_back(request.substr(0, body_start));
    changed_.notify_all();
    if (!changed_.wait(lock, stop, [&] { return !responses_.empty(); })) return;
    response = std::move(responses_.front());
    responses_.pop_front();
  }
  if (!Send(socket, "HTTP/1.1 " + std::to_string(response.status) +
                        " Fixture\r\nContent-Type: " + response.content_type +
                        "\r\nConnection: close\r\n\r\n"))
    return;
  if (response.hold) {
    if (!Send(socket, ": waiting for the test\n\n")) return;
    std::unique_lock lock(mutex_);
    if (!changed_.wait(lock, stop, [&] { return released_; })) return;
  }
  if (response.fragment) {
    for (size_t i = 0; i < response.stream.size() && !stop.stop_requested();
         i += 3) {
      if (!Send(socket, std::string_view(response.stream).substr(i, 3))) return;
    }
  } else {
    Send(socket, response.stream);
  }
}

auto Quote(std::string_view text) -> std::string {
  hosq::types::value::String value;
  value.value() = text;
  std::string out;
  hosq::json::CompactWriter writer(out);
  if (!hosq::encode_into(value, writer))
    throw std::runtime_error("cannot encode test string");
  return out;
}

auto JsonString(std::string_view json, std::string_view pointer)
    -> std::string {
  hosq::types::value::String value;
  if (Parse(json)[pointer].decode_into(value))
    throw std::runtime_error("RPC reply has no string at " +
                             std::string(pointer));
  return value.value();
}

auto JsonBool(std::string_view json, std::string_view pointer) -> bool {
  hosq::types::value::Bool value;
  if (Parse(json)[pointer].decode_into(value))
    throw std::runtime_error("RPC reply has no boolean at " +
                             std::string(pointer));
  return value.value();
}

auto JsonUInt(std::string_view json, std::string_view pointer) -> uint64_t {
  hosq::types::value::Uint64 value;
  if (Parse(json)[pointer].decode_into(value))
    throw std::runtime_error("RPC reply has no integer at " +
                             std::string(pointer));
  return value.value();
}

auto AnthropicText(std::string_view text, uint64_t input_tokens)
    -> std::string {
  return Event(
             "{\"type\":\"message_start\",\"message\":{\"usage\":{\"input_"
             "tokens\":" +
             std::to_string(input_tokens) + "}}}") +
         Event(
             R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})") +
         Event(
             "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
             "\"type\":\"text_delta\",\"text\":" +
             Quote(text) + "}}") +
         Event(R"({"type":"content_block_stop","index":0})") +
         Event(
             R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":4}})") +
         Event(R"({"type":"message_stop"})");
}

auto AnthropicTool(std::string_view id, std::string_view name,
                   std::string_view arguments) -> std::string {
  return Event(
             R"({"type":"message_start","message":{"usage":{"input_tokens":12}}})") +
         Event(
             "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
             "\"type\":\"tool_use\",\"id\":" +
             Quote(id) + ",\"name\":" + Quote(name) + ",\"input\":{}}}") +
         Event(
             "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
             "\"type\":\"input_json_delta\",\"partial_json\":" +
             Quote(arguments) + "}}") +
         Event(R"({"type":"content_block_stop","index":0})") +
         Event(
             R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":8}})") +
         Event(R"({"type":"message_stop"})");
}

auto AnthropicFamilyTool(std::string_view id, std::string_view family,
                         std::string_view action, std::string_view arguments)
    -> std::string {
  return AnthropicTool(id, "tools",
                       "{\"family\":" + Quote(family) +
                           ",\"action\":" + Quote(action) +
                           ",\"arguments\":" + std::string(arguments) + "}");
}

auto ResponsesText(std::string_view text) -> std::string {
  return Event(
             "{\"type\":\"response.output_text.delta\",\"output_index\":0,"
             "\"delta\":" +
             Quote(text) + "}") +
         Event(
             "{\"type\":\"response.completed\",\"response\":{\"output\":[{"
             "\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
             "\"type\":\"output_text\",\"text\":" +
             Quote(text) +
             ",\"annotations\":[]}]}],\"usage\":{\"input_tokens\":12,\"output_"
             "tokens\":4}}}");
}

}  // namespace tsugumi::test
