#include "tsugumi/document/document.hh"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

#include "tsugumi/document/graph.hh"
#include "tsugumi/document/readme.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/text.hh"

namespace tsugumi {

namespace stdfs = std::filesystem;

SourceFile::~SourceFile() {
  if (!journaled) {
    std::error_code ec;
    stdfs::remove(path, ec);
  }
}

std::string path_key(const CardPath& p) {
  std::string out;
  for (size_t i = 0; i < p.size(); ++i) {
    if (i) out += '/';
    out += p[i];
  }
  return out;
}

CardPath path_from_key(const std::string& key) {
  CardPath p;
  if (key.empty()) return p;
  size_t s = 0;
  while (true) {
    size_t e = key.find('/', s);
    p.push_back(
        key.substr(s, e == std::string::npos ? std::string::npos : e - s));
    if (e == std::string::npos) break;
    s = e + 1;
  }
  return p;
}

bool valid_id(const std::string& id) {
  if (id.empty() || id.size() > 128 || id == "." || id == "..") return false;
  for (char c : id) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!ok) return false;
  }
  return true;
}


bool valid_name(std::string_view name) {
  if (name.empty() || name.size() > 64 || name[0] < 'a' || name[0] > 'z')
    return false;
  for (char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
      return false;
  return true;
}

namespace {
auto local_document_path(std::string_view path) -> std::string_view {
  while (path.starts_with("externals/")) {
    const auto slash = path.find('/', 10);
    if (slash == std::string_view::npos ||
        !valid_name(path.substr(10, slash - 10)))
      return {};
    path.remove_prefix(slash + 1);
  }
  return path;
}
}  // namespace

auto Document::is_card_note_file(const std::string& path) -> bool {
  const auto rel = local_document_path(path);
  return (rel.ends_with("/README.md") || rel.ends_with("/_comments.json")) &&
         is_document_file(std::string(rel));
}

auto Document::is_generated_input_file(const std::string& path) -> bool {
  auto local = local_document_path(path);
  if (!local.starts_with("definitions/") || !local.ends_with("/_inputs.fbs"))
    return false;
  local.remove_prefix(12);
  local.remove_suffix(12);
  return valid_name(local);
}

auto Document::is_note_file(const std::string& path) -> bool {
  const auto rel = local_document_path(path);
  if (!rel.starts_with("notes/") || !rel.ends_with(".md")) return false;
  const auto id = rel.substr(6, rel.size() - 9);
  return valid_id(std::string(id));
}

bool Document::is_document_file(const std::string& rel) {
  if (rel == "tsugumi.json" || rel == "layout.bin") return true;
  if (!rel.starts_with("definitions/")) return false;
  auto rest = rel.substr(12);
  auto slash = rest.find('/');
  if (slash == std::string::npos || !valid_name(rest.substr(0, slash)))
    return false;
  auto file = rest.substr(slash + 1);
  return file == "_card.json" || file == "README.md" ||
         file == "_comments.json";
}

auto Document::is_external_document_file(std::string_view path) -> bool {
  return path.starts_with("externals/") &&
         (is_document_file(std::string(local_document_path(path))) ||
          is_lock_file(path));
}

auto Document::is_lock_file(std::string_view path) -> bool {
  return local_document_path(path) == "tsugumi.lock";
}

auto external_document_source(const std::string& path, std::string bytes,
                              bool canonicalize)
    -> SourceEntry {
  const auto local = local_document_path(path);
  if (local != "layout.bin" && !text::valid_utf8(bytes))
    throw DocumentError("invalid_document", path + ": expected UTF-8 text",
                        {{"path", path}});
  auto check = [&](ParsedDocument parsed) {
    if (canonicalize) bytes = std::move(parsed.text);
  };
  if (local == "tsugumi.lock") parse_project_lock(path, bytes);
  if (local == "tsugumi.json") check(parse_external_project(path, bytes));
  if (local.ends_with("/_card.json"))
    check(parse_card_json(stdfs::path(path).parent_path().filename().string(),
                          bytes, path));
  if (local.ends_with("/_comments.json"))
    check(parse_comments_json(path, bytes));
  if (local.ends_with("/README.md")) check_readme(path, bytes);
  if (local == "layout.bin") check_layout(path, bytes);
  return {crypto::sha256_hex(bytes), bytes.size(),
          std::make_shared<const std::string>(std::move(bytes))};
}

auto note_source(const std::string& path, std::string bytes) -> SourceEntry {
  if (!text::valid_utf8(bytes))
    throw DocumentError("invalid_document", path + ": expected UTF-8 Markdown",
                        {{"path", path}});
  return {crypto::sha256_hex(bytes), bytes.size(),
          std::make_shared<const std::string>(std::move(bytes))};
}

auto Document::check_document_size() const -> void {
  size_t json_bytes = project_json.size() + layout.size();
  for (const auto& [_, definition] : definitions)
    json_bytes += definition.card_json.size() +
                  (definition.readme ? definition.readme->size() : 0) +
                  (definition.comments_json ? definition.comments_json->size()
                                            : 0);
  for (const auto& [_, source] : sources)
    if (source.document) json_bytes += source.size;
  // Snapshot and save replies must leave room in the RPC frame for manifests,
  // graph resolution and operation metadata in addition to document text.
  if (json_bytes > kMaxDocumentJsonBytes)
    throw DocumentError(
        "resource_limit",
        "the documents have " + std::to_string(json_bytes) +
            " bytes; limit is 8388608 bytes (8 MiB). Keep data in an attached "
            "folder and read it through a grants.hostPaths entry.");
}

bool Document::is_source_area(const std::string& rel) {
  return classify_source_path(rel) == SourcePath::Source;
}

auto Document::classify_source_path(const std::string& rel) -> SourcePath {
  if (!fs::valid_relative_path(rel)) return SourcePath::Invalid;
  if (is_document_file(rel) || is_lock_file(rel) ||
      rel.starts_with(".tsugumi/") || rel == ".tsugumi" ||
      rel.find(".tsugumi-tmp-") != std::string::npos)
    return SourcePath::Protected;
  if (rel.starts_with("definitions/")) {
    auto rest = rel.substr(12);
    auto slash = rest.find('/');
    if (slash != std::string::npos && slash > 0 &&
        valid_name(rest.substr(0, slash)))
      return SourcePath::Source;
  }
  // Nested projects are sources, excluding their runtime state.
  if (rel.starts_with("externals/") &&
      rel.find("/.tsugumi/") == std::string::npos &&
      !rel.ends_with("/.tsugumi"))
    return SourcePath::Source;
  if (rel.starts_with("templates/") && valid_version_path(rel))
    return SourcePath::Source;
  if (is_note_file(rel)) return SourcePath::Source;
  return rel.starts_with("skills/") ? SourcePath::Source
                                    : SourcePath::Protected;
}

auto Document::valid_version_path(const std::string& rel) -> bool {
  if (!fs::valid_relative_path(rel)) return false;
  for (const auto& part : stdfs::path(rel)) {
    auto name = part.string();
    for (auto& c : name)
      if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (name == ".git" || name == ".tsugumi") return false;
  }
  return true;
}

namespace {

namespace D = Tsugumi::Document;
using document_text::Schema;
using document_text::str;

[[noreturn]] void invalid(const std::string& message,
                          std::map<std::string, std::string> args = {}) {
  throw DocumentError("invalid_document", message, std::move(args));
}

auto parsed(Schema schema, const std::string& what, const std::string& text)
    -> ParsedDocument {
  auto buffer = document_text::parse(schema, what, text);
  auto canonical = document_text::print(schema, buffer);
  return {std::move(buffer), std::move(canonical)};
}

auto require_field(bool present, const std::string& where,
                   std::string_view field) -> void {
  if (!present)
    invalid(where + ": " + std::string(field) + " is required",
            {{"path", where}});
}

auto check_name(std::string_view name, const std::string& where) -> void {
  if (!valid_name(name))
    invalid(where + ": " + json::Quote(name) +
            " must match [a-z][a-z0-9_]{0,63}");
}

void check_graph_shape(const D::Graph& g, const std::string& where) {
  for (const auto* card : *g.cards())
    check_name(card->id()->string_view(), where + ".cards id");
  for (const auto* wire : *g.connections()) {
    for (const auto* end : {wire->from(), wire->to()}) {
      check_name(end->card()->string_view(), where + ".connections card");
      if (end->port())
        check_name(end->port()->string_view(), where + ".connections port");
    }
  }
}

void check_boundary(const D::Project& p, const std::string& where) {
  const auto* b = p.boundary();
  if (!b) return;
  if (where == "tsugumi.json")
    invalid(where + ": boundary is only allowed in an external project");
  for (const auto* side : {b->inputs(), b->outputs()}) {
    std::set<std::string> names;
    for (const auto* port : *side) {
      check_name(port->name()->string_view(), where + " boundary name");
      check_name(port->card()->string_view(), where + " boundary card");
      check_name(port->port()->string_view(), where + " boundary port");
      if (!names.insert(port->name()->str()).second)
        invalid(where + " boundary: duplicate port " + port->name()->str());
    }
  }
}

template <class Entries>
auto check_nix_path(const Entries* entries, const std::string& path) -> void {
  if (!entries) return;
  std::set<std::string> names;
  for (const auto* entry : *entries) {
    const auto name = entry->name()->str();
    if (!valid_nix_path_name(name))
      invalid(path + ": nixPath name \"" + name +
              "\" must match [A-Za-z0-9][A-Za-z0-9._-]*");
    if (!names.insert(name).second)
      invalid(path + ": duplicate nixPath name " + name);
    const auto url = entry->url()->string_view();
    if (url.empty() || url.find_first_of(" \t\r\n") != std::string_view::npos ||
        url.find('\0') != std::string_view::npos ||
        url.find(':') == std::string_view::npos)
      invalid(path + ": nixPath \"" + name + "\" needs a fetchTree URL");
  }
}

auto check_tracked_paths(const D::Project& p, const std::string& where) -> void {
  for (const auto* path : *p.trackedPaths())
    if (!Document::valid_version_path(path->str()))
      invalid(where + ": trackedPaths: expected a relative file or folder "
                      "outside .git and .tsugumi",
              {{"path", path->str()}});
}

auto check_log_limit(const D::Project& p, const std::string& where) -> void {
  if (const auto limit = p.logLimitBytes(); limit && *limit > kMaxLogLimitBytes)
    invalid(where + ": logLimitBytes must be between 0 and 1073741824",
            {{"path", where}});
}

}  // namespace

auto initial_project_json(const std::string& name,
                          const std::vector<std::string>& agent_exclude)
    -> std::string {
  std::string exclude;
  for (const auto& path : agent_exclude)
    exclude += (exclude.empty() ? "" : ",") + json::Quote(path);
  return parse_project_json(
             "{\"formatVersion\":" + std::to_string(kProjectFormatVersion) +
             ",\"name\":" + json::Quote(name) +
             R"(,"nixPath":[],"trackedPaths":[],"agentExclude":[)" + exclude +
             R"(],"recordLogs":true,"recordIO":false,"logLimitBytes":)" +
             std::to_string(kDefaultLogLimitBytes) +
             ",\"outputLimitBytes\":" +
             std::to_string(kDefaultOutputLimitBytes) +
             ",\"outputLimitEntries\":" +
             std::to_string(kDefaultOutputLimitEntries) +
             R"(,"canvas":{"cards":[],"connections":[]}})")
      .text;
}

auto valid_nix_path_name(std::string_view name) -> bool {
  const auto alnum = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
  };
  return !name.empty() && alnum(name.front()) &&
         std::ranges::all_of(name, [&](char c) {
           return alnum(c) || c == '.' || c == '_' || c == '-';
         });
}

