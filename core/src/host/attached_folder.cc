#include "host/attached_folder.hh"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <memory>
#include <vector>

#include "util/crypto.hh"
#include "util/fs.hh"
#include "util/text.hh"

namespace tsugumi::host {
namespace {
namespace json = agent::json;
using Failure = FolderReadFailure;
struct Descriptor {
  int value;
  ~Descriptor() {
    if (value >= 0) ::close(value);
  }
};
auto Number(const json::Members& args, const char* key, uint64_t fallback)
    -> uint64_t {
  const auto raw = json::Get(args, key, "null");
  if (raw == "null") return fallback;
  const auto value = raw.starts_with('"') ? json::String(raw) : raw;
  uint64_t number = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), number);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
    throw Failure("invalid_request",
                  std::string(key) + " must be an unsigned integer");
  return number;
}
}  // namespace

auto ReadAttachedFolder(const std::filesystem::path& root,
                        const json::Members& arguments) -> std::string {
  if (!root.is_absolute() || root.string().find('\0') != std::string::npos)
    throw Failure("invalid_path", "Attached folder must be an absolute path");
  const auto path = json::Text(arguments, "path");
  if (!path.empty() && !fs::valid_relative_path(path))
    throw Failure("invalid_path", "Invalid attached-folder relative path");
  const auto offset = Number(arguments, "offset", 0);
  const auto limit = Number(arguments, "limit_bytes", 262144);
  if (!limit || limit > (1u << 20))
    throw Failure("invalid_request",
                  "limit_bytes must be between 1 and 1048576");
  Descriptor file{
      ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (file.value < 0)
    throw Failure("not_found", "Attached folder is unavailable");
  for (const auto& component : std::filesystem::path(path)) {
    const int next = ::openat(file.value, component.c_str(),
                              O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (next < 0)
      throw Failure("permission_denied",
                    "Cannot read attached path without following links");
    ::close(file.value);
    file.value = next;
  }
  struct stat status{};
  if (::fstat(file.value, &status) != 0)
    throw Failure("not_found", "Cannot inspect attached path");
  std::string bytes;
  if (S_ISDIR(status.st_mode)) {
    struct CloseDirectory {
      auto operator()(DIR* value) const -> void { ::closedir(value); }
    };
    std::unique_ptr<DIR, CloseDirectory> directory(
        ::fdopendir(::dup(file.value)));
    if (!directory)
      throw Failure("not_found", "Cannot list attached directory");
    std::vector<std::string> names;
    while (const auto* entry = ::readdir(directory.get())) {
      const std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      if (names.size() == 5000)
        throw Failure("limit_exceeded",
                      "Attached directory exceeds 5000 entries");
      struct stat child{};
      const bool directory_entry = ::fstatat(file.value, name.c_str(), &child,
                                             AT_SYMLINK_NOFOLLOW) == 0 &&
                                   S_ISDIR(child.st_mode);
      names.push_back(name + (directory_entry ? "/" : ""));
    }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) bytes += name + "\n";
  } else if (S_ISREG(status.st_mode)) {
    if (status.st_size < 0)
      throw Failure("invalid_path", "Invalid attached file size");
    const auto size = static_cast<uint64_t>(status.st_size);
    const auto begin = std::min(offset, size);
    bytes.resize(std::min(limit, size - begin));
    size_t position = 0;
    while (position < bytes.size()) {
      const auto count = ::pread(file.value, bytes.data() + position,
                                  bytes.size() - position, begin + position);
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) throw Failure("not_found", "Cannot read attached file");
      if (!count) break;
      position += static_cast<size_t>(count);
    }
    bytes.resize(position);
  } else {
    throw Failure("invalid_path", "Attached path must be a file or directory");
  }
  const auto size = S_ISREG(status.st_mode)
                        ? static_cast<uint64_t>(status.st_size) : bytes.size();
  const auto begin = std::min<uint64_t>(offset, size);
  const auto page = S_ISREG(status.st_mode) ? bytes : bytes.substr(begin, limit);
  const auto encoded =
      text::valid_utf8(page) && page.find('\0') == std::string::npos
          ? json::Quote(page)
          : json::Encode(
                json::Members{{"base64", json::Quote(crypto::base64(page))}});
  return json::Encode(json::Members{
      {"reply", json::Quote("FileContent")},
      {"value",
       json::Encode(json::Members{
           {"name",
            json::Quote(path.empty()
                            ? root.filename().string()
                            : std::filesystem::path(path).filename().string())},
           {"bytes", encoded},
           {"shell_path", json::Quote("attached/" + crypto::sha256_hex(root.string()).substr(0, 16) + "/")},
           {"offset", json::Quote(std::to_string(offset))},
           {"size_bytes", json::Quote(std::to_string(size))},
           {"next_offset", json::Quote(std::to_string(begin + page.size()))},
           {"eof", begin + page.size() == size ? "true" : "false"},
           {"hint", json::Quote("Use project_copy to move this file without reading it; use project_shell to examine large files.")}})}});
}
}  // namespace tsugumi::host
