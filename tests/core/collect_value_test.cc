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

#include "util/crypto.hh"
#include "util/fs.hh"

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
)", "test.Message");
}

auto path_type() -> types::TypeRef {
  return compile_type("namespace test; table Path { value:string (id: 0); }", "test.Path");
}

auto finish(flatbuffers::FlatBufferBuilder& b, flatbuffers::uoffset_t root) -> std::string {
  b.Finish(flatbuffers::Offset<flatbuffers::Table>(root));
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}

auto message(const types::TypeRef& type, const std::string& label, uint64_t big, double real,
             std::optional<int32_t> maybe) -> Value {
  flatbuffers::FlatBufferBuilder b;
  b.ForceDefaults(true);
  const auto inner_start = b.StartTable();
  b.AddElement<uint64_t>(kSlot0, big, 0);
  b.AddElement<double>(kSlot1, real, 0.0);
  if (maybe) b.AddElement<int32_t>(kSlot2, *maybe, 0);
  const auto inner = flatbuffers::Offset<flatbuffers::Table>(b.EndTable(inner_start));
  const auto text = b.CreateString(label);
  const auto start = b.StartTable();
  b.AddOffset(kSlot0, text);
  b.AddOffset(kSlot1, inner);
  return Value{"in", PortKind::FlatBuffer, type, finish(b, b.EndTable(start)), std::nullopt};
}

auto path_value_of(const types::TypeRef& type, PortKind kind, const stdfs::path& path) -> Value {
  flatbuffers::FlatBufferBuilder b;
  const auto text = b.CreateString(path.string());
  const auto start = b.StartTable();
  b.AddOffset(kSlot0, text);
  return Value{"in", kind, type, finish(b, b.EndTable(start)), std::nullopt};
}

auto root_of(const Value& v) -> const flatbuffers::Table* {
  return flatbuffers::GetRoot<flatbuffers::Table>(v.bytes.data());
}

auto items_of(const flatbuffers::Table* table, flatbuffers::voffset_t slot)
    -> const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>* {
  return table->GetPointer<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*>(slot);
}

auto string_of(const flatbuffers::Table* table, flatbuffers::voffset_t slot) -> std::string {
  const auto* s = table->GetPointer<const flatbuffers::String*>(slot);
  return s ? s->str() : "<missing>";
}