auto parse_project_lock(const std::string& path, const std::string& text)
    -> ParsedDocument {
  auto buffer = document_text::parse(Schema::Lock, path, text);
  const auto& lock = document_text::root<D::ProjectLock>(buffer);
  check_nix_path(lock.entries(), path);
  for (const auto* entry : *lock.entries()) {
    const auto hash = entry->narHash()->string_view();
    if (!hash.starts_with("sha256-") || hash.size() <= 7)
      invalid(path + ": invalid narHash for " + entry->name()->str());
  }
  std::unique_ptr<D::ProjectLockT> object(lock.UnPack());
  std::ranges::sort(object->entries, {}, &D::LockEntryT::name);
  buffer = document_text::pack<D::ProjectLock>(*object);
  return {buffer, document_text::print(Schema::Lock, buffer)};
}

auto parse_project_json(const std::string& text) -> ParsedDocument {
  return parse_external_project("tsugumi.json", text);
}

auto parse_external_project(const std::string& rel, const std::string& text)
    -> ParsedDocument {
  auto doc = parsed(Schema::Project, rel, text);
  const auto& p = document_text::root<D::Project>(doc.buffer);
  require_field(p.formatVersion().has_value(), rel, "formatVersion");
  // The check is on the file format, not on the Tsugumi version that wrote the
  // project. Tsugumi is designed so that the same project gives the same
  // processing on any version, so the app version that ran it does not
  // matter; only the format of the files does. Checking the app version would
  // also force rewriting every demo, template and fixture on each release.
  if (*p.formatVersion() != kProjectFormatVersion)
    invalid(rel + " has project format " +
                std::to_string(*p.formatVersion()) + "; this Tsugumi reads " +
                "format " + std::to_string(kProjectFormatVersion) +
                "; there is no migration before v1.0",
            {{"path", rel}});
  require_field(p.recordLogs().has_value(), rel, "recordLogs");
  require_field(p.recordIO().has_value(), rel, "recordIO");
  require_field(p.logLimitBytes().has_value(), rel, "logLimitBytes");
  require_field(p.outputLimitBytes().has_value(), rel, "outputLimitBytes");
  require_field(p.outputLimitEntries().has_value(), rel, "outputLimitEntries");
  if (const auto* paths = p.hostPaths()) {
    std::set<std::string_view> names;
    for (const auto* path : *paths) {
      const auto name = path->name()->string_view();
      check_name(name, rel + " hostPaths name");
      if (!names.insert(name).second)
        invalid(rel + ": duplicate hostPaths name " + std::string(name));
    }
  }
  check_nix_path(p.nixPath(), rel);
  check_tracked_paths(p, rel);
  check_log_limit(p, rel);
  check_graph_shape(*p.canvas(), rel + " canvas");
  check_boundary(p, rel);
  return doc;
}

