#include "tsugumi/document/schema_sources.hh"

#include <flatbuffers/idl.h>
#include <flatbuffers/util.h>

#include <map>
#include <string_view>

#include "tsugumi/util/fs.hh"

extern const unsigned char tsugumi_artifact_fbs[];
extern const unsigned long tsugumi_artifact_fbs_size;

namespace tsugumi::types {
namespace {
constexpr std::string_view kRoot = "/tsugumi-schema/";
constexpr std::string_view kArtifact = "tsugumi/artifact.fbs";

struct IncludeScope {
  const SchemaSourceReader& read;
  const std::string directory;
  std::map<std::string, std::optional<std::string>> files;

  auto lookup(std::string_view path) -> const std::optional<std::string>& {
    static const std::optional<std::string> missing;
    if (!path.starts_with(kRoot)) return missing;
    path.remove_prefix(kRoot.size());
    if (!fs::valid_relative_path(path)) return missing;
    auto [entry, fresh] = files.try_emplace(std::string(path));
    if (!fresh) return entry->second;
    if (path == kArtifact || path == directory + std::string(kArtifact)) {
      entry->second.emplace(reinterpret_cast<const char*>(tsugumi_artifact_fbs),
                            tsugumi_artifact_fbs_size);
    } else if (read && path.starts_with(directory)) {
      entry->second = read(entry->first);
    }
    return entry->second;
  }
};

// FlatBuffers sets its include callbacks process-wide. Install once, before
// any parser runs. Each definition parse uses its own thread-local snapshot.
thread_local IncludeScope* active = nullptr;
extern const flatbuffers::FileExistsFunction default_exists;
extern const flatbuffers::LoadFileFunction default_load;
auto include_exists(const char* filename) -> bool {
  return active ? active->lookup(filename).has_value()
                : default_exists(filename);
}
auto load_include(const char* filename, bool binary, std::string* out) -> bool {
  if (!active) return default_load(filename, binary, out);
  const auto& contents = active->lookup(filename);
  if (!contents) return false;
  *out = *contents;
  return true;
}
const auto default_exists = flatbuffers::SetFileExistsFunction(include_exists);
const auto default_load = flatbuffers::SetLoadFileFunction(load_include);
}  // namespace

auto parse_schema(flatbuffers::Parser& parser, const std::string& text,
                  const std::string& filename, const SchemaSourceReader& read)
    -> bool {
  const auto slash = filename.rfind('/');
  IncludeScope scope{read, filename.substr(0, slash + 1), {}};
  struct Reset {
    IncludeScope* previous;
    ~Reset() { active = previous; }
  } reset{active};
  active = &scope;
  const std::string root(kRoot);
  const auto directory = root + scope.directory;
  const auto source = root + filename;
  const char* paths[]{directory.c_str(), root.c_str(), nullptr};
  return parser.Parse(text.c_str(), paths, source.c_str());
}

}  // namespace tsugumi::types
