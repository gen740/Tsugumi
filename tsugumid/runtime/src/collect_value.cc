#include "collect_value.hh"

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>

#include "tsugumi/protocol/card_generated.h"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi {

namespace {

namespace stdfs = std::filesystem;

constexpr size_t kMaxMessageBytes = size_t{16} << 20;
constexpr uint64_t kMaxFileBytes = uint64_t{64} << 30;

auto check_cancel(const std::atomic<bool>& cancel) -> void {
  if (cancel.load()) throw Failed(FailureKind::cancelled, "collect cancelled");
}

auto invalid_member(size_t index, const std::string& why) -> Failed {
  return Failed(FailureKind::output_invalid,
                "collect element " + std::to_string(index) + ": " + why);
}

auto as_string(const flatbuffers::FlatBufferBuilder& builder) -> std::string {
  return {reinterpret_cast<const char*>(builder.GetBufferPointer()),
          builder.GetSize()};
}

auto check_member(const types::PortType& source, const Value& member,
                  size_t index) -> void {
  if (member.kind != source.kind) throw invalid_member(index, "wrong kind");
}

// --- FlatBuffer members -----------------------------------------------------

// tsugumi.builtin.Collected { items:[Tsugumi.Card.CardPort] (id: 0); }: one
// envelope per member, named by its ordinal, with the member's bytes copied
// unopened.
auto collect_messages(const types::PortType& source,
                      std::span<const Value> members,
                      const std::atomic<bool>& cancel) -> std::string {
  size_t total = 0;
  for (size_t i = 0; i < members.size(); ++i) {
    check_cancel(cancel);
    check_member(source, members[i], i);
    total += members[i].bytes.size();
    if (total > kMaxMessageBytes)
      throw Failed(FailureKind::output_invalid,
                   "collected values exceed 16 MiB");
  }
  flatbuffers::FlatBufferBuilder builder(total + 1024);
  std::vector<flatbuffers::Offset<Tsugumi::Card::CardPort>> items;
  items.reserve(members.size());
  for (size_t i = 0; i < members.size(); ++i) {
    check_cancel(cancel);
    const auto& bytes = members[i].bytes;
    const auto name = builder.CreateString(std::to_string(i));
    // Each member is read in place by the next Card's SDK.
    builder.ForceVectorAlignment(bytes.size(), sizeof(uint8_t),
                                 FLATBUFFERS_MAX_ALIGNMENT);
    const auto data = builder.CreateVector(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    items.push_back(Tsugumi::Card::CreateCardPort(
        builder, name, PortKind::FlatBuffer, data));
    if (builder.GetSize() > kMaxMessageBytes)
      throw Failed(FailureKind::output_invalid,
                   "collected value exceeds 16 MiB");
  }
  const auto vector = builder.CreateVector(items);
  constexpr flatbuffers::voffset_t kItems = 4;  // field id 0
  const auto start = builder.StartTable();
  builder.AddOffset(kItems, vector);
  builder.Finish(
      flatbuffers::Offset<flatbuffers::Table>(builder.EndTable(start)));
  if (builder.GetSize() > kMaxMessageBytes)
    throw Failed(FailureKind::output_invalid, "collected value exceeds 16 MiB");
  return as_string(builder);
}

// --- Path members ------------------------------------------------------------

// Where a member lives: a trusted root (runs or inputs) and a path below it.
struct MemberSource {
  stdfs::path root;
  stdfs::path rel;
  std::string name;  // original basename
};

struct Budget {
  size_t files;
  size_t entries;
  uint64_t bytes = 0;
};

auto member_source(const Value& member, std::span<const stdfs::path> file_roots,
                   size_t index) -> MemberSource {
  if (member.path.empty()) throw invalid_member(index, "has no path");
  const stdfs::path path(member.path);
  if (!path.is_absolute() || path.lexically_normal() != path)
    throw invalid_member(index, "path must be absolute and normal");
  for (const auto& folder : file_roots) {
    const auto root = folder.lexically_normal();
    const auto rel = path.lexically_relative(root);
    // Rejects "..", ".", empty segments, backslashes and invalid UTF-8.
    if (rel.empty() || !fs::valid_relative_path(rel.generic_string())) continue;
    return {root, rel, rel.filename().string()};
  }
  throw invalid_member(index, "path is outside the run and input folders");
}

// A private folder beside the output, removed unless committed.
class Staging {
 public:
  explicit Staging(const stdfs::path& output_dir) {
    stdfs::create_directories(output_dir.parent_path());
    path_ =
        output_dir.parent_path() / ("." + output_dir.filename().string() +
                                    ".collect-tmp-" + crypto::random_token(8));
    // Only a folder this call created is ever removed.
    if (!stdfs::create_directory(path_))
      throw Failed(FailureKind::internal_error,
                   "collect staging folder already exists");
  }
  Staging(const Staging&) = delete;
  auto operator=(const Staging&) -> Staging& = delete;
  ~Staging() {
    if (committed_) return;
    std::error_code ec;
    stdfs::remove_all(path_, ec);
  }

  auto path() const -> const stdfs::path& { return path_; }

  auto commit(const stdfs::path& target) -> void {
    std::error_code ec;
    if (stdfs::symlink_status(target, ec).type() != stdfs::file_type::not_found)
      throw Failed(FailureKind::internal_error,
                   "collect output " + target.string() + " already exists");
    stdfs::rename(path_, target);
    committed_ = true;
    fs::fsync_dir(target.parent_path());
  }

 private:
  stdfs::path path_;
  bool committed_ = false;
};

auto copy_file(const stdfs::path& root, const stdfs::path& rel,
               const stdfs::path& destination, Budget& budget,
               const std::atomic<bool>& cancel) -> void {
  check_cancel(cancel);
  if (budget.files == 0)
    throw Failed(FailureKind::output_invalid, "collected files exceed 1,000");
  --budget.files;
  std::ofstream out(destination, std::ios::binary | std::ios::trunc);
  if (!out)
    throw Failed(FailureKind::internal_error,
                 "cannot create " + destination.string());
  const auto size = fs::stream_beneath(
      root, rel, kMaxFileBytes - budget.bytes, [&](std::string_view chunk) {
        check_cancel(cancel);
        out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        if (!out)
          throw Failed(FailureKind::internal_error,
                       "cannot write " + destination.string());
      });
  out.close();
  if (!out)
    throw Failed(FailureKind::internal_error,
                 "cannot write " + destination.string());
  budget.bytes += size;
}

auto copy_directory(const stdfs::path& root, const stdfs::path& rel,
                    const stdfs::path& destination, Budget& budget,
                    const std::atomic<bool>& cancel, size_t index) -> void {
  const auto listing = fs::list_beneath(root, rel, budget.entries);
  if (!listing.links.empty() || !listing.special.empty())
    throw invalid_member(index, "directory contains a symlink or special file");
  budget.entries -= listing.files.size() + listing.directories.size();
  stdfs::create_directory(destination);
  // Sorted, so every parent precedes its children.
  for (const auto& child : listing.directories) {
    check_cancel(cancel);
    if (!fs::valid_relative_path(child))
      throw invalid_member(index, "unsafe name " + child);
    stdfs::create_directory(destination / child);
  }
  for (const auto& child : listing.files) {
    if (!fs::valid_relative_path(child))
      throw invalid_member(index, "unsafe name " + child);
    copy_file(root, rel / child, destination / child, budget, cancel);
  }
}

// One directory with an ordinal folder per member: <output>/<i>/<basename>,
// each a regular file or a whole directory as found.
auto collect_files(const types::PortType& source,
                   std::span<const Value> members,
                   std::span<const stdfs::path> file_roots,
                   const stdfs::path& output_dir,
                   const std::atomic<bool>& cancel, CollectFileLimits limits)
    -> void {
  if (members.size() > limits.entries)
    throw Failed(FailureKind::output_invalid, "too many collected elements");
  std::error_code ec;
  if (stdfs::symlink_status(output_dir, ec).type() !=
      stdfs::file_type::not_found)
    throw Failed(FailureKind::internal_error,
                 "collect output " + output_dir.string() + " already exists");

  // Every member is validated before anything is written.
  std::vector<MemberSource> sources;
  sources.reserve(members.size());
  for (size_t i = 0; i < members.size(); ++i) {
    check_cancel(cancel);
    check_member(source, members[i], i);
    sources.push_back(member_source(members[i], file_roots, i));
  }

  try {
    Staging staging(output_dir);
    Budget budget{limits.files, limits.entries - members.size()};
    for (size_t i = 0; i < sources.size(); ++i) {
      check_cancel(cancel);
      const auto& member = sources[i];
      // One ordinal folder per element, so equal basenames never collide.
      const auto ordinal = std::to_string(i);
      stdfs::create_directory(staging.path() / ordinal);
      const auto destination = staging.path() / ordinal / member.name;
      try {
        if (fs::directory_beneath(member.root, member.rel))
          copy_directory(member.root, member.rel, destination, budget, cancel,
                         i);
        else
          copy_file(member.root, member.rel, destination, budget, cancel);
      } catch (const fs::Error& e) {
        throw invalid_member(i, e.what());
      }
    }
    check_cancel(cancel);
    staging.commit(output_dir);
  } catch (const stdfs::filesystem_error& e) {
    throw Failed(FailureKind::internal_error,
                 std::string("collect staging failed: ") + e.what());
  } catch (const fs::Error& e) {
    throw Failed(FailureKind::internal_error,
                 std::string("collect staging failed: ") + e.what());
  }
}

}  // namespace

auto collect_values(const types::PortType& source,
                    const types::PortType& output,
                    std::span<const Value> members,
                    std::span<const std::filesystem::path> file_roots,
                    const std::filesystem::path& output_dir,
                    const std::atomic<bool>& cancel, CollectFileLimits limits)
    -> Value {
  check_cancel(cancel);
  Value result;
  result.name = output.name;
  result.kind = output.kind;
  result.type = output.type;
  if (source.kind == PortKind::FlatBuffer) {
    result.bytes = collect_messages(source, members, cancel);
  } else {
    collect_files(source, members, file_roots, output_dir, cancel, limits);
    result.path = output_dir.string();
  }
  return result;
}

}  // namespace tsugumi
