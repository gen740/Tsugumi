#include "host/cache_process.hh"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "tsugumi/rpc/peer_tls.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/process.hh"

namespace tsugumi::host {
namespace {
constexpr int kReadyFd = 3;
constexpr int kLifetimeFd = 4;
constexpr auto kStartupTimeout = std::chrono::seconds(10);
constexpr auto kStopTimeout = std::chrono::seconds(2);

struct Fd {
  explicit Fd(int descriptor) : value(descriptor) {}
  ~Fd() {
    if (value >= 0) ::close(value);
  }
  Fd(const Fd&) = delete;
  auto operator=(const Fd&) -> Fd& = delete;
  int value;
};

auto OpenPipe(int (&ends)[2]) -> void {
#ifdef __linux__
  if (::pipe2(ends, O_CLOEXEC) != 0)
    throw std::runtime_error("Cannot open binary cache control pipe");
#else
  if (::pipe(ends) != 0)
    throw std::runtime_error("Cannot open binary cache control pipe");
  if (::fcntl(ends[0], F_SETFD, FD_CLOEXEC) == 0 &&
      ::fcntl(ends[1], F_SETFD, FD_CLOEXEC) == 0)
    return;
  ::close(ends[0]);
  ::close(ends[1]);
  throw std::runtime_error("Cannot protect binary cache control pipe");
#endif
}

auto ReadReady(int input) -> CacheProcess::Endpoint {
  auto until = std::chrono::steady_clock::now() + kStartupTimeout;
  std::string message;
  char bytes[256];
  while (message.size() < 1024) {
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(
        until - std::chrono::steady_clock::now());
    if (left.count() <= 0)
      throw std::runtime_error("Binary cache startup timed out");
    pollfd readable{input, POLLIN, 0};
    const auto result = ::poll(&readable, 1, static_cast<int>(left.count()));
    if (result < 0 && errno == EINTR) continue;
    if (result <= 0) throw std::runtime_error("Binary cache did not start");
    const auto n = ::read(input, bytes, sizeof bytes);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) throw std::runtime_error("Cannot read binary cache readiness");
    if (n == 0) break;
    message.append(bytes, static_cast<size_t>(n));
  }
  CacheProcess::Endpoint out;
  std::istringstream fields(message);
  std::string extra;
  if (!(fields >> out.port >> out.signing_key >> out.tls_key) ||
      fields >> extra)
    throw std::runtime_error("Invalid binary cache readiness");
  unsigned port = 0;
  const auto [end, error] =
      std::from_chars(out.port.data(), out.port.data() + out.port.size(), port);
  if (error != std::errc{} || end != out.port.data() + out.port.size() ||
      port == 0 || port > 65535)
    throw std::runtime_error("Invalid binary cache listener port");
  peer_tls::ValidateKey(out.tls_key);
  return out;
}
}  // namespace

CacheProcess::CacheProcess(std::filesystem::path state,
                           std::filesystem::path executable)
    : state_(std::move(state)), executable_(std::move(executable)) {}

CacheProcess::~CacheProcess() { Stop(); }

auto CacheProcess::UpdateKeys(const std::vector<std::string>& public_keys)
    -> void {
  if (public_keys == allowed_keys_ && pid_ >= 0) return;
  std::string allowlist;
  for (const auto& key : public_keys) {
    peer_tls::ValidateKey(key);
    allowlist += key + '\n';
  }
  fs::write_atomic(state_ / "authorized_keys", allowlist, 0600);
  allowed_keys_ = public_keys;
}

auto CacheProcess::Start(const std::vector<std::string>& public_keys)
    -> Endpoint {
  if (pid_ >= 0) {
    if (!proc::try_wait(pid_)) {
      UpdateKeys(public_keys);
      return endpoint_;
    }
    pid_ = -1;
    ::close(std::exchange(lifetime_, -1));
  }
  if (!executable_.is_absolute())
    throw std::runtime_error(
        "The built-in binary cache executable is unavailable");
  std::filesystem::create_directories(state_);
  if (::chmod(state_.c_str(), 0700) != 0)
    throw std::runtime_error("Cannot protect binary cache state");
  UpdateKeys(public_keys);
  int readiness[2], lifetime[2];
  OpenPipe(readiness);
  Fd ready_reader(readiness[0]), ready_writer(readiness[1]);
  OpenPipe(lifetime);
  Fd lifetime_reader(lifetime[0]), lifetime_writer(lifetime[1]);
  proc::Spec spec;
  spec.argv = {executable_.string(),
               "--state-dir",
               state_.string(),
               "--port",
               "0",
               "--ready-fd",
               std::to_string(kReadyFd),
               "--lifetime-fd",
               std::to_string(kLifetimeFd)};
  spec.fds = {{kReadyFd, ready_writer.value},
              {kLifetimeFd, lifetime_reader.value}};
  pid_ = proc::spawn(spec);
  lifetime_ = std::exchange(lifetime_writer.value, -1);
  ::close(std::exchange(ready_writer.value, -1));
  try {
    endpoint_ = ReadReady(ready_reader.value);
  } catch (...) {
    Stop();
    throw;
  }
  return endpoint_;
}

auto CacheProcess::Stop() -> void {
  if (lifetime_ >= 0) ::close(std::exchange(lifetime_, -1));
  if (pid_ < 0) return;
  const auto pid = std::exchange(pid_, -1);
  const auto until = std::chrono::steady_clock::now() + kStopTimeout;
  while (std::chrono::steady_clock::now() < until) {
    if (proc::try_wait(pid)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ::kill(pid, SIGKILL);
  proc::wait(pid);
}

}  // namespace tsugumi::host