auto parse_card_json(const std::string& id, const std::string& text,
                     const std::string& path) -> ParsedDocument {
  const auto rel = path.empty() ? "definitions/" + id + "/_card.json" : path;
  check_name(id, rel + " Card id");
  json::Members fields;
  try {
    fields = json::Object(text);
  } catch (const json::DecodeFailure& error) {
    invalid(rel + ": " + error.what(), {{"path", rel}});
  }
  const auto type = json::Text(fields, "type");
  const bool executable = type == "code";
  const bool inputs = executable || type == "merge";
  const bool runs = inputs || type == "collect";
  std::set<std::string_view> required{"type", "symbol"};
  if (inputs) required.insert("inputs");
  if (runs) {
    required.insert("maxRuns");
    required.insert("recordIO");
  }
  if (executable)
    required.insert({"reuseWorker", "timeoutSeconds", "buildTimeoutSeconds",
                     "recordLogs", "sandbox", "grants", "systems"});
  for (const auto field : required)
    require_field(json::Get(fields, field) != "null", rel, field);
  for (const auto& [field, _] : fields)
    if (!required.contains(field) && !(executable && field == "secrets"))
      invalid(rel + ": " + field + " is not allowed on type " + type,
              {{"path", rel}});

  const auto check_record = [&](const char* name) {
    const auto setting = json::Text(fields, name);
    if (setting != "project" && setting != "on" && setting != "off")
      invalid(rel + ": " + name + " must be project, on or off");
  };
  const auto check_uint = [&](const char* name) {
    const auto& raw = fields.at(name);
    uint64_t value;
    const auto end = raw.data() + raw.size();
    const auto result = std::from_chars(raw.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end)
      invalid(rel + ": " + name + " must be an unsigned integer");
  };
  if (runs) {
    check_uint("maxRuns");
    check_record("recordIO");
  }
  if (executable) {
    check_uint("timeoutSeconds");
    check_uint("buildTimeoutSeconds");
    check_record("recordLogs");
  }

  auto doc = parsed(Schema::Card, rel, text);
  const auto& card = document_text::root<D::Card>(doc.buffer);
  if (type != D::EnumNameCardType(*card.type()))
    invalid(rel + ": type must be a Card type name");
  if (runs && *card.maxRuns() == 0)
    invalid(rel + ": maxRuns must be at least 1");
  if (inputs) {
    if (type == "merge" && card.inputs()->size() == 0)
      invalid(rel + ": merge inputs must contain at least one port");
    std::set<std::string_view> names;
    for (const auto* entry : *card.inputs()) {
      const auto name = entry->name()->string_view();
      check_name(name, rel + " inputs name");
      if (!names.insert(name).second)
        invalid(rel + ": duplicate inputs name " + std::string(name));
    }
  }
  if (executable) {
    const auto& sandbox = *card.sandbox();
    require_field(sandbox.filesystem().has_value(), rel, "sandbox.filesystem");
    require_field(sandbox.pid().has_value(), rel, "sandbox.pid");
    require_field(sandbox.ipc().has_value(), rel, "sandbox.ipc");
    require_field(sandbox.identity().has_value(), rel, "sandbox.identity");
    require_field(sandbox.limits(), rel, "sandbox.limits");
    const auto& limits = *sandbox.limits();
    require_field(limits.cpu().has_value(), rel, "sandbox.limits.cpu");
    require_field(limits.memoryMiB().has_value(), rel,
                  "sandbox.limits.memoryMiB");
    if (!std::isfinite(*limits.cpu()) || *limits.cpu() < 0 ||
        *limits.cpu() > 256)
      invalid(rel + ": sandbox.limits.cpu must be finite and in [0, 256]");
    if (*limits.memoryMiB() > 1048576)
      invalid(rel + ": sandbox.limits.memoryMiB must be in [0, 1048576]");
    const auto& grants = *card.grants();
    require_field(grants.network().has_value(), rel, "grants.network");
    require_field(grants.gui().has_value(), rel, "grants.gui");
    if (const auto* paths = grants.hostPaths()) {
      std::set<std::string> variables;
      for (const auto* folder : *paths) {
        require_field(folder->write().has_value(), rel,
                      "grants.hostPaths.write");
        const auto name = folder->name()->str();
        check_name(name, rel + " grants.hostPaths name");
        if (!variables.insert(name).second)
          invalid(rel + ": duplicate grants.hostPaths name " + name);
      }
    }
    if (card.systems()->size() == 0)
      invalid(rel + ": systems needs at least one entry", {{"path", rel}});
    std::set<std::string_view> systems;
    for (const auto* entry : *card.systems()) {
      const auto system = entry->string_view();
      if (system != "x86_64-linux" && system != "aarch64-linux" &&
          system != "x86_64-darwin" && system != "aarch64-darwin")
        invalid(rel + ": systems entries must be x86_64-linux, aarch64-linux, "
                      "x86_64-darwin or aarch64-darwin",
                {{"path", rel}});
      if (!systems.insert(system).second)
        invalid(rel + ": duplicate systems entry " + std::string(system),
                {{"path", rel}});
    }
    if (const auto* secrets = card.secrets()) {
      std::set<std::string_view> names;
      for (const auto* entry : *secrets) {
        const auto name = entry->string_view();
        check_name(name, rel + " secrets name");
        if (!names.insert(name).second)
          invalid(rel + ": duplicate secrets name " + std::string(name));
      }
    }
  }
  return doc;
}

