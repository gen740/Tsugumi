#include "host/projects.hh"

#include <algorithm>
#include <chrono>

#include "agent/json.hh"
#include "util/fs.hh"

namespace tsugumi::host {
namespace {
namespace A = Tsugumi::Agent;
namespace f = std::filesystem;
namespace json = agent::json;

auto Text(const flatbuffers::String* value, std::string fallback = {})
    -> std::string {
  return value ? value->str() : fallback;
}

auto Name(const std::string& name) -> void {
  // A single folder component, preserved verbatim (never sanitized).
  if (name.empty() || !fs::valid_relative_path(name) || name == "." ||
      name == ".." || name.back() == '.' || name.front() == ' ' ||
      name.back() == ' ' ||
      std::any_of(name.begin(), name.end(), [](unsigned char c) {
        return c < 32 || c == 127 ||
               std::string_view("/\\<>:\"|?*").find(c) !=
                   std::string_view::npos;
      }))
    throw ProjectFailure("invalid_project_name",
                         "Choose a non-empty folder name without separators, "
                         "control characters or reserved punctuation");
}

auto Path(const std::string& value) -> f::path {
  if (value.empty() || value.find('\0') != std::string::npos)
    throw ProjectFailure("project_io_error", "A host folder path is required");
  return f::absolute(value).lexically_normal();
}

auto Info(const f::path& folder) -> std::string {
  if (!f::is_regular_file(folder / "tsugumi.json"))
    throw ProjectFailure("invalid_project",
                         "Choose a folder containing tsugumi.json");
  const auto project =
      json::Object(fs::read_file_or_throw(folder / "tsugumi.json"));
  auto name = json::Text(project, "name");
  if (name.empty()) name = folder.filename().string();
  return json::Encode(
      json::Members{{"path", json::Quote(f::canonical(folder).string())},
                    {"name", json::Quote(name)}});
}

auto Pin() -> std::string {
  const std::string rev = TSUGUMI_CARD_NIXPKGS_REV;
  const std::string hash = TSUGUMI_CARD_NIXPKGS_NARHASH;
  if (rev.empty() || hash.empty())
    throw ProjectFailure("project_defaults_unavailable",
                         "This build has no default Card Nixpkgs pin");
  return "https://github.com/NixOS/nixpkgs/archive/" + rev + ".tar.gz#" + hash;
}

// Destination ownership is established before writing any files. Never remove
// somebody else's pre-existing folder on failure, including an accepted empty
// one.
// A folder a person merely looked at in Finder holds a .DS_Store; that is
// still an empty folder to them.
auto HoldsNothing(const f::path& folder) -> bool {
  for (const auto& entry : f::directory_iterator(folder))
    if (entry.path().filename() != ".DS_Store") return false;
  return true;
}

auto Prepare(const f::path& destination) -> bool {
  if (f::is_symlink(f::symlink_status(destination)))
    throw ProjectFailure(
        "folder_not_empty",
        "The destination must be a new or empty folder, not a symbolic link");
  if (f::exists(destination)) {
    if (!f::is_directory(destination) || !HoldsNothing(destination))
      throw ProjectFailure(
          "folder_not_empty",
          "The destination is not empty; choose an empty folder or a new name");
    return false;
  }
  f::create_directories(destination.parent_path());
  if (!f::create_directory(destination))
    throw ProjectFailure("folder_not_empty",
                         "The destination was created by another operation");
  return true;
}

auto Copy(const f::path& source, const f::path& destination) -> void {
  // Do not traverse links or bring repository identity and run state along.
  for (const auto& entry : f::directory_iterator(source)) {
    const auto name = entry.path().filename();
    if (name == ".tsugumi" || name == ".git" || entry.is_symlink()) continue;
    const auto target = destination / name;
    if (entry.is_directory()) {
      f::create_directory(target);
      Copy(entry.path(), target);
    } else if (entry.is_regular_file()) {
      f::copy_file(entry.path(), target);
      f::permissions(target,
                     entry.status().permissions() | f::perms::owner_write);
    }
  }
  f::permissions(destination, f::perms::owner_all, f::perm_options::add);
}

template <typename Write>
auto Populate(const f::path& destination, Write write) -> std::string {
  const bool created = Prepare(destination);
  try {
    write();
    return json::Encode(json::Members{
        {"path", json::Quote(f::canonical(destination).string())}});
  } catch (...) {
    if (created) {
      std::error_code ignored;
      f::remove_all(destination, ignored);
    } else {
      // Only this operation's files can be in the accepted empty destination.
      std::error_code ignored;
      for (const auto& entry : f::directory_iterator(destination))
        f::remove_all(entry.path(), ignored);
    }
    throw;
  }
}
}  // namespace

auto Projects::Handles(A::RequestPayload type) -> bool {
  return type >= A::RequestPayload::HostProjectDefaultsRequest &&
         type <= A::RequestPayload::HostRecentForgetRequest;
}

auto Projects::Catalog(const std::string& language) const
    -> std::vector<std::string> {
  if (language != "ja" && language != "en")
    throw ProjectFailure("invalid_language",
                         "Choose ja or en for the demo catalog");
  if (demos_.empty()) return {};
  const auto root = f::absolute(demos_) / language;
  const auto raw = fs::read_file(root / "index.json");
  if (!raw) return {};
  auto entries = json::Array(json::Get(json::Object(*raw), "demos", "[]"));
  for (auto& entry : entries) {
    auto value = json::Object(entry);
    const auto relative = json::Text(value, "path");
    Name(json::Text(value, "id"));
    if (!fs::valid_relative_path(relative) ||
        !fs::resolves_inside(root, root / relative))
      throw ProjectFailure("demo_not_found",
                           "The bundled demo path is invalid");
    value["path"] = json::Quote((root / relative).string());
    value["description"] = json::Quote(json::Text(value, "description"));
    const auto tags = json::Array(json::Get(value, "tags", "[]"));
    for (const auto& tag : tags) {
      schema::v::String text;
      if (schema::decode(tag, text))
        throw ProjectFailure("demo_not_found",
                             "Bundled demo tags must be strings");
    }
    value["tags"] = json::Encode(tags);
    if (json::Text(value, "field").empty()) value.erase("field");
    entry = json::Encode(value);
  }
  return entries;
}

auto Projects::Recents() const -> std::vector<std::string> {
  const auto raw = fs::read_file(registry_);
  auto entries = raw ? json::Array(*raw) : std::vector<std::string>{};
  std::erase_if(entries, [](const auto& entry) {
    return !f::is_regular_file(
        f::path(json::Text(json::Object(entry), "path")) / "tsugumi.json");
  });
  return entries;
}

auto Projects::Handle(const A::Request& request) -> std::string {
  try {
    switch (request.value_type()) {
      case A::RequestPayload::HostProjectDefaultsRequest:
        return json::Encode(json::Members{{"nixpkgsPin", json::Quote(Pin())}});
      case A::RequestPayload::HostProjectInfoRequest:
        return Info(
            Path(request.value_as_HostProjectInfoRequest()->path()->str()));
      case A::RequestPayload::HostProjectCreateRequest: {
        const auto* p = request.value_as_HostProjectCreateRequest();
        const auto name = p->name()->str();
        Name(name);
        const auto flow = Text(p->flow(), "topToBottom");
        if (flow != "topToBottom" && flow != "leftToRight")
          throw ProjectFailure("invalid_flow",
                               "Choose topToBottom or leftToRight");
        const auto pin = Pin();
        const auto destination = Path(p->parent()->str()) / name;
        return Populate(destination, [&] {
          fs::write_atomic(
              destination / "tsugumi.json",
              json::Encode(json::Members{
                  {"name", json::Quote(name)},
                  {"pkgsPath", json::Quote(pin)},
                  {"canvas", R"({"cards":[],"connections":[]})"}}) +
                  '\n');
          fs::write_atomic(
              destination / "layout.json",
              json::Encode(json::Members{{"flow", json::Quote(flow)}}) + '\n');
        });
      }
      case A::RequestPayload::HostProjectDuplicateRequest: {
        const auto* p = request.value_as_HostProjectDuplicateRequest();
        Name(p->name()->str());
        const auto source = f::canonical(Path(p->path()->str()));
        Info(source);
        const auto destination = Path(p->parent()->str()) / p->name()->str();
        if (fs::resolves_inside(source, destination))
          throw ProjectFailure("invalid_project",
                               "The copy must be outside the source project");
        return Populate(destination, [&] { Copy(source, destination); });
      }
      case A::RequestPayload::HostDemoListRequest:
        return json::Encode(Catalog(
            Text(request.value_as_HostDemoListRequest()->language(), "ja")));
      case A::RequestPayload::HostDemoCopyRequest: {
        const auto* p = request.value_as_HostDemoCopyRequest();
        const auto id = p->id()->str();
        const auto entries = Catalog(Text(p->language(), "ja"));
        for (const auto& entry : entries) {
          const auto value = json::Object(entry);
          if (json::Text(value, "id") != id) continue;
          const auto parent = Path(p->parent()->str());
          auto destination = parent / id;
          for (int n = 2; f::exists(f::symlink_status(destination)); ++n)
            destination = parent / (id + "-" + std::to_string(n));
          const auto source = f::path(json::Text(value, "path"));
          Info(source);
          if (fs::resolves_inside(source, destination))
            throw ProjectFailure("invalid_project",
                                 "The copy must be outside the demo");
          return Populate(destination, [&] { Copy(source, destination); });
        }
        throw ProjectFailure("demo_not_found",
                             "The bundled demo was not found");
      }
      case A::RequestPayload::HostRecentListRequest:
        return json::Encode(Recents());
      case A::RequestPayload::HostRecentRememberRequest: {
        auto value = json::Object(Info(
            Path(request.value_as_HostRecentRememberRequest()->path()->str())));
        const auto path = json::Text(value, "path");
        auto entries = Recents();
        std::erase_if(entries, [&](const auto& entry) {
          return json::Text(json::Object(entry), "path") == path;
        });
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        // Strictly increasing times preserve order even within one clock tick.
        if (!entries.empty())
          now = std::max<int64_t>(
              now,
              json::UInt(json::Get(json::Object(entries.front()), "openedAt")) +
                  1);
        value["openedAt"] = std::to_string(now);
        const auto entry = json::Encode(value);
        entries.insert(entries.begin(), entry);
        if (entries.size() > 30) entries.resize(30);
        fs::write_atomic(registry_, json::Encode(entries), 0600);
        return entry;
      }
      case A::RequestPayload::HostRecentForgetRequest: {
        auto entries = Recents();
        const auto* paths = request.value_as_HostRecentForgetRequest()->paths();
        std::vector<std::string> canonical_paths;
        if (paths)
          for (const auto* path : *paths)
            canonical_paths.push_back(
                f::weakly_canonical(Path(path->str())).string());
        if (paths)
          std::erase_if(entries, [&](const auto& entry) {
            const auto path = json::Text(json::Object(entry), "path");
            return std::find(canonical_paths.begin(), canonical_paths.end(),
                             path) != canonical_paths.end();
          });
        fs::write_atomic(registry_, json::Encode(entries), 0600);
        return "{}";
      }
      default:
        throw ProjectFailure("invalid_request",
                             "Unsupported host project request");
    }
  } catch (const ProjectFailure&) {
    throw;
  } catch (const std::exception& e) {
    throw ProjectFailure("project_io_error", e.what());
  }
}
}  // namespace tsugumi::host
