#include "host/process_lock.hh"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <utility>

#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/log.hh"
#include "tsugumi/util/text.hh"

namespace tsugumi::host {
namespace {

constexpr size_t kRecordLimit = 4096;
constexpr std::string_view kDefaultApplication = "tsugumi-hostd";

auto Hostname() -> std::string {
  std::array<char, 256> name{};
  if (::gethostname(name.data(), name.size() - 1) != 0) return {};
  return name.data();
}

auto ValidApplication(std::string_view name) -> bool {
  return !name.empty() && name.size() <= 512 && text::valid_utf8(name) &&
         name.find_first_of("\r\n\t") == std::string_view::npos;
}

auto OpenRecord(const std::filesystem::path& path, int flags) -> int {
  const int fd =
      ::open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
  if (fd < 0) return -1;
  struct stat st{};
  if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
      st.st_uid == ::geteuid())
    return fd;
  ::close(fd);
  return -1;
}

auto ReadHolder(const std::filesystem::path& path)
    -> std::optional<LockHolder> {
  const int fd = OpenRecord(path, O_RDONLY);
  if (fd < 0) return std::nullopt;
  struct flock writer{};
  writer.l_type = F_WRLCK;
  writer.l_whence = SEEK_SET;
  std::array<char, kRecordLimit> buffer{};
  auto count = ssize_t{-1};
  if (::fcntl(fd, F_GETLK, &writer) == 0 && writer.l_type == F_WRLCK &&
      writer.l_pid > 1)
    count = ::pread(fd, buffer.data(), buffer.size(), 0);
  ::close(fd);
  if (count <= 0 || static_cast<size_t>(count) == buffer.size())
    return std::nullopt;
  const auto machine = Hostname();
  try {
    const auto record = json::Object(
        std::string_view(buffer.data(), static_cast<size_t>(count)));
    if (machine.empty() || json::Text(record, "hostname") != machine ||
        json::UInt(json::Get(record, "pid", "0")) !=
            static_cast<uint64_t>(writer.l_pid) ||
        (::kill(writer.l_pid, 0) != 0 && errno != EPERM))
      return std::nullopt;
    auto application = json::Text(record, "application");
    if (ValidApplication(application))
      return LockHolder{writer.l_pid, std::move(application)};
  } catch (const std::exception&) {
    // A missing, incomplete or malformed record cannot identify a holder.
  }
  return std::nullopt;
}

auto Publish(int fd, std::string_view application) -> bool {
  const auto record =
      json::Encode({{"pid", std::to_string(::getpid())},
                    {"application", json::Quote(ValidApplication(application)
                                                    ? application
                                                    : kDefaultApplication)},
                    {"hostname", json::Quote(Hostname())}});
  if (::ftruncate(fd, 0) != 0 ||
      ::pwrite(fd, record.data(), record.size(), 0) !=
          static_cast<ssize_t>(record.size()))
    return false;
  // Publish only after the record is complete. The lock disappears even if
  // the process is killed, and a copied record cannot carry it over.
  struct flock writer{};
  writer.l_type = F_WRLCK;
  writer.l_whence = SEEK_SET;
  return ::fcntl(fd, F_SETLK, &writer) == 0;
}

}  // namespace

auto LockHolder::Description() const -> std::string {
  return application + " (PID " + std::to_string(pid) + ")";
}

ProcessLock::ProcessLock(fs::FileLock lock) : lock_(std::move(lock)) {}

ProcessLock::ProcessLock(ProcessLock&& other) noexcept
    : lock_(std::move(other.lock_)),
      record_(std::exchange(other.record_, -1)) {}

ProcessLock::~ProcessLock() {
  if (record_ >= 0) ::close(record_);
}

auto ProcessLock::TryAcquire(const std::filesystem::path& path,
                             std::string_view application,
                             std::optional<LockHolder>& holder)
    -> std::optional<ProcessLock> {
  auto record_path = path;
  record_path += "-owner";
  auto lock = fs::FileLock::try_acquire(path);
  holder.reset();
  if (!lock) {
    holder = ReadHolder(record_path);
    return std::nullopt;
  }
  ProcessLock result(std::move(*lock));
  result.record_ = OpenRecord(record_path, O_RDWR | O_CREAT);
  if (result.record_ < 0 || !Publish(result.record_, application))
    log::warn("Cannot publish lock holder for " + path.string());
  return result;
}

}  // namespace tsugumi::host