auto parse_comments_json(const std::string& rel, const std::string& text)
    -> ParsedDocument {
  if (text.size() > (1u << 20)) invalid(rel + " exceeds 1 MiB", {{"path", rel}});
  auto doc = parsed(Schema::Comments, rel, text);
  std::set<std::string> ids;
  for (const auto* c :
       *document_text::root<D::Comments>(doc.buffer).comments())
    if (c->id() && !ids.insert(c->id()->str()).second)
      invalid(rel + ": duplicate comment id " + c->id()->str(),
              {{"path", rel}});
  return doc;
}

void check_readme(const std::string& rel, const std::string& text) {
  if (text.size() > (1u << 20)) invalid(rel + " exceeds 1 MiB", {{"path", rel}});
  if (!text::valid_utf8(text))
    invalid(rel + ": expected UTF-8 text", {{"path", rel}});
  std::string error;
  if (!readme::parse(text, &error))
    invalid(rel + ": " + error, {{"path", rel}});
}

void check_layout(const std::string& rel, const std::string& bytes) {
  if (!bytes.empty() &&
      !document_text::verify(Schema::Layout, bytes))
    invalid(rel + ": not a layout FlatBuffer", {{"path", rel}});
}

namespace {

// Disk loading also sees empty directories. Transactions validate the final
// file manifest after all document edits have been applied.
auto folders_at(const Document& doc, const stdfs::path* root,
                const std::string& prefix) -> std::set<std::string> {
  std::set<std::string> folders;
  for (auto it = doc.sources.lower_bound(prefix);
       it != doc.sources.end() && it->first.starts_with(prefix); ++it) {
    const auto rest = std::string_view(it->first).substr(prefix.size());
    if (const auto slash = rest.find('/'); slash != std::string_view::npos)
      folders.emplace(rest.substr(0, slash));
  }
  if (root) {
    const auto path = *root / prefix;
    std::error_code ec;
    if (stdfs::is_directory(path, ec))
      for (const auto& entry : stdfs::directory_iterator(path))
        if (entry.is_directory())
          folders.insert(entry.path().filename().string());
  }
  return folders;
}

auto check_scope(const Document& doc, const D::Project& project,
                 const std::string& prefix, const stdfs::path* root) -> void {
  const auto& graph = *project.canvas();
  const auto definitions = prefix + "definitions/";
  const auto externals = prefix + "externals/";
  std::map<std::string, D::CardType> cards;
  std::set<std::string_view> host_paths;
  if (const auto* paths = project.hostPaths())
    for (const auto* path : *paths)
      host_paths.insert(path->name()->string_view());
  const auto check_host_paths = [&](const D::Card& definition,
                                    const std::string& dir) {
    if (const auto* grants = definition.grants())
      if (const auto* paths = grants->hostPaths())
        for (const auto* path : *paths)
          if (!host_paths.contains(path->name()->string_view()))
            invalid(dir + "_card.json: unknown host path '" +
                    path->name()->str() + "' in " + prefix + "tsugumi.json");
  };
  int triggers = 0;
  for (const auto* card : *graph.cards()) {
    const auto id = card->id()->str();
    if (cards.contains(id))
      invalid(prefix + "duplicate card id " + id, {{"card", id}});
    const auto dir = definitions + id + "/";
    D::CardType type;
    if (prefix.empty()) {
      const auto it = doc.definitions.find(id);
      if (it == doc.definitions.end())
        invalid("card " + id + " has no definition (" + dir + ")",
                {{"card", id}, {"definition", id}});
      if (!it->second.readme) invalid(dir + "README.md is missing");
      type = *it->second.card().type();
      check_host_paths(it->second.card(), dir);
    } else {
      const auto it = doc.sources.find(dir + "_card.json");
      if (it == doc.sources.end() || !it->second.document)
        invalid("card " + id + " has no definition (" + dir + "_card.json)",
                {{"card", id}, {"definition", id}});
      if (!doc.sources.contains(dir + "README.md"))
        invalid(dir + "README.md is missing");
      const auto parsed =
          parse_card_json(id, *it->second.document, dir + "_card.json");
      const auto& definition = document_text::root<D::Card>(parsed.buffer);
      type = *definition.type();
      check_host_paths(definition, dir);
    }
    cards.emplace(id, type);
    if (type == D::CardType::trigger) ++triggers;
  }
  if (triggers > 1)
    invalid(prefix + "the canvas has more than one trigger card");
  auto definition_folders = folders_at(doc, root, definitions);
  if (prefix.empty())
    for (const auto& [id, _] : doc.definitions) definition_folders.insert(id);
  for (const auto& id : definition_folders) {
    check_name(id, definitions + " folder name");
    if (!cards.contains(id)) invalid(definitions + id + "/ has no canvas Card");
  }
  for (const auto* c : *graph.connections()) {
    for (const auto* ep : {c->from(), c->to()}) {
      const auto card = ep->card()->str();
      if (!cards.contains(card))
        invalid(prefix + "connection refers to unknown card " + card,
                {{"card", card}});
      // A Draft has no named ports: a wire ends on the Draft itself.
      const bool draft = cards.at(card) == D::CardType::draft;
      if (draft && ep->port())
        invalid(prefix + "a connection end on Draft " + card +
                    " must not name a port",
                {{"card", card}});
      if (!draft && !ep->port())
        invalid(prefix + "a connection end on " + card + " needs a port",
                {{"card", card}});
    }
  }
  if (const auto* boundary = project.boundary())
    for (const auto* side : {boundary->inputs(), boundary->outputs()})
      for (const auto* port : *side) {
        const auto it = cards.find(port->card()->str());
        if (it != cards.end() && it->second == D::CardType::draft)
          invalid(prefix + "boundary " + port->name()->str() +
                      " is on Draft " + it->first +
                      ", which has no ports",
                  {{"card", it->first}});
      }
  for (const auto& id : folders_at(doc, root, externals)) {
    check_name(id, externals + " folder name");
    if (!cards.contains(id) || cards.at(id) != D::CardType::subgraph)
      invalid(externals + id + "/ has no subgraph Card");
  }
  for (const auto& [id, type] : cards) {
    if (type != D::CardType::subgraph) continue;
    const auto child = externals + id + "/";
    const auto path = child + "tsugumi.json";
    const auto entry = doc.sources.find(path);
    if (entry == doc.sources.end() || !entry->second.document)
      invalid(path + " is missing");
    const auto parsed = parse_external_project(path, *entry->second.document);
    check_scope(doc, document_text::root<D::Project>(parsed.buffer), child,
                root);
  }
}

}  // namespace

