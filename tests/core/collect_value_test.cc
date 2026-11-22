#include "collect_value.hh"

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "tsugumi/protocol/card_generated.h"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi {
namespace {

namespace stdfs = std::filesystem;

// Field voffsets follow the declared ids: 4 + 2 * id.
constexpr flatbuffers::voffset_t kSlot0 = 4;
constexpr flatbuffers::voffset_t kSlot1 = 6;
constexpr flatbuffers::voffset_t kSlot2 = 8;

constexpr uint64_t kNanBits = 0x7ff8'0000'0000'1234ULL;  // a NaN with a payload

auto compile_type(const char* text, const char* root) -> types::TypeRef {
  std::string error;
  auto type = types::compile(text, root, &error);
  EXPECT_TRUE(type) << error;
  return type ? *type : nullptr;
}

auto message_type() -> types::TypeRef {
  return compile_type(R"(namespace test;
table Inner { big:ulong (id: 0); real:double (id: 1); maybe:int = null (id: 2); }
table Message { label:string (id: 0); inner:Inner (id: 1); }
)",
                      "test.Message");
}

auto finish(flatbuffers::FlatBufferBuilder& b, flatbuffers::uoffset_t root)
    -> std::string {
  b.Finish(flatbuffers::Offset<flatbuffers::Table>(root));
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}

auto message(const types::TypeRef& type, const std::string& label, uint64_t big,
             double real, std::optional<int32_t> maybe) -> Value {
  flatbuffers::FlatBufferBuilder b;
  b.ForceDefaults(true);
  const auto inner_start = b.StartTable();
  b.AddElement<uint64_t>(kSlot0, big, 0);
  b.AddElement<double>(kSlot1, real, 0.0);
  if (maybe) b.AddElement<int32_t>(kSlot2, *maybe, 0);
  const auto inner =
      flatbuffers::Offset<flatbuffers::Table>(b.EndTable(inner_start));
  const auto text = b.CreateString(label);
  const auto start = b.StartTable();
  b.AddOffset(kSlot0, text);
  b.AddOffset(kSlot1, inner);
  return Value{"in", PortKind::FlatBuffer, type, finish(b, b.EndTable(start)),
               std::nullopt};
}

auto path_member(const stdfs::path& path) -> Value {
  Value value{"in", PortKind::Path, nullptr, {}, std::nullopt};
  value.path = path.string();
  return value;
}

auto root_of(const Value& v) -> const flatbuffers::Table* {
  return flatbuffers::GetRoot<flatbuffers::Table>(v.bytes.data());
}

using Envelopes =
    flatbuffers::Vector<flatbuffers::Offset<Tsugumi::Card::CardPort>>;

// The items of tsugumi.builtin.Collected: one CardPort per member.
auto envelopes(const Value& v) -> const Envelopes* {
  return root_of(v)->GetPointer<const Envelopes*>(kSlot0);
}

auto bytes_of(const Tsugumi::Card::CardPort* port) -> std::string {
  return port->bytes() ? std::string(reinterpret_cast<const char*>(
                                         port->bytes()->data()),
                                     port->bytes()->size())
                       : "";
}

auto write_text(const stdfs::path& path, const std::string& text) -> void {
  stdfs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

auto read_text(const stdfs::path& path) -> std::string {
  return fs::read_file(path).value_or("<missing>");
}

// A fresh state folder with runs/ and inputs/, removed afterwards.
class CollectFiles : public ::testing::Test {
 protected:
  void SetUp() override {
    state = stdfs::temp_directory_path() /
            ("tsugumi-collect-test-" + crypto::random_token(8));
    stdfs::create_directories(state / "runs");
    stdfs::create_directories(state / "inputs");
    stdfs::create_directories(state / "invocation");
    output_dir = state / "invocation" / "out";
  }
  void TearDown() override {
    std::error_code ec;
    stdfs::remove_all(state, ec);
  }

  auto port(const char* name, PortKind kind, types::TypeRef type)
      -> types::PortType {
    return {name, kind, false, std::move(type)};
  }

  auto collect(const std::vector<Value>& members,
               CollectFileLimits limits = {}) -> Value {
    const auto source = port("in", PortKind::Path, nullptr);
    std::string error;
    const auto type = collect_output_type(source, error);
    EXPECT_TRUE(type) << error;
    return collect_values(source, port("out", PortKind::Path, type), members,
                          std::vector{state / "runs", state / "inputs"},
                          output_dir, cancel, limits);
  }

  // Nothing but the original invocation folder content (no staging left over).
  auto invocation_is_clean() -> bool {
    return stdfs::is_empty(state / "invocation");
  }

  stdfs::path state;
  stdfs::path output_dir;
  std::atomic<bool> cancel{false};
};

TEST(CollectValue, EmptyFlatBufferListHasEmptyItems) {
  const auto in = message_type();
  const types::PortType source{"in", PortKind::FlatBuffer, false, in};
  std::string error;
  const auto out = collect_output_type(source, error);
  ASSERT_TRUE(out) << error;
  EXPECT_EQ(out->root_type, "tsugumi.builtin.Collected");
  const std::atomic<bool> cancel{false};
  const auto value = collect_values(
      source, {"list", PortKind::FlatBuffer, false, out}, {}, {}, {}, cancel);
  EXPECT_EQ(value.name, "list");
  EXPECT_EQ(value.type, out);
  ASSERT_TRUE(types::verify(*out, value.bytes));
  ASSERT_NE(envelopes(value), nullptr);
  EXPECT_EQ(envelopes(value)->size(), 0u);
}

TEST(CollectValue, CollectedTypeDoesNotDependOnTheMemberType) {
  std::string error;
  const auto first = collect_output_type(
      {"in", PortKind::FlatBuffer, false, message_type()}, error);
  const auto second = collect_output_type(
      {"in", PortKind::FlatBuffer, false,
       compile_type("namespace other; table Other { x:int; }", "other.Other")},
      error);
  ASSERT_TRUE(first && second) << error;
  EXPECT_EQ(first, second);
}

TEST(CollectValue, FlatBufferListKeepsOrderAndExactBytes) {
  const auto in = message_type();
  const types::PortType source{"in", PortKind::FlatBuffer, false, in};
  std::string error;
  const auto out = collect_output_type(source, error);
  ASSERT_TRUE(out) << error;
  const double nan = std::bit_cast<double>(kNanBits);
  const std::vector<Value> members{
      message(in, "first", std::numeric_limits<uint64_t>::max(), nan, 0),
      message(in, "second", 1, -std::numeric_limits<double>::infinity(),
              std::nullopt),
  };
  const std::atomic<bool> cancel{false};
  const auto value =
      collect_values(source, {"list", PortKind::FlatBuffer, false, out},
                     members, {}, {}, cancel);
  ASSERT_TRUE(types::verify(*out, value.bytes));
  const auto* items = envelopes(value);
  ASSERT_NE(items, nullptr);
  ASSERT_EQ(items->size(), 2u);
  for (flatbuffers::uoffset_t i = 0; i < items->size(); ++i) {
    const auto* item = items->Get(i);
    EXPECT_EQ(item->name()->str(), std::to_string(i));
    EXPECT_EQ(item->kind(), PortKind::FlatBuffer);
    EXPECT_EQ(item->path(), nullptr);
    EXPECT_EQ(bytes_of(item), members[i].bytes);
    // Each member is still readable in place.
    EXPECT_EQ((reinterpret_cast<const char*>(item->bytes()->data()) -
               value.bytes.data()) %
                  FLATBUFFERS_MAX_ALIGNMENT,
              0);
  }
}

TEST(CollectValue, FlatBufferMembersAreNotOpened) {
  const auto in = message_type();
  const types::PortType source{"in", PortKind::FlatBuffer, false, in};
  std::string error;
  const types::PortType output{"list", PortKind::FlatBuffer, false,
                               collect_output_type(source, error)};
  ASSERT_TRUE(output.type) << error;
  std::atomic<bool> cancel{false};

  const std::vector<Value> opaque{
      Value{"in", PortKind::FlatBuffer, in, "garbage", std::nullopt}};
  const auto value = collect_values(source, output, opaque, {}, {}, cancel);
  ASSERT_EQ(envelopes(value)->size(), 1u);
  EXPECT_EQ(bytes_of(envelopes(value)->Get(0)), "garbage");

  const std::vector<Value> wrong_kind{path_member("/tmp/x")};
  EXPECT_THROW((void)collect_values(source, output, wrong_kind, {}, {}, cancel),
               Failed);

  cancel = true;
  const std::vector<Value> members{message(in, "x", 1, 1.0, std::nullopt)};
  try {
    (void)collect_values(source, output, members, {}, {}, cancel);
    FAIL() << "cancellation was ignored";
  } catch (const Failed& e) {
    EXPECT_EQ(e.failure.kind, FailureKind::cancelled);
  }
}

TEST(CollectValue, RejectsACollectedVectorLargerThan16MiB) {
  const auto in = message_type();
  const types::PortType source{"in", PortKind::FlatBuffer, false, in};
  std::string error;
  const types::PortType output{"out", PortKind::FlatBuffer, false,
                               collect_output_type(source, error)};
  ASSERT_TRUE(output.type) << error;
  const std::string label(size_t{9} << 20, 'a');
  const std::vector<Value> members{message(in, label, 1, 1.0, std::nullopt),
                                   message(in, label, 2, 2.0, std::nullopt)};
  const std::atomic<bool> cancel{false};
  try {
    (void)collect_values(source, output, members, {}, {}, cancel);
    FAIL() << "a vector larger than the worker value limit was accepted";
  } catch (const Failed& failure) {
    EXPECT_EQ(failure.failure.kind, FailureKind::output_invalid);
  }
}

TEST_F(CollectFiles, RejectsFilesOverTheFileCountLimit) {
  const auto source = state / "runs" / "tree";
  write_text(source / "first", "content");
  write_text(source / "second", "");
  try {
    (void)collect({path_member(source)}, {.files = 1});
    FAIL() << "the file count limit was ignored";
  } catch (const Failed& failure) {
    EXPECT_EQ(failure.failure.kind, FailureKind::output_invalid);
  }
  EXPECT_TRUE(invocation_is_clean());
}

TEST_F(CollectFiles, CountsMembersAgainstTheFilesystemEntryLimit) {
  const auto source = state / "runs" / "tree";
  stdfs::create_directories(source / "first");
  stdfs::create_directory(source / "second");
  EXPECT_THROW((void)collect({path_member(source)}, {.entries = 2}), Failed);
  EXPECT_TRUE(invocation_is_clean());
  EXPECT_NO_THROW((void)collect({path_member(source)}, {.entries = 3}));
}

TEST_F(CollectFiles, SavedArtifactCopyKeepsItsHashAndDiscardsCancelledWrites) {
  const auto source = state / "runs" / "source.bin";
  const auto destination = state / "inputs" / "destination.bin";
  std::string bytes(145'023, 'a');
  bytes[65'535] = '\0';
  bytes[65'536] = 'b';
  bytes.back() = 'z';
  write_text(source, bytes);
  crypto::Sha256 hash;
  EXPECT_EQ(
      fs::copy_beneath(source.parent_path(), source.filename(), destination,
                       [&](std::string_view chunk) { hash.update(chunk); }),
      bytes.size());
  EXPECT_EQ(hash.finish_hex(), crypto::sha256_hex(bytes));
  EXPECT_EQ(read_text(destination), bytes);

  size_t chunks = 0;
  EXPECT_THROW(fs::copy_beneath(
                   source.parent_path(), source.filename(), destination,
                   [&](std::string_view) {
                     if (++chunks == 2)
                       throw Failed(FailureKind::cancelled, "copy cancelled");
                   }),
               Failed);
  EXPECT_EQ(chunks, 2u);
  EXPECT_EQ(read_text(destination), bytes);
  EXPECT_EQ(std::distance(stdfs::directory_iterator(destination.parent_path()),
                          stdfs::directory_iterator{}),
            1);
}

TEST_F(CollectFiles, EmptyListMakesEmptyRoot) {
  const auto value = collect({});
  ASSERT_TRUE(stdfs::is_directory(output_dir));
  EXPECT_TRUE(stdfs::is_empty(output_dir));
  EXPECT_EQ(value.kind, PortKind::Path);
  EXPECT_EQ(value.path, output_dir.string());
  EXPECT_TRUE(value.bytes.empty());
}

TEST_F(CollectFiles, CopiesFromExplicitInvocationScratchRoot) {
  const auto scratch = state / "scratch";
  const auto file = scratch / "run" / "out" / "result.txt";
  write_text(file, "latest run scratch");
  const auto source = port("in", PortKind::Path, nullptr);
  std::string error;
  const auto type = collect_output_type(source, error);
  ASSERT_TRUE(type) << error;
  const std::vector roots{state / "runs", state / "inputs", scratch};
  const auto value =
      collect_values(source, port("out", PortKind::Path, type),
                     std::vector{path_member(file)}, roots, output_dir, cancel);
  EXPECT_EQ(value.path, output_dir.string());
  EXPECT_EQ(read_text(output_dir / "0" / "result.txt"), "latest run scratch");
}

TEST_F(CollectFiles, FilesAndDirectoriesKeepOrderAndDuplicateBasenames) {
  write_text(state / "runs" / "a" / "out" / "report.txt", "from run");
  write_text(state / "inputs" / "b" / "report.txt", "from input");
  write_text(state / "runs" / "c" / "data" / "sub" / "deep.txt", "deep");
  stdfs::create_directories(state / "runs" / "c" / "data" / "empty");
  const auto value = collect({
      path_member(state / "runs" / "a" / "out" / "report.txt"),
      path_member(state / "inputs" / "b" / "report.txt"),
      path_member(state / "runs" / "c" / "data"),
  });
  EXPECT_EQ(value.name, "out");
  EXPECT_EQ(value.kind, PortKind::Path);
  EXPECT_EQ(value.path, output_dir.string());
  EXPECT_EQ(read_text(output_dir / "0" / "report.txt"), "from run");
  EXPECT_EQ(read_text(output_dir / "1" / "report.txt"), "from input");
  EXPECT_EQ(read_text(output_dir / "2" / "data" / "sub" / "deep.txt"), "deep");
  EXPECT_TRUE(stdfs::is_directory(output_dir / "2" / "data" / "empty"));
  // Only the output folder was added beside it.
  EXPECT_EQ(std::distance(stdfs::directory_iterator(state / "invocation"),
                          stdfs::directory_iterator()),
            1);
}

TEST_F(CollectFiles, RejectsSymlinksAndLeavesNoOutput) {
  write_text(state / "runs" / "r1" / "data" / "real.txt", "real");
  stdfs::create_symlink(state / "runs" / "r1" / "data" / "real.txt",
                        state / "runs" / "r1" / "data" / "link.txt");
  stdfs::create_symlink(state / "runs" / "r1" / "data",
                        state / "runs" / "r1" / "alias");

  const auto expect_invalid = [&](const stdfs::path& path) {
    try {
      (void)collect({path_member(path)});
      ADD_FAILURE() << "accepted " << path;
    } catch (const Failed& e) {
      EXPECT_EQ(e.failure.kind, FailureKind::output_invalid) << e.what();
    }
    EXPECT_FALSE(stdfs::exists(output_dir));
    EXPECT_TRUE(invocation_is_clean());
  };
  // A link inside a collected directory.
  expect_invalid(state / "runs" / "r1" / "data");
  // A link as the collected path itself.
  expect_invalid(state / "runs" / "r1" / "data" / "link.txt");
  // A link on an intermediate component.
  expect_invalid(state / "runs" / "r1" / "alias" / "real.txt");
  // Paths leaving the trusted folders.
  write_text(state / "outside.txt", "secret");
  expect_invalid(state / "outside.txt");
  expect_invalid(state / "runs" / ".." / "outside.txt");
}

TEST_F(CollectFiles, CancelLeavesNoOutput) {
  write_text(state / "runs" / "a" / "f.txt", "data");
  cancel = true;
  try {
    (void)collect({path_member(state / "runs" / "a" / "f.txt")});
    FAIL() << "cancellation was ignored";
  } catch (const Failed& e) {
    EXPECT_EQ(e.failure.kind, FailureKind::cancelled);
  }
  EXPECT_FALSE(stdfs::exists(output_dir));
  EXPECT_TRUE(invocation_is_clean());
}

}  // namespace
}  // namespace tsugumi
