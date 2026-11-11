// External processes and HTTP peers used by the public Agent RPC scenarios.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tsugumi::test {

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  auto operator=(const ChildProcess&) -> ChildProcess& = delete;
  auto Start(const std::vector<std::string>& arguments,
             const std::filesystem::path& log) -> void;
  auto Stop(bool crash = false) -> void;
  auto Alive() -> bool;

 private:
  int pid_ = -1;
};

class TemporaryDirectory {
 public:
  TemporaryDirectory();
  ~TemporaryDirectory();
  std::filesystem::path path;
};

auto Write(const std::filesystem::path& path, std::string_view text) -> void;
auto Read(const std::filesystem::path& path) -> std::string;
auto Readiness(ChildProcess& process, const std::filesystem::path& path,
               const std::filesystem::path& log) -> std::string;

// No Agent or core implementation is mocked: this is an independent HTTP
// provider speaking the actual streaming wire format, with recorded requests.
class ModelServer {
 public:
  ModelServer();
  ~ModelServer();
  ModelServer(const ModelServer&) = delete;
  auto operator=(const ModelServer&) -> ModelServer& = delete;

  auto URL() const -> std::string;
  auto Queue(std::string stream, bool hold = false, bool fragment = false)
      -> void;
  auto QueueHTTP(std::string body, int status = 200) -> void;
  auto WaitForRequests(size_t count, std::chrono::milliseconds timeout =
                                         std::chrono::seconds(5)) -> bool;
  auto Requests() -> std::vector<std::string>;
  auto Headers() -> std::vector<std::string>;
  auto Release() -> void;

 private:
  struct Response {
    std::string stream;
    bool hold = false;
    bool fragment = false;
    int status = 200;
    std::string content_type = "text/event-stream";
  };
  auto Serve(std::stop_token stop) -> void;
  auto Handle(int socket, std::stop_token stop) -> void;
  int socket_ = -1;
  uint16_t port_ = 0;
  std::jthread thread_;
  std::mutex mutex_;
  std::condition_variable_any changed_;
  std::deque<Response> responses_;
  std::vector<std::string> requests_;
  std::vector<std::string> headers_;
  bool released_ = false;
};

auto AnthropicText(std::string_view text) -> std::string;
auto AnthropicTool(std::string_view id, std::string_view name,
                   std::string_view arguments) -> std::string;
auto ResponsesText(std::string_view text) -> std::string;
auto Quote(std::string_view text) -> std::string;
auto JsonString(std::string_view json, std::string_view pointer) -> std::string;
auto JsonBool(std::string_view json, std::string_view pointer) -> bool;
auto JsonUInt(std::string_view json, std::string_view pointer) -> uint64_t;

}  // namespace tsugumi::test