void check_structure(const Document& doc) {
  check_scope(doc, doc.project(), "", nullptr);
}

namespace {
// What of a project or Card document identifies a run: its text without the
// presentation fields (a project's name and Agent exclusions).
auto execution_text(Schema schema, const std::string& buffer) -> std::string {
  if (schema == Schema::Project) {
    auto fields = json::Object(document_text::print(schema, buffer));
    fields.erase("name");
    fields.erase("agentExclude");
    fields.erase(
        "hostPaths");  // descriptions and declarations do not affect a run
    return json::Encode(fields);
  }
  return document_text::print(schema, buffer);
}
}  // namespace

std::string execution_fingerprint(const Document& doc) {
  crypto::Sha256 h;
  h.update(execution_text(Schema::Project, doc.project_buffer));
  for (const auto& [id, def] : doc.definitions) {
    h.update("\0def\0", 5);
    h.update(id);
    h.update(execution_text(Schema::Card, def.card_buffer));
  }
  for (const auto& [path, src] : doc.sources) {
    const auto relative = local_document_path(path);
    // Presentation never identifies a run: an external project's layout,
    // notes and Card READMEs and comments.
    if (path.starts_with("skills/") || path.starts_with("templates/") ||
        relative.starts_with("templates/") ||
        Document::is_card_note_file(path) || Document::is_note_file(path) ||
        relative == "layout.bin" || Document::is_generated_input_file(path))
      continue;
    h.update("\0src\0", 5);
    h.update(path);
    if (src.document && relative == "tsugumi.json")
      h.update(execution_text(Schema::Project,
                              document_text::parse(Schema::Project, path,
                                                   *src.document)));
    else if (src.document && relative.ends_with("/_card.json"))
      h.update(execution_text(
          Schema::Card,
          document_text::parse(Schema::Card, path, *src.document)));
    else
      h.update(src.hash);
  }
  return h.finish_hex();
}

