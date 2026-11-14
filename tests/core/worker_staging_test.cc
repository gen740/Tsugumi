// Socket/Nix-free harness over the production cleaner and output copier.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include "util/staging.hh"
#include "util/sparse.hh"

namespace {
namespace stdfs = std::filesystem;

auto expect(bool condition, const char* message) -> void {
  if (!condition) throw std::runtime_error(message);
}

// The harness has no Card process exchanging unreadable entries. Its recovery
// callback may chmod a verified directory directly; production uses a helper
// constrained to the Card's identity and sandbox instead.
auto clear_staging(const stdfs::path& path) -> void {
  tsugumi::fs::clear_staging(path, [](int parent, const char* name) {
    struct stat status{};
    expect(::fstatat(parent, name, &status, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(status.st_mode),
           "recovery target is not a directory");
    expect(::fchmodat(parent, name, status.st_mode | S_IRWXU, AT_SYMLINK_NOFOLLOW) == 0,
           "recover test directory permissions");
  });
}

class Scratch {
 public:
  Scratch() {
    auto pattern =
        (stdfs::temp_directory_path() / "tsugumi-staging-XXXXXX").string();
    const auto* made = ::mkdtemp(pattern.data());
    if (!made) throw std::runtime_error("mkdtemp failed");
    path = made;
  }
  ~Scratch() {
    try {
      clear_staging(path);
      stdfs::remove(path);
    } catch (...) {
    }
  }
  stdfs::path path;
};

auto clean_card_entries(const stdfs::path& root) -> void {
  const auto staging = root / "staging";
  const auto outside = root / "outside";
  stdfs::create_directories(staging / "notebook/source/nested");
  stdfs::create_directory(outside);
  std::ofstream(outside / "sentinel") << "must survive";
  std::ofstream(staging / "notebook/source/nested/notebook.py") << "Nix source";
  std::ofstream(staging / ".DS_Store") << "hidden entry";
  expect(::chmod((outside / "sentinel").c_str(), 0444) == 0, "chmod file");
  expect(::chmod(outside.c_str(), 0555) == 0, "chmod outside");
  expect(::link((outside / "sentinel").c_str(),
                (staging / "hardlink").c_str()) == 0,
         "hardlink");
  stdfs::create_directory_symlink(outside, staging / "directory-link");
  stdfs::create_symlink(outside / "sentinel", staging / "file-link");
  stdfs::create_symlink(root / "missing", staging / "dangling-link");
  expect(::mkfifo((staging / "fifo").c_str(), 0600) == 0, "mkfifo");
  const auto source = staging / "notebook/source";
  expect(::chmod((source / "nested/notebook.py").c_str(), 0444) == 0,
         "chmod source file");
  expect(::chmod((source / "nested").c_str(), 0500) == 0, "chmod nested");
  expect(::chmod(source.c_str(), 0555) == 0, "chmod source");

  // An open directory and a thread still writing an already-open file must not
  // block removal. The writer never creates new entries during the walk.
  const int directory = ::open(source.c_str(), O_RDONLY | O_DIRECTORY);
  expect(directory >= 0, "open source directory");
  const int writing =
      ::open((staging / "writing").c_str(), O_WRONLY | O_CREAT, 0600);
  expect(writing >= 0, "open writer");
  std::atomic<bool> started = false;
  std::atomic<bool> write_failed = false;
  {
    std::jthread writer([&](std::stop_token stop) {
      do {
        if (::pwrite(writing, "x", 1, 0) != 1) write_failed = true;
        started = true;
        std::this_thread::yield();
      } while (!stop.stop_requested());
    });
    while (!started.load()) std::this_thread::yield();
    clear_staging(staging);
    expect(stdfs::is_empty(staging), "staging still has entries");
    expect(::pwrite(writing, "y", 1, 0) == 1,
           "open file stopped being writable");
  }
  ::close(writing);
  ::close(directory);
  expect(!write_failed.load(), "concurrent writer failed");
  expect(stdfs::exists(outside / "sentinel"), "followed a symlink");
  struct stat status{};
  expect(
      ::stat(outside.c_str(), &status) == 0 && (status.st_mode & 0777) == 0555,
      "changed symlink target directory permissions");
  expect(::stat((outside / "sentinel").c_str(), &status) == 0 &&
             (status.st_mode & 0777) == 0444,
         "changed hardlink target file permissions");

  // The fixed staging root is also Card-writable: preserve its inode and make
  // it usable for another invocation if the Card removed its write bit.
  expect(::stat(staging.c_str(), &status) == 0, "stat staging");
  const auto inode = status.st_ino;
  std::ofstream(staging / "next") << "second invocation";
  expect(::chmod(staging.c_str(), 0555) == 0, "chmod staging");
  clear_staging(staging);
  expect(stdfs::is_empty(staging), "second invocation was not cleared");
  expect(::stat(staging.c_str(), &status) == 0 && status.st_ino == inode,
         "replaced the fixed staging directory");
}

auto clean_unreadable_directories(const stdfs::path& root) -> void {
  const auto staging = root / "unreadable";
  stdfs::create_directories(staging / "private/nested");
  std::ofstream(staging / "private/nested/data") << "private";
  expect(::chmod((staging / "private/nested").c_str(), 0000) == 0, "chmod nested 000");
  expect(::chmod((staging / "private").c_str(), 0000) == 0, "chmod private 000");
  expect(::chmod(staging.c_str(), 0000) == 0, "chmod root 000");
  clear_staging(staging);
  expect(stdfs::is_empty(staging), "mode-000 staging was not cleared");
}

// Both staging directions use this copier. Exercise a multi-GiB sparse file
// with actual data at separated offsets and a trailing hole, without ever
// materializing its contents in the test process.
auto copy_sparse_outputs(const stdfs::path& root) -> void {
  const auto staging = root / "sparse";
  stdfs::create_directory(staging);
  const auto source = staging / "large";
  constexpr off_t size = off_t{4} * 1024 * 1024 * 1024;
  const int file = ::open(source.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  expect(file >= 0, "open sparse source");
  expect(::ftruncate(file, size) == 0, "size sparse source");
  for (const off_t offset : {off_t{0}, size / 2, size - 131072})
    expect(::pwrite(file, "data", 4, offset) == 4, "write sparse data");
  const int directory = ::open(staging.c_str(), O_RDONLY | O_DIRECTORY);
  expect(directory >= 0, "open sparse staging");
  std::atomic<bool> cancel = false;
  const auto output = root / "sparse-output";
  tsugumi::fs::copy_staging_output(directory, "large", output, false, cancel);
  ::close(directory);
  const auto check = [&](const stdfs::path& path) {
    struct stat status{};
    expect(::stat(path.c_str(), &status) == 0 && status.st_size == size, "sparse copy size");
    expect(status.st_blocks * 512 < 1024 * 1024, "sparse copy allocated its holes");
    const int fd = ::open(path.c_str(), O_RDONLY);
    expect(fd >= 0, "open sparse copy");
    for (const off_t offset : {off_t{0}, size / 2, size - 131072}) {
      char data[4];
      expect(::pread(fd, data, sizeof data, offset) == 4 && std::string_view(data, 4) == "data",
             "sparse data changed");
    }
    for (const off_t offset : {off_t{65536}, size / 2 - 4, size - 4}) {
      char data[4];
      expect(::pread(fd, data, sizeof data, offset) == 4 &&
                 std::string_view(data, 4).find_first_not_of('\0') == std::string_view::npos,
             "sparse hole changed");
    }
    ::close(fd);
  };
  check(output);
  // Input transfers additionally consume every logical byte for hashing.
  const auto input = root / "sparse-input";
  const int destination = ::open(input.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  expect(destination >= 0, "open sparse input");
  uint64_t observed = 0;
  const auto copied = tsugumi::fs::copy_sparse_file(file, destination, source, input,
      [&](std::string_view bytes) { observed += bytes.size(); });
  expect(copied == static_cast<uint64_t>(size) && observed == copied, "sparse callback lost logical bytes");
  ::close(destination);
  ::close(file);
  check(input);

  // A filesystem may report all allocated zero blocks as data. The block
  // detector must avoid allocating them even when extent seeking skips none.
  {
    std::ofstream dense(staging / "allocated-zeros", std::ios::binary);
    const std::string zeros(65536, '\0');
    for (int i = 0; i < 64; ++i) dense.write(zeros.data(), zeros.size());
  }
  const int zeros_root = ::open(staging.c_str(), O_RDONLY | O_DIRECTORY);
  expect(zeros_root >= 0, "open zero-block staging");
  const auto zeros_copy = root / "zero-block-copy";
  tsugumi::fs::copy_staging_output(zeros_root, "allocated-zeros", zeros_copy, false, cancel);
  ::close(zeros_root);
  struct stat zeros_status{};
  expect(::stat(zeros_copy.c_str(), &zeros_status) == 0 && zeros_status.st_size == 4 * 1024 * 1024,
         "zero-block copy size");
  expect(zeros_status.st_blocks * 512 < 1024 * 1024, "allocated zero blocks were copied densely");

  cancel = true;
  const int fd = ::open(staging.c_str(), O_RDONLY | O_DIRECTORY);
  bool cancelled = false;
  try {
    tsugumi::fs::copy_staging_output(fd, "large", root / "sparse-cancelled", false, cancel);
  } catch (const std::runtime_error&) {
    cancelled = true;
  }
  ::close(fd);
  expect(cancelled, "sparse transfer ignored cancellation");
}

auto failure_details(const stdfs::path& root) -> void {
  // The root itself must never be followed if a Card replaces it with a link.
  const auto link = root / "replaced";
  stdfs::create_directory_symlink(root / "outside", link);
  try {
    clear_staging(link);
  } catch (const std::system_error& error) {
    const std::string message = error.what();
    expect(message.find(link.string()) != std::string::npos,
           "error lacks path");
    expect(message.find("errno " + std::to_string(error.code().value())) !=
               std::string::npos,
           "error lacks errno");
    expect(error.code().value() == ENOTDIR || error.code().value() == ELOOP,
           "unexpected symlink error");
    return;
  }
  throw std::runtime_error("followed a replaced staging root");
}
auto read_file(const stdfs::path& path) -> std::string {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

auto copy_outputs(const stdfs::path& root) -> void {
  const auto staging = root / "outputs";
  const auto report = staging / "report";
  stdfs::create_directories(report / "assets");
  std::ofstream(report / "assets/data") << "inside";
  std::ofstream(staging / "secret") << "outside-output";
  stdfs::create_symlink("assets/data", report / "file-link");
  stdfs::create_directory_symlink("assets", report / "directory-link");
  stdfs::create_symlink("file-link", report / "chain");
  stdfs::create_symlink("../file-link", report / "assets/up-link");
  // Resolution must use the physical parent of an aliased directory.
  stdfs::create_directories(report / "nested/inner");
  std::ofstream(report / "nested/data") << "physical-parent";
  stdfs::create_symlink("../data", report / "nested/inner/up");
  stdfs::create_directory_symlink("nested/inner", report / "alias");
  // These are not declared outputs and must never be walked.
  stdfs::create_symlink("/etc/passwd", staging / "unused-link");
  expect(::mkfifo((staging / "unused-fifo").c_str(), 0600) == 0,
         "mkfifo unused");
  const int fd = ::open(staging.c_str(), O_RDONLY | O_DIRECTORY);
  expect(fd >= 0, "open output staging");
  const std::atomic<bool> cancel = false;
  const auto destination = root / "saved-report";
  tsugumi::fs::copy_staging_output(fd, "report", destination, true, cancel);
  for (const auto& name :
       {"file-link", "directory-link/data", "chain", "assets/up-link"})
    expect(read_file(destination / name) == "inside",
           "internal link lost its contents");
  expect(read_file(destination / "alias/up") == "physical-parent",
         "resolved from logical rather than physical parent");
  for (const auto& entry : stdfs::recursive_directory_iterator(destination))
    expect(!entry.is_symlink(), "copy retained a source symlink");
  tsugumi::fs::copy_staging_output(fd, "report/assets/data",
                                   destination / "direct", false, cancel);
  expect(read_file(destination / "direct") == "inside", "declared File copy");

  const auto bad = report / "bad";
  const auto refuse = [&](const std::string& kind) {
    bool rejected = false;
    try {
      tsugumi::fs::copy_staging_output(fd, "report", root / "rejected", true,
                                       cancel);
    } catch (const std::exception& error) {
      const std::string message = error.what();
      expect(message.find("report/bad") != std::string::npos,
             "refusal lacks entry path");
      expect(message.find(kind) != std::string::npos,
             "refusal lacks entry kind");
      rejected = true;
    }
    expect(rejected, "unsafe output was accepted");
    stdfs::remove(bad);
  };
  stdfs::create_symlink("../secret", bad);
  refuse("symbolic link");
  stdfs::create_symlink(staging / "secret", bad);
  refuse("symbolic link");
  // An innocent-looking chain must not allow an escape through a later link.
  stdfs::create_symlink("../../secret", report / "assets/escape");
  stdfs::create_symlink("assets/escape", bad);
  try {
    tsugumi::fs::copy_staging_output(fd, "report", root / "chain-rejected",
                                     true, cancel);
    throw std::logic_error("accepted escaping chain");
  } catch (const std::runtime_error& error) {
    expect(std::string(error.what()).find("symbolic link") != std::string::npos,
           "chain rejection kind");
  }
  stdfs::remove(bad);
  stdfs::remove(report / "assets/escape");
  stdfs::create_symlink("bad", bad);
  refuse("symbolic link");
  stdfs::create_directory_symlink(".", bad);
  refuse("directory symbolic link");
  expect(::mkfifo(bad.c_str(), 0600) == 0, "mkfifo output");
  refuse("FIFO");
  ::close(fd);
}

auto copy_link_race(const stdfs::path& root) -> void {
  const auto staging = root / "race";
  const auto report = staging / "report";
  stdfs::create_directories(report);
  std::ofstream(report / "safe") << "inside";
  const auto secret = root / "host-secret";
  std::ofstream(secret) << "host-secret";
  const auto link = report / "link";
  const auto next = staging / "next";
  stdfs::create_symlink("safe", link);
  const int fd = ::open(staging.c_str(), O_RDONLY | O_DIRECTORY);
  expect(fd >= 0, "open racing output");
  const std::atomic<bool> cancel = false;
  std::atomic<bool> started = false;
  std::atomic<bool> swap_failed = false;
  {
    std::jthread swapper([&](std::stop_token stop) {
      while (!stop.stop_requested()) {
        for (const auto& target : {secret.string(), std::string("safe")}) {
          ::unlink(next.c_str());
          if (::symlink(target.c_str(), next.c_str()) != 0 ||
              ::rename(next.c_str(), link.c_str()) != 0) {
            swap_failed = true;
            started = true;
            return;
          }
          started = true;
          std::this_thread::yield();
        }
      }
    });
    while (!started.load()) std::this_thread::yield();
    for (int i = 0; i < 100; ++i) {
      const auto destination = root / ("race-copy-" + std::to_string(i));
      try {
        tsugumi::fs::copy_staging_output(fd, "report", destination, true,
                                         cancel);
      } catch (const std::runtime_error& error) {
        expect(
            std::string(error.what()).find("report/link") != std::string::npos,
            "race error lacks path");
      }
      if (stdfs::exists(destination / "link"))
        expect(read_file(destination / "link") == "inside",
               "race read a host file");
      for (const auto& entry : stdfs::recursive_directory_iterator(destination))
        expect(!entry.is_symlink(), "race created a destination symlink");
    }
  }
  ::close(fd);
  expect(!swap_failed.load(), "link swapper failed");
  expect(read_file(secret) == "host-secret", "race changed host file");
}

}  // namespace

auto main() -> int {
  try {
    Scratch scratch;
    clean_card_entries(scratch.path);
    clean_unreadable_directories(scratch.path);
    copy_sparse_outputs(scratch.path);
    failure_details(scratch.path);
    copy_outputs(scratch.path);
    copy_link_race(scratch.path);
    std::cout << "worker staging tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
