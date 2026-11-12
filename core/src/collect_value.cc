#include "collect_value.hh"

#include <flatbuffers/reflection.h>

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {

namespace {

namespace stdfs = std::filesystem;
using reflection::BaseType;

constexpr size_t kMaxMessageBytes = size_t{16} << 20;
constexpr size_t kMaxFiles = 1000;
constexpr uint64_t kMaxFileBytes = uint64_t{64} << 30;
constexpr size_t kMaxEntries = 10000;  // files, directories and members together

constexpr const char* kDirectorySchema = R"(namespace tsugumi.collect;
table Entry {
  name:string (id: 0);
  path:string (id: 1);
}
table Directory {
  value:string (id: 0);
  items:[Entry] (id: 1);
}
)";

// The message of a collected File / Directory list (compiled once).
auto directory_type() -> types::TypeRef {
  static const types::TypeRef type = [] {
    std::string error;
    auto compiled = types::compile(kDirectorySchema, "tsugumi.collect.Directory", &error);
    if (!compiled) throw std::logic_error("collect Directory type failed: " + error);
    return *compiled;
  }();
  return type;
}

auto check_cancel(const std::atomic<bool>& cancel) -> void {
  if (cancel.load()) throw Failed(FailureKind::cancelled, "collect cancelled");
}

auto invalid_member(size_t index, const std::string& why) -> Failed {
  return Failed(FailureKind::output_invalid, "collect element " + std::to_string(index) + ": " + why);
}

auto field_of(const reflection::Object& object, const char* name) -> const reflection::Field& {
  const auto* field = object.fields()->LookupByKey(name);
  if (!field)
    throw Failed(FailureKind::internal_error, "collect type " + object.name()->str() + " has no field " + name);
  return *field;
}

// The table type of a vector-of-tables field.
auto element_object(const types::Type& type, const reflection::Field& field) -> const reflection::Object& {
  const auto* t = field.type();
  if (t->base_type() != BaseType::Vector || t->element() != BaseType::Obj || t->index() < 0)
    throw Failed(FailureKind::internal_error, "collect field " + field.name()->str() + " is not a vector of tables");
  return *type.schema()->objects()->Get(static_cast<flatbuffers::uoffset_t>(t->index()));
}

auto as_string(const flatbuffers::FlatBufferBuilder& builder) -> std::string {
  return {reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize()};
}

// The caller's output port must carry the type derived from the source.
auto check_output_port(const types::PortType& source, const types::PortType& output, PortKind kind) -> void {
  if (output.kind != kind || !output.type)
    throw Failed(FailureKind::internal_error, "collect output " + output.name + " has the wrong kind or no type");
  std::string error;
  const auto expected = collect_output_type(source, error);
  if (!expected) throw Failed(FailureKind::internal_error, "collect input type: " + error);
  if (!types::compatible(*expected, *output.type))
    throw Failed(FailureKind::internal_error, "collect output " + output.name + " does not match its input type");
}

auto check_member(const types::PortType& source, const Value& member, size_t index) -> void {
  if (member.kind != source.kind || !member.type) throw invalid_member(index, "wrong kind or no type");
  if (member.type != source.type && !types::compatible(*source.type, *member.type))
    throw invalid_member(index, "type does not match the input port");
  if (!types::verify(*source.type, member.bytes)) throw invalid_member(index, "value does not verify");
}

// --- FlatBuffer members -----------------------------------------------------

auto collect_messages(const types::PortType& source, const types::PortType& output,
                      std::span<const Value> members, const std::atomic<bool>& cancel) -> std::string {
  size_t total = 0;
  for (size_t i = 0; i < members.size(); ++i) {
    check_cancel(cancel);
    check_member(source, members[i], i);
    total += members[i].bytes.size();
    if (total > kMaxMessageBytes) throw Failed(FailureKind::output_invalid, "collected values exceed 16 MiB");
  }
  const auto& items = field_of(*output.type->root(), "items");
  (void)element_object(*output.type, items);

  // Relocating a whole verified buffer leaves its internal offsets intact.
  // The vector points at each relocated root table. This preserves shared
  // tables, scalar bits and optional fields without rebuilding the graph.
  flatbuffers::FlatBufferBuilder builder(total + 1024);
  std::vector<flatbuffers::Offset<const flatbuffers::Table*>> tables;
  tables.reserve(members.size());
  for (const auto& member : members) {
    check_cancel(cancel);
    const auto* data = reinterpret_cast<const uint8_t*>(member.bytes.data());
    const auto root = flatbuffers::ReadScalar<flatbuffers::uoffset_t>(data);
    builder.PreAlign(member.bytes.size(), FLATBUFFERS_MAX_ALIGNMENT);
    builder.PushBytes(data, member.bytes.size());
    tables.emplace_back(static_cast<flatbuffers::uoffset_t>(builder.GetSize() - root));
    if (builder.GetSize() > kMaxMessageBytes)
      throw Failed(FailureKind::output_invalid, "collected value exceeds 16 MiB");
  }
  const auto vector = builder.CreateVector(tables);
  const auto start = builder.StartTable();
  builder.AddOffset(items.offset(), vector);
  builder.Finish(flatbuffers::Offset<flatbuffers::Table>(builder.EndTable(start)));
  if (builder.GetSize() > kMaxMessageBytes) throw Failed(FailureKind::output_invalid, "collected value exceeds 16 MiB");
  auto bytes = as_string(builder);
  if (!types::verify(*output.type, bytes))
    throw Failed(FailureKind::internal_error, "collected value does not verify against its type");
  return bytes;
}

// --- File / Directory members -----------------------------------------------

// Where a member lives: a trusted root (runs or inputs) and a path below it.
struct MemberSource {
  stdfs::path root;
  stdfs::path rel;
  std::string name;  // original basename
};

struct Entry {
  std::string name;
  std::string path;  // relative to the output root
};

struct Budget {
  size_t files = 0;
  uint64_t bytes = 0;
  size_t entries = kMaxEntries;
};

auto member_source(const Value& member, const stdfs::path& state_root, size_t index) -> MemberSource {
  const auto value = path_value(member);
  if (!value) throw invalid_member(index, "has no path");
  const stdfs::path path(*value);
  if (!path.is_absolute() || path.lexically_normal() != path)
    throw invalid_member(index, "path must be absolute and normal");
  for (const char* folder : {"runs", "inputs"}) {
    const auto root = (state_root / folder).lexically_normal();
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
    path_ = output_dir.parent_path() /
            ("." + output_dir.filename().string() + ".collect-tmp-" + crypto::random_token(8));
    // Only a folder this call created is ever removed.
    if (!stdfs::create_directory(path_))
      throw Failed(FailureKind::internal_error, "collect staging folder already exists");
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
      throw Failed(FailureKind::internal_error, "collect output " + target.string() + " already exists");
    stdfs::rename(path_, target);
    committed_ = true;
    fs::fsync_dir(target.parent_path());
  }

 private:
  stdfs::path path_;
  bool committed_ = false;
};

auto copy_file(const stdfs::path& root, const stdfs::path& rel, const stdfs::path& destination, Budget& budget,
               const std::atomic<bool>& cancel) -> void {
  check_cancel(cancel);
  if (budget.files >= kMaxFiles) throw Failed(FailureKind::output_invalid, "collected files exceed 1,000");
  ++budget.files;
  std::ofstream out(destination, std::ios::binary | std::ios::trunc);
  if (!out) throw Failed(FailureKind::internal_error, "cannot create " + destination.string());
  const auto size = fs::stream_beneath(root, rel, kMaxFileBytes - budget.bytes, [&](std::string_view chunk) {
    check_cancel(cancel);
    out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    if (!out) throw Failed(FailureKind::internal_error, "cannot write " + destination.string());
  });
  out.close();
  if (!out) throw Failed(FailureKind::internal_error, "cannot write " + destination.string());
  budget.bytes += size;
}

auto copy_directory(const stdfs::path& root, const stdfs::path& rel, const stdfs::path& destination,
                    Budget& budget, const std::atomic<bool>& cancel, size_t index) -> void {
  const auto listing = fs::list_beneath(root, rel, budget.entries);
  if (!listing.links.empty() || !listing.special.empty())
    throw invalid_member(index, "directory contains a symlink or special file");
  budget.entries -= listing.files.size() + listing.directories.size();
  stdfs::create_directory(destination);
  // Sorted, so every parent precedes its children.
  for (const auto& child : listing.directories) {
    check_cancel(cancel);
    if (!fs::valid_relative_path(child)) throw invalid_member(index, "unsafe name " + child);
    stdfs::create_directory(destination / child);
  }
  for (const auto& child : listing.files) {
    if (!fs::valid_relative_path(child)) throw invalid_member(index, "unsafe name " + child);
    copy_file(root, rel / child, destination / child, budget, cancel);
  }
}

auto directory_message(const types::Type& type, const std::string& value, const std::vector<Entry>& entries)
    -> std::string {
  const auto& root = *type.root();
  const auto& value_field = field_of(root, "value");
  const auto& items_field = field_of(root, "items");
  const auto& entry = element_object(type, items_field);
  const auto& name_field = field_of(entry, "name");
  const auto& path_field = field_of(entry, "path");

  flatbuffers::FlatBufferBuilder builder;
  std::vector<flatbuffers::Offset<flatbuffers::Table>> tables;
  tables.reserve(entries.size());
  for (const auto& e : entries) {
    const auto name = builder.CreateString(e.name);
    const auto path = builder.CreateString(e.path);
    const auto start = builder.StartTable();
    builder.AddOffset(name_field.offset(), name);
    builder.AddOffset(path_field.offset(), path);
    tables.emplace_back(builder.EndTable(start));
  }
  const auto items = builder.CreateVector(tables);
  const auto path = builder.CreateString(value);
  const auto start = builder.StartTable();
  builder.AddOffset(value_field.offset(), path);
  builder.AddOffset(items_field.offset(), items);
  builder.Finish(flatbuffers::Offset<flatbuffers::Table>(builder.EndTable(start)));
  if (builder.GetSize() > kMaxMessageBytes) throw Failed(FailureKind::output_invalid, "collected value exceeds 16 MiB");
  auto bytes = as_string(builder);
  if (!types::verify(type, bytes))
    throw Failed(FailureKind::internal_error, "collected directory value does not verify against its type");
  return bytes;
}

auto collect_files(const types::PortType& source, const types::PortType& output, std::span<const Value> members,
                   const stdfs::path& state_root, const stdfs::path& output_dir, const std::atomic<bool>& cancel)
    -> std::string {
  if (members.size() > kMaxEntries) throw Failed(FailureKind::output_invalid, "too many collected elements");
  std::error_code ec;
  if (stdfs::symlink_status(output_dir, ec).type() != stdfs::file_type::not_found)
    throw Failed(FailureKind::internal_error, "collect output " + output_dir.string() + " already exists");

  // Every member is validated before anything is written.
  std::vector<MemberSource> sources;
  sources.reserve(members.size());
  for (size_t i = 0; i < members.size(); ++i) {
    check_cancel(cancel);
    check_member(source, members[i], i);
    sources.push_back(member_source(members[i], state_root, i));
  }

  try {
    Staging staging(output_dir);
    Budget budget;
    budget.entries -= members.size();
    std::vector<Entry> entries;
    entries.reserve(sources.size());
    for (size_t i = 0; i < sources.size(); ++i) {
      check_cancel(cancel);
      const auto& member = sources[i];
      // One ordinal folder per element, so equal basenames never collide.
      const auto ordinal = std::to_string(i);
      stdfs::create_directory(staging.path() / ordinal);
      const auto destination = staging.path() / ordinal / member.name;
      try {
        if (source.kind == PortKind::File) copy_file(member.root, member.rel, destination, budget, cancel);
        else copy_directory(member.root, member.rel, destination, budget, cancel, i);
      } catch (const fs::Error& e) {
        throw invalid_member(i, e.what());
      }
      entries.push_back({member.name, ordinal + "/" + member.name});
    }
    check_cancel(cancel);
    auto bytes = directory_message(*output.type, output_dir.string(), entries);
    staging.commit(output_dir);
    return bytes;
  } catch (const stdfs::filesystem_error& e) {
    throw Failed(FailureKind::internal_error, std::string("collect staging failed: ") + e.what());
  } catch (const fs::Error& e) {
    throw Failed(FailureKind::internal_error, std::string("collect staging failed: ") + e.what());
  }
}

}  // namespace