std::shared_ptr<Document> load_from_disk(const stdfs::path& root) {
  auto doc = std::make_shared<Document>();
  size_t remaining = kMaxDocumentJsonBytes;
  const auto consume = [&](const std::string& path, size_t bytes) {
    if (bytes > remaining)
      throw DocumentError(
          "resource_limit",
          path + " exceeds the remaining 8 MiB document budget",
          {{"path", path}});
    remaining -= bytes;
  };
  auto project_text = fs::read_file(root / "tsugumi.json");
  if (!project_text)
    throw DocumentError("invalid_document", "tsugumi.json is missing");
  consume("tsugumi.json", project_text->size());
  auto project = parse_project_json(*project_text);
  doc->project_buffer = std::move(project.buffer);
  doc->project_json = std::move(project.text);
  doc->disk_hashes["tsugumi.json"] = crypto::sha256_hex(*project_text);
  if (std::error_code error;
      stdfs::exists(stdfs::symlink_status(root / "tsugumi.lock", error))) {
    std::string bytes;
    fs::stream_beneath(root, "tsugumi.lock", UINT64_MAX,
                       [&](std::string_view chunk) {
                         consume("tsugumi.lock", chunk.size());
                         bytes.append(chunk);
                       });
    doc->sources.emplace(
        "tsugumi.lock", external_document_source("tsugumi.lock", std::move(bytes)));
  }

  if (auto layout = fs::read_file(root / "layout.bin")) {
    consume("layout.bin", layout->size());
    check_layout("layout.bin", *layout);
    doc->disk_hashes["layout.bin"] = crypto::sha256_hex(*layout);
    doc->layout = std::move(*layout);
  }

  std::error_code ec;
  if (stdfs::is_directory(root / "definitions", ec)) {
    for (const auto& entry : stdfs::directory_iterator(root / "definitions")) {
      if (entry.is_symlink())
        throw DocumentError("invalid_path", "definitions may not be symlinks");
      if (!entry.is_directory()) continue;
      std::string id = entry.path().filename().string();
      if (!valid_name(id))
        throw DocumentError("invalid_document", "invalid definition id " + id);
      auto card_text = fs::read_file(entry.path() / "_card.json");
      if (!card_text)
        throw DocumentError("invalid_document",
                            "definitions/" + id + " needs _card.json");
      const auto prefix = "definitions/" + id + '/';
      consume(prefix + "_card.json", card_text->size());
      DefinitionDoc d;
      auto card = parse_card_json(id, *card_text);
      d.card_buffer = std::move(card.buffer);
      d.card_json = std::move(card.text);
      doc->disk_hashes[prefix + "_card.json"] = crypto::sha256_hex(*card_text);
      if (auto readme = fs::read_file(entry.path() / "README.md")) {
        consume(prefix + "README.md", readme->size());
        check_readme(prefix + "README.md", *readme);
        doc->disk_hashes[prefix + "README.md"] = crypto::sha256_hex(*readme);
        d.readme = std::move(*readme);
      }
      if (auto comments = fs::read_file(entry.path() / "_comments.json")) {
        consume(prefix + "_comments.json", comments->size());
        d.comments_json =
            parse_comments_json(prefix + "_comments.json", *comments).text;
        doc->disk_hashes[prefix + "_comments.json"] =
            crypto::sha256_hex(*comments);
      }
      doc->definitions.emplace(id, std::move(d));
    }
  }
  for (const char* area :
       {"definitions", "skills", "externals", "templates", "notes"}) {
    auto listing = fs::list_tree(root / area);
    if (!listing.links.empty())
      throw DocumentError(
          "invalid_path",
          std::string(area) + "/" + listing.links.front() +
              " is a symlink; links are not allowed",
          {{"path", std::string(area) + "/" + listing.links.front()}});
    for (const auto& f : listing.files) {
      std::string rel = std::string(area) + "/" + f;
      if (!Document::is_source_area(rel) && !Document::is_lock_file(rel))
        continue;
      const bool external = Document::is_external_document_file(rel);
      if (external || Document::is_note_file(rel)) {
        std::string bytes;
        fs::stream_beneath(root, rel, UINT64_MAX, [&](std::string_view chunk) {
          consume(rel, chunk.size());
          bytes.append(chunk);
        });
        doc->sources.emplace(rel, external ? external_document_source(
                                                 rel, std::move(bytes))
                                           : note_source(rel, std::move(bytes)));
        continue;
      }
      crypto::Sha256 hash;
      const auto size = fs::stream_beneath(
          root, rel, UINT64_MAX,
          [&](std::string_view chunk) { hash.update(chunk); });
      doc->sources[rel] = SourceEntry{hash.finish_hex(), size};
    }
  }
  check_scope(*doc, doc->project(), "", &root);
  doc->execution_fingerprint = execution_fingerprint(*doc);
  return doc;
}

}  // namespace tsugumi
