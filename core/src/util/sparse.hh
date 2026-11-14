// Sparse regular-file transfer shared by input staging and output capture.
#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace tsugumi::fs {

// The destination must be empty. on_chunk observes every logical byte,
// including zeros in holes, for hashing and cancellation checks.
inline auto copy_sparse_file(int source, int destination,
                      const std::filesystem::path& source_path,
                      const std::filesystem::path& destination_path,
                      const std::function<void(std::string_view)>& on_chunk)
    -> uint64_t {
  const auto failed = [](const char* operation, const std::filesystem::path& path, int error = errno) -> void {
    throw std::system_error(error, std::generic_category(),
                            std::string(operation) + " " + path.string());
  };
  struct stat status{};
  if (::fstat(source, &status) != 0) failed("fstat", source_path);
  if (!S_ISREG(status.st_mode) || status.st_size < 0)
    failed("expected regular file", source_path, EINVAL);
  const off_t size = status.st_size;
  std::vector<char> buffer(65536);
  const std::string zeros(buffer.size(), '\0');
  off_t offset = 0;
  bool seek_extents = true;
  while (offset < size) {
    off_t end = size;
#if defined(SEEK_DATA) && defined(SEEK_HOLE)
    if (seek_extents) {
      const off_t data = ::lseek(source, offset, SEEK_DATA);
      if (data < 0 && errno != ENXIO) {
        if (errno != EINVAL && errno != ENOTSUP && errno != ENOSYS)
          failed("SEEK_DATA", source_path);
        seek_extents = false;
      } else {
        const off_t next = data < 0 ? size : std::min(data, size);
        while (offset < next) {
          const auto count = static_cast<size_t>(std::min<off_t>(next - offset, zeros.size()));
          if (on_chunk) on_chunk(std::string_view(zeros.data(), count));
          offset += static_cast<off_t>(count);
        }
        if (offset == size) break;
        const off_t hole = ::lseek(source, offset, SEEK_HOLE);
        if (hole < 0) {
          if (errno != EINVAL && errno != ENOTSUP && errno != ENOSYS)
            failed("SEEK_HOLE", source_path);
          seek_extents = false;
        } else if (hole > offset) {
          end = std::min(hole, size);
        } else {
          seek_extents = false;
        }
      }
    }
#else
    (void)seek_extents;
#endif
    while (offset < end) {
      const auto requested = static_cast<size_t>(std::min<off_t>(end - offset, buffer.size()));
      const auto count = ::pread(source, buffer.data(), requested, offset);
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) failed("pread", source_path);
      if (count == 0) failed("source shrank during copy", source_path, EIO);
      const std::string_view chunk(buffer.data(), static_cast<size_t>(count));
      if (on_chunk) on_chunk(chunk);
      // Filesystems may not report extents, or may report allocated zero
      // blocks as data. Avoid allocating those blocks in either case.
      if (chunk.find_first_not_of('\0') != std::string_view::npos) {
        ssize_t written = 0;
        while (written < count) {
          const auto n = ::pwrite(destination, buffer.data() + written,
                                   static_cast<size_t>(count - written), offset + written);
          if (n < 0 && errno == EINTR) continue;
          if (n <= 0) failed("pwrite", destination_path, n == 0 ? EIO : errno);
          written += n;
        }
      }
      offset += count;
    }
  }
  if (::ftruncate(destination, size) != 0) failed("ftruncate", destination_path);
  return static_cast<uint64_t>(size);
}

}  // namespace tsugumi::fs