auto collect_output_type(const types::PortType& source, std::string& error) -> types::TypeRef {
  if (!source.type) {
    error = "the collected input " + source.name + " has no resolved type";
    return nullptr;
  }
  if (source.kind == PortKind::FlatBuffer) {
    std::string text;
    const auto member = types::render_closure(*source.type, "tsugumi.collect.member", text, /*nested=*/true);
    text += "namespace tsugumi.collect;\ntable Collected {\n  items:[" + member + "] (id: 0, required);\n}\n";
    auto type = types::compile(text, "tsugumi.collect.Collected", &error);
    return type ? *type : nullptr;
  }
  if (!types::has_string_value(*source.type)) {
    error = "the collected File/Directory input " + source.name + " needs a string `value` field";
    return nullptr;
  }
  try {
    return directory_type();
  } catch (const std::exception& e) {
    error = e.what();
    return nullptr;
  }
}

auto collect_values(const types::PortType& source, const types::PortType& output, std::span<const Value> members,
                    const std::filesystem::path& state_root, const std::filesystem::path& output_dir,
                    const std::atomic<bool>& cancel) -> Value {
  if (!source.type) throw Failed(FailureKind::internal_error, "collect input " + source.name + " has no type");
  check_cancel(cancel);
  Value result;
  result.name = output.name;
  result.kind = output.kind;
  result.type = output.type;
  if (source.kind == PortKind::FlatBuffer) {
    check_output_port(source, output, PortKind::FlatBuffer);
    result.bytes = collect_messages(source, output, members, cancel);
  } else {
    check_output_port(source, output, PortKind::Directory);
    result.bytes = collect_files(source, output, members, state_root, output_dir, cancel);
  }
  return result;
}

}  // namespace tsugumi