auto write_text(const stdfs::path& path, const std::string& text) -> void {
  stdfs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

auto read_text(const stdfs::path& path) -> std::string { return fs::read_file(path).value_or("<missing>"); }

// A fresh state folder with runs/ and inputs/, removed afterwards.
class CollectFiles : public ::testing::Test {
 protected:
  void SetUp() override {
    state = stdfs::temp_directory_path() / ("tsugumi-collect-test-" + crypto::random_token(8));
    stdfs::create_directories(state / "runs");
    stdfs::create_directories(state / "inputs");
    stdfs::create_directories(state / "invocation");
    output_dir = state / "invocation" / "out";
  }
  void TearDown() override {
    std::error_code ec;
    stdfs::remove_all(state, ec);
  }

  auto port(const char* name, PortKind kind, types::TypeRef type) -> types::PortType {
    return {name, kind, false, std::move(type)};
  }

  auto collect(PortKind kind, const std::vector<Value>& members) -> Value {
    const auto source = port("in", kind, path_type());
    std::string error;
    const auto type = collect_output_type(source, error);
    EXPECT_TRUE(type) << error;
    return collect_values(source, port("out", PortKind::Directory, type), members, state, output_dir, cancel);
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
  EXPECT_EQ(out->root_type, "tsugumi.collect.Collected");
  const std::atomic<bool> cancel{false};
  const auto value = collect_values(source, {"list", PortKind::FlatBuffer, false, out}, {}, {}, {}, cancel);
  EXPECT_EQ(value.name, "list");
  EXPECT_EQ(value.type, out);
  ASSERT_TRUE(types::verify(*out, value.bytes));
  const auto* items = items_of(root_of(value), kSlot0);
  ASSERT_NE(items, nullptr);
  EXPECT_EQ(items->size(), 0u);
}

TEST(CollectValue, FlatBufferListKeepsOrderAndExactValues) {
  const auto in = message_type();
  const types::PortType source{"in", PortKind::FlatBuffer, false, in};
  std::string error;
  const auto out = collect_output_type(source, error);
  ASSERT_TRUE(out) << error;
  const double nan = std::bit_cast<double>(kNanBits);
  const std::vector<Value> members{
      message(in, "first", std::numeric_limits<uint64_t>::max(), nan, 0),
      message(in, "second", 1, -std::numeric_limits<double>::infinity(), std::nullopt),
  };
  const std::atomic<bool> cancel{false};
  const auto value = collect_values(source, {"list", PortKind::FlatBuffer, false, out}, members, {}, {}, cancel);
  ASSERT_TRUE(types::verify(*out, value.bytes));
  const auto* items = items_of(root_of(value), kSlot0);
  ASSERT_NE(items, nullptr);
  ASSERT_EQ(items->size(), 2u);

  const auto* first = items->Get(0);
  EXPECT_EQ(string_of(first, kSlot0), "first");
  const auto* inner = first->GetPointer<const flatbuffers::Table*>(kSlot1);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->GetField<uint64_t>(kSlot0, 0), std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(std::bit_cast<uint64_t>(inner->GetField<double>(kSlot1, 0.0)), kNanBits);
  EXPECT_TRUE(inner->CheckField(kSlot2));  // optional present with value 0
  EXPECT_EQ(inner->GetField<int32_t>(kSlot2, 1), 0);

  const auto* second = items->Get(1);
  EXPECT_EQ(string_of(second, kSlot0), "second");
  inner = second->GetPointer<const flatbuffers::Table*>(kSlot1);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->GetField<uint64_t>(kSlot0, 0), 1u);
  EXPECT_EQ(inner->GetField<double>(kSlot1, 0.0), -std::numeric_limits<double>::infinity());
  EXPECT_FALSE(inner->CheckField(kSlot2));  // optional absent stays absent
}

TEST(CollectValue, FlatBufferListKeepsUnionAndNestedVectors) {
  const auto in = compile_type(R"(namespace test;
table Nested { value:long; }
table Payload { numbers:[long]; nested:[Nested]; }
union Member { Payload }
table Value { member:Member; }
)", "test.Value");
  const types::PortType source{"in", PortKind::FlatBuffer, false, in};
  std::string error;
  const auto out = collect_output_type(source, error);
  ASSERT_TRUE(out) << error;
  std::string bytes;
  ASSERT_TRUE(types::json_to_flatbuffer(
      *in, R"({"member_type":"Payload","member":{"numbers":[-1,7],"nested":[{"value":42}]}})", &bytes, &error)) << error;
  const std::vector<Value> members{{"in", PortKind::FlatBuffer, in, std::move(bytes), std::nullopt}};
  const std::atomic<bool> cancel{false};
  const auto value = collect_values(source, {"list", PortKind::FlatBuffer, false, out}, members, {}, {}, cancel);
  const auto* items = items_of(root_of(value), kSlot0);
  ASSERT_NE(items, nullptr);
  ASSERT_EQ(items->size(), 1u);
  const auto* member = items->Get(0);
  ASSERT_EQ(member->GetField<uint8_t>(kSlot0, 0), 1u);
  const auto* payload = member->GetPointer<const flatbuffers::Table*>(kSlot1);
  ASSERT_NE(payload, nullptr);
  const auto* numbers = payload->GetPointer<const flatbuffers::Vector<int64_t>*>(kSlot0);
  ASSERT_NE(numbers, nullptr);
  ASSERT_EQ(numbers->size(), 2u);
  EXPECT_EQ(numbers->Get(0), -1);
  EXPECT_EQ(numbers->Get(1), 7);
  const auto* nested = items_of(payload, kSlot1);
  ASSERT_NE(nested, nullptr);
  ASSERT_EQ(nested->size(), 1u);
  EXPECT_EQ(nested->Get(0)->GetField<int64_t>(kSlot0, 0), 42);
}

TEST(CollectValue, FlatBufferRejectsInvalidMemberAndCancel) {
  const auto in = message_type();
  const types::PortType source{"in", PortKind::FlatBuffer, false, in};
  std::string error;
  const types::PortType output{"list", PortKind::FlatBuffer, false, collect_output_type(source, error)};
  ASSERT_TRUE(output.type) << error;
  std::atomic<bool> cancel{false};

  const std::vector<Value> broken{Value{"in", PortKind::FlatBuffer, in, "garbage", std::nullopt}};
  try {
    (void)collect_values(source, output, broken, {}, {}, cancel);
    FAIL() << "an unverifiable member was accepted";
  } catch (const Failed& e) {
    EXPECT_EQ(e.failure.kind, FailureKind::output_invalid);
  }

  cancel = true;
  const std::vector<Value> members{message(in, "x", 1, 1.0, std::nullopt)};
  try {
    (void)collect_values(source, output, members, {}, {}, cancel);
    FAIL() << "cancellation was ignored";
  } catch (const Failed& e) {
    EXPECT_EQ(e.failure.kind, FailureKind::cancelled);
  }
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
  EXPECT_EQ(fs::copy_beneath(source.parent_path(), source.filename(), destination,
                            [&](std::string_view chunk) { hash.update(chunk); }), bytes.size());
  EXPECT_EQ(hash.finish_hex(), crypto::sha256_hex(bytes));
  EXPECT_EQ(read_text(destination), bytes);

  size_t chunks = 0;
  EXPECT_THROW(fs::copy_beneath(source.parent_path(), source.filename(), destination, [&](std::string_view) {
    if (++chunks == 2) throw Failed(FailureKind::cancelled, "copy cancelled");
  }), Failed);
  EXPECT_EQ(chunks, 2u);
  EXPECT_EQ(read_text(destination), bytes);
  EXPECT_EQ(std::distance(stdfs::directory_iterator(destination.parent_path()), stdfs::directory_iterator{}), 1);
}

TEST_F(CollectFiles, EmptyListMakesEmptyRoot) {
  const auto value = collect(PortKind::File, {});
  ASSERT_TRUE(stdfs::is_directory(output_dir));
  EXPECT_TRUE(stdfs::is_empty(output_dir));
  EXPECT_EQ(path_value(value), output_dir.string());
  const auto* items = items_of(root_of(value), kSlot1);
  ASSERT_NE(items, nullptr);
  EXPECT_EQ(items->size(), 0u);
}

TEST_F(CollectFiles, FilesKeepOrderAndDuplicateBasenames) {
  write_text(state / "runs" / "a" / "out" / "report.txt", "from run");
  write_text(state / "inputs" / "b" / "report.txt", "from input");
  write_text(state / "runs" / "c" / "empty.bin", "");
  const auto type = path_type();
  const auto value = collect(PortKind::File, {
      path_value_of(type, PortKind::File, state / "runs" / "a" / "out" / "report.txt"),
      path_value_of(type, PortKind::File, state / "inputs" / "b" / "report.txt"),
      path_value_of(type, PortKind::File, state / "runs" / "c" / "empty.bin"),
  });
  EXPECT_EQ(value.name, "out");
  EXPECT_EQ(value.kind, PortKind::Directory);
  ASSERT_TRUE(types::verify(*value.type, value.bytes));
  EXPECT_EQ(path_value(value), output_dir.string());
  EXPECT_EQ(read_text(output_dir / "0" / "report.txt"), "from run");
  EXPECT_EQ(read_text(output_dir / "1" / "report.txt"), "from input");
  EXPECT_EQ(read_text(output_dir / "2" / "empty.bin"), "");

  const auto* items = items_of(root_of(value), kSlot1);
  ASSERT_NE(items, nullptr);
  ASSERT_EQ(items->size(), 3u);
  EXPECT_EQ(string_of(items->Get(0), kSlot0), "report.txt");
  EXPECT_EQ(string_of(items->Get(0), kSlot1), "0/report.txt");
  EXPECT_EQ(string_of(items->Get(1), kSlot0), "report.txt");
  EXPECT_EQ(string_of(items->Get(1), kSlot1), "1/report.txt");
  EXPECT_EQ(string_of(items->Get(2), kSlot1), "2/empty.bin");
  // Only the output folder was added beside it.
  EXPECT_EQ(std::distance(stdfs::directory_iterator(state / "invocation"), stdfs::directory_iterator()), 1);
}

TEST_F(CollectFiles, DirectoriesKeepNestedFilesAndEmptyChildren) {
  write_text(state / "runs" / "r1" / "data" / "top.txt", "top");
  write_text(state / "runs" / "r1" / "data" / "sub" / "deep.txt", "deep");
  stdfs::create_directories(state / "runs" / "r1" / "data" / "empty");
  write_text(state / "runs" / "r2" / "data" / "other.txt", "other");
  const auto type = path_type();
  const auto value = collect(PortKind::Directory, {
      path_value_of(type, PortKind::Directory, state / "runs" / "r1" / "data"),
      path_value_of(type, PortKind::Directory, state / "runs" / "r2" / "data"),
  });
  EXPECT_EQ(read_text(output_dir / "0" / "data" / "top.txt"), "top");
  EXPECT_EQ(read_text(output_dir / "0" / "data" / "sub" / "deep.txt"), "deep");
  EXPECT_TRUE(stdfs::is_directory(output_dir / "0" / "data" / "empty"));
  EXPECT_TRUE(stdfs::is_empty(output_dir / "0" / "data" / "empty"));
  EXPECT_EQ(read_text(output_dir / "1" / "data" / "other.txt"), "other");
  EXPECT_FALSE(stdfs::exists(output_dir / "1" / "data" / "top.txt"));

  const auto* items = items_of(root_of(value), kSlot1);
  ASSERT_NE(items, nullptr);
  ASSERT_EQ(items->size(), 2u);
  EXPECT_EQ(string_of(items->Get(0), kSlot0), "data");
  EXPECT_EQ(string_of(items->Get(0), kSlot1), "0/data");
  EXPECT_EQ(string_of(items->Get(1), kSlot1), "1/data");
}

TEST_F(CollectFiles, RejectsSymlinksAndLeavesNoOutput) {
  write_text(state / "runs" / "r1" / "data" / "real.txt", "real");
  stdfs::create_symlink(state / "runs" / "r1" / "data" / "real.txt", state / "runs" / "r1" / "data" / "link.txt");
  stdfs::create_symlink(state / "runs" / "r1" / "data", state / "runs" / "r1" / "alias");
  const auto type = path_type();

  const auto expect_invalid = [&](PortKind kind, const stdfs::path& path) {
    try {
      (void)collect(kind, {path_value_of(type, kind, path)});
      ADD_FAILURE() << "accepted " << path;
    } catch (const Failed& e) {
      EXPECT_EQ(e.failure.kind, FailureKind::output_invalid) << e.what();
    }
    EXPECT_FALSE(stdfs::exists(output_dir));
    EXPECT_TRUE(invocation_is_clean());
  };
  // A link inside a collected directory.
  expect_invalid(PortKind::Directory, state / "runs" / "r1" / "data");
  // A link as the collected file itself.
  expect_invalid(PortKind::File, state / "runs" / "r1" / "data" / "link.txt");
  // A link on an intermediate component.
  expect_invalid(PortKind::File, state / "runs" / "r1" / "alias" / "real.txt");
  // Paths leaving the trusted folders.
  write_text(state / "outside.txt", "secret");
  expect_invalid(PortKind::File, state / "outside.txt");
  expect_invalid(PortKind::File, state / "runs" / ".." / "outside.txt");
}

TEST_F(CollectFiles, CancelLeavesNoOutput) {
  write_text(state / "runs" / "a" / "f.txt", "data");
  cancel = true;
  try {
    (void)collect(PortKind::File, {path_value_of(path_type(), PortKind::File, state / "runs" / "a" / "f.txt")});
    FAIL() << "cancellation was ignored";
  } catch (const Failed& e) {
    EXPECT_EQ(e.failure.kind, FailureKind::cancelled);
  }
  EXPECT_FALSE(stdfs::exists(output_dir));
  EXPECT_TRUE(invocation_is_clean());
}

}  // namespace
}  // namespace tsugumi
