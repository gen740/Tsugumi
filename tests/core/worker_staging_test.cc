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

namespace {
namespace stdfs = std::filesystem;

auto expect(bool condition, const char* message) -> void {
  if (!condition) throw std::runtime_error(message);
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
      tsugumi::fs::clear_staging(path);
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
    tsugumi::fs::clear_staging(staging);
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
  tsugumi::fs::clear_staging(staging);
  expect(stdfs::is_empty(staging), "second invocation was not cleared");
  expect(::stat(staging.c_str(), &status) == 0 && status.st_ino == inode,
         "replaced the fixed staging directory");
}

auto failure_details(const stdfs::path& root) -> void {
  // The root itself must never be followed if a Card replaces it with a link.
  const auto link = root / "replaced";
  stdfs::create_directory_symlink(root / "outside", link);
  try {
    tsugumi::fs::clear_staging(link);
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
