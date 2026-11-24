// Production input/output copiers, without a worker process or Nix build.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/workerd/staging.hh"

namespace {
namespace fs = std::filesystem;

auto expect(bool condition, const char* message) -> void {
  if (!condition) throw std::runtime_error(message);
}

class Scratch {
 public:
  Scratch() {
    auto pattern =
        (fs::temp_directory_path() / "tsugumi-transfer-XXXXXX").string();
    const auto* made = ::mkdtemp(pattern.data());
    if (!made) throw std::runtime_error("mkdtemp failed");
    path = made;
  }
  ~Scratch() {
    // Installed artifacts are sealed read-only.
    try {
      tsugumi::fs::clear_staging(path);
      fs::remove(path);
    } catch (...) {
    }
  }
  fs::path path;
};

auto large_file(const fs::path& root, bool sparse) -> void {
  const auto output = root / "worker-out";
  fs::create_directories(output / "artifact");
  const auto source = output / "artifact/data";
  const uint64_t size = sparse ? (uint64_t{2} << 30) + 131 : 3 * 65536 + 17;
  {
    std::ofstream file(source, std::ios::binary);
    if (sparse) {
      file.seekp(static_cast<std::streamoff>(size - 4));
      file.write("tail", 4);
    } else {
      std::string block(65536, '\0');
      for (size_t i = 0; i < block.size(); ++i)
        block[i] = static_cast<char>(i % 251);
      for (uint64_t offset = 0; offset < size; offset += block.size()) {
        file.write(block.data(),
                   static_cast<std::streamsize>(
                       std::min<uint64_t>(block.size(), size - offset)));
      }
    }
    expect(static_cast<bool>(file), "write large file");
  }
  // Linux moves the staged file into the store.
  const auto expected = sparse ? std::string() : tsugumi::fs::sha256_file(source);
  const auto check = [&](const fs::path& path) {
    expect(fs::file_size(path) == size, "staging truncated a large file");
    if (sparse) {
      struct stat status{};
      expect(::stat(path.c_str(), &status) == 0 &&
                 status.st_blocks * 512 < 1024 * 1024,
             "sparse copy allocated its holes");
      std::ifstream file(path, std::ios::binary);
      file.seekg(static_cast<std::streamoff>(size - 4));
      char tail[4];
      file.read(tail, sizeof tail);
      expect(file.gcount() == 4 && std::string_view(tail, 4) == "tail",
             "lost data beyond 2 GiB");
    } else {
      expect(tsugumi::fs::sha256_file(path) == expected,
             "staging changed file content");
    }
  };
  const int directory = ::open(output.c_str(), O_RDONLY | O_DIRECTORY);
  expect(directory >= 0, "open worker output");
  const auto saved = root / "saved";
  tsugumi::fs::OutputBudget budget;
#ifdef __APPLE__
  // macOS installs a clone of the staged tree.
  const auto clones = root / "clones";
  fs::create_directory(clones);
  tsugumi::fs::clone_staging_artifact(directory, "artifact", clones / "artifact",
                                      budget);
  ::close(directory);
  const int cloned = ::open(clones.c_str(), O_RDONLY | O_DIRECTORY);
  expect(cloned >= 0, "open clones");
  tsugumi::fs::move_staging_artifact(cloned, "artifact", saved, budget);
  ::close(cloned);
#else
  tsugumi::fs::move_staging_artifact(directory, "artifact", saved, budget);
  ::close(directory);
#endif
  check(saved / "data");
  // The next Card can copy the forwarded artifact member as input.
  const auto next = root / "next-input";
  expect(tsugumi::fs::copy_beneath(root, "saved/data", next, {}) == size,
         "input copy count");
  check(next);
}

auto shrinking_source(const fs::path& root, bool sparse) -> void {
  const auto source = root / "changing";
  const int file = ::open(source.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  expect(file >= 0, "open changing file");
  if (sparse) {
    expect(::ftruncate(file, 47040016) == 0, "size changing sparse file");
  } else {
    const std::string data(131072, 'x');
    expect(::write(file, data.data(), data.size()) ==
               static_cast<ssize_t>(data.size()),
           "write changing file");
  }
  bool changed = false;
  const auto shrink = [&](std::string_view) {
    if (!changed) {
      expect(::ftruncate(file, 0) == 0, "truncate source during transfer");
      changed = true;
    }
  };
  bool rejected = false;
  try {
    if (sparse)
      tsugumi::fs::copy_beneath(root, "changing", root / "failed-copy", shrink);
    else
      tsugumi::fs::stream_beneath(root, "changing", UINT64_MAX, shrink);
  } catch (const std::exception&) {
    rejected = true;
  }
  ::close(file);
  expect(changed && rejected, "source truncation silently succeeded");
  if (sparse)
    expect(!fs::exists(root / "failed-copy"), "published an incomplete copy");
}
}  // namespace

auto main(int argc, char** argv) -> int {
  try {
    expect(argc == 2, "expected large, sparse, shrink-stream or shrink-sparse");
    Scratch scratch;
    const std::string test = argv[1];
    if (test == "large" || test == "sparse")
      large_file(scratch.path, test == "sparse");
    else if (test == "shrink-stream" || test == "shrink-sparse")
      shrinking_source(scratch.path, test == "shrink-sparse");
    else
      throw std::runtime_error("unknown test");
    std::cout << test << " passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
