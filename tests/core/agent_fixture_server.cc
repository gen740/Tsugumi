#include "agent_fixture_server.hh"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
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
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (Alive() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
  std::string pattern = "/tmp/tsugumi-agent-XXXXXX";
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

auto Readiness(ChildProcess& process, const std::filesystem::path& path,
               const std::filesystem::path& log) -> std::string {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    if (std::filesystem::is_regular_file(path)) return Read(path);
    if (!process.Alive())
      throw std::runtime_error("daemon exited during startup:\n" + Read(log));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error("daemon did not become ready:\n" + Read(log));
}

ModelServer::ModelServer() {
  socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket_ < 0)
    throw std::runtime_error("cannot create model fixture socket");
  ::fcntl(socket_, F_SETFD, FD_CLOEXEC);
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
  ::shutdown(socket_, SHUT_RDWR);
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
  while (!stop.stop_requested()) {
    pollfd readable{socket_, POLLIN, 0};
    if (::poll(&readable, 1, 100) <= 0) continue;
    const auto socket = ::accept(socket_, nullptr, nullptr);
    if (socket < 0) continue;
    ::fcntl(socket, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int yes = 1;
    ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
    try {
      Handle(socket, stop);
    } catch (...) {
    }
    ::close(socket);
  }
}

auto ModelServer::Handle(int socket, std::stop_token stop) -> void {
  std::string request;
  size_t body_start = std::string::npos;
  size_t body_size = 0;
  while (!stop.stop_requested()) {
    pollfd readable{socket, POLLIN, 0};
    if (::poll(&readable, 1, 100) <= 0) continue;
    char bytes[8192];
    const auto count = ::recv(socket, bytes, sizeof bytes, 0);
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

auto AnthropicText(std::string_view text) -> std::string {
  return Event(
             R"({"type":"message_start","message":{"usage":{"input_tokens":12}}})") +
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
