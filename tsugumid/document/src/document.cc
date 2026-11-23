#include "tsugumi/document/document.hh"

#include <algorithm>
#include <cctype>
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

bool valid_secret_name(std::string_view name) {
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
    if (slash == std::string_view::npos) return {};
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
  if (slash == std::string::npos) return false;
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
  if (local == "tsugumi.lock")
    return lock_source_document(path, std::move(bytes));
  if (local != "layout.bin" && !text::valid_utf8(bytes))
    throw DocumentError("invalid_document", path + ": expected UTF-8 text",
                        {{"path", path}});
  auto check = [&](ParsedDocument parsed) {
    if (canonicalize) bytes = std::move(parsed.text);
  };
  if (local == "tsugumi.json") check(parse_external_project(path, bytes));
  if (local.ends_with("/_card.json"))
    check(parse_card_json(stdfs::path(path).parent_path().filename().string(),
                          bytes));
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
        valid_id(rest.substr(0, slash)))
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

void check_graph_shape(const D::Graph& g, const std::string& where) {
  for (size_t i = 0; i < g.cards()->size(); ++i)
    if (!valid_id(g.cards()->Get(i)->id()->str()))
      invalid(where + ".cards[" + std::to_string(i) +
              "]: card id must match [A-Za-z0-9_.-]{1,128}");
}

void check_boundary(const D::Project& p, const std::string& where) {
  const auto* b = p.boundary();
  if (!b) return;
  for (const auto* side : {b->inputs(), b->outputs()}) {
    if (!side) continue;
    std::set<std::string> names;
    for (const auto* port : *side)
      if (!names.insert(port->name()->str()).second)
        invalid(where + " boundary: duplicate port " + port->name()->str());
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

auto check_version_paths(const D::Project& p) -> void {
  if (const auto* paths = p.versionPaths())
    for (const auto* path : *paths)
      if (!Document::valid_version_path(path->str()))
        invalid(
            "versionPaths: expected a relative file or folder outside .git "
            "and .tsugumi",
            {{"path", path->str()}});
}

auto check_log_limit(const D::Project& p, const std::string& where) -> void {
  if (const auto limit = p.logLimitBytes(); limit && *limit > kMaxLogLimitBytes)
    invalid(where + ": logLimitBytes must be between 0 and 1073741824",
            {{"path", where}});
}

}  // namespace

auto initial_project_json(const std::string& name) -> std::string {
  return parse_project_json(
             "{\"name\":" + json::Quote(name) +
             R"(,"nixPath":[],"canvas":{"cards":[],"connections":[]}})")
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

auto supported_nix_path_url(std::string_view url) -> bool {
  return url.starts_with("github:") || url.starts_with("gitlab:") ||
         url.starts_with("sourcehut:");
}

auto parse_project_lock(const std::string& path, const std::string& text)
    -> ParsedDocument {
  auto buffer = document_text::parse(Schema::Lock, path, text);
  const auto& lock = document_text::root<D::ProjectLock>(buffer);
  for (const auto& [name, value] :
       json::Object(lock.nixPath()->string_view())) {
    if (!valid_nix_path_name(name))
      invalid(path + ": invalid nixPath name " + json::Quote(name));
    const auto url = json::String(value);
    const auto query = url.find('?');
    const auto source = std::string_view(url).substr(0, query);
    const auto rev = source.substr(source.rfind('/') + 1);
    if (!supported_nix_path_url(url) || std::ranges::count(source, '/') != 2 ||
        rev.size() != 40 ||
        !std::ranges::all_of(
            rev, [](unsigned char c) { return std::isxdigit(c); }) ||
        query == url.npos)
      invalid(path + ": invalid locked URL for " + json::Quote(name));
    bool hash = false;
    bool modified = false;
    auto options = std::string_view(url).substr(query + 1);
    while (!options.empty()) {
      const auto separator = options.find('&');
      const auto option = options.substr(0, separator);
      if (!hash && option.starts_with("narHash=sha256-") &&
          option.size() > std::string_view("narHash=sha256-").size()) {
        hash = true;
      } else if (!modified && option.starts_with("lastModified=")) {
        const auto timestamp =
            option.substr(std::string_view("lastModified=").size());
        if (timestamp.empty() || !std::ranges::all_of(timestamp, [](char c) {
              return c >= '0' && c <= '9';
            }))
          invalid(path + ": invalid lastModified for " + json::Quote(name));
        modified = true;
      } else {
        invalid(path + ": unexpected locked URL query for " +
                json::Quote(name));
      }
      if (separator == options.npos) break;
      options.remove_prefix(separator + 1);
    }
    if (!hash) invalid(path + ": missing narHash for " + json::Quote(name));
  }
  return {buffer, document_text::print(Schema::Lock, buffer)};
}

auto lock_source_document(const std::string& path, std::string bytes)
    -> SourceEntry {
  parse_project_lock(path, bytes);
  return {crypto::sha256_hex(bytes), bytes.size(),
          std::make_shared<const std::string>(std::move(bytes))};
}

auto parse_project_json(const std::string& text) -> ParsedDocument {
  auto doc = parsed(Schema::Project, "tsugumi.json", text);
  const auto& p = document_text::root<D::Project>(doc.buffer);
  check_nix_path(p.nixPath(), "tsugumi.json");
  check_version_paths(p);
  check_log_limit(p, "tsugumi.json");
  check_graph_shape(*p.canvas(), "tsugumi.json canvas");
  check_boundary(p, "tsugumi.json");
  return doc;
}

auto parse_external_project(const std::string& rel, const std::string& text)
    -> ParsedDocument {
  auto doc = parsed(Schema::Project, rel, text);
  const auto& p = document_text::root<D::Project>(doc.buffer);
  check_log_limit(p, rel);
  check_nix_path(p.nixPath(), rel);
  check_version_paths(p);
  check_graph_shape(*p.canvas(), rel + " canvas");
  check_boundary(p, rel);
  return doc;
}

auto parse_card_json(const std::string& id, const std::string& text)
    -> ParsedDocument {
  const auto rel = "definitions/" + id + "/_card.json";
  auto doc = parsed(Schema::Card, rel, text);
  const auto& card = document_text::root<D::Card>(doc.buffer);
  if (!card.type())
    invalid(rel +
                ": \"type\" is required (one of code, draft, trigger, input, "
                "branch, fork, merge, collect, subgraph)",
            {{"path", rel}});
  const auto* external = card.external();
  if ((*card.type() == D::CardType::subgraph) != (external != nullptr))
    invalid(rel + (external ? ": \"external\" needs \"type\": \"subgraph\""
                            : ": a subgraph needs \"external\""),
            {{"path", rel}});
  if (external) {
    if (!valid_id(external->str()))
      invalid(rel + ": \"external\" must name a folder of externals/",
              {{"path", rel}});
    // A Card in Card has only its kind, its project and its display mode.
    D::CardT rest;
    card.UnPackTo(&rest);
    rest.type.reset();
    rest.external.clear();
    rest.displayMode.clear();
    const auto fields = document_text::pack<D::Card>(rest);
    if (fields != document_text::pack<D::Card>(D::CardT{}))
      invalid(rel + ": execution fields on a Card in Card: " +
                  document_text::print(Schema::Card, fields),
              {{"path", rel}});
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

void check_structure(const Document& doc) {
  const auto& graph = *doc.project().canvas();
  std::set<std::string> ids;
  int triggers = 0;
  for (const auto* card : *graph.cards()) {
    const auto id = card->id()->str();
    if (!ids.insert(id).second)
      throw DocumentError("invalid_document", "duplicate card id " + id,
                          {{"card", id}});
    auto it = doc.definitions.find(id);
    if (it == doc.definitions.end())
      throw DocumentError(
          "invalid_document",
          "card " + id + " has no definition (definitions/" + id + "/)",
          {{"card", id}, {"definition", id}});
    if (card_type_of(it->second) == CardType::Trigger) ++triggers;
  }
  if (triggers > 1)
    throw DocumentError("invalid_document",
                        "the canvas has more than one trigger card");
  for (const auto* c : *graph.connections()) {
    for (const auto* ep : {c->from(), c->to()}) {
      const auto card = ep->card()->str();
      if (!ids.count(card))
        throw DocumentError("invalid_document",
                            "connection refers to unknown card " + card,
                            {{"card", card}});
    }
  }
}

namespace {
// What of a project or Card document identifies a run: its text without the
// presentation fields (a project's name and Agent exclusions, a Card's
// display mode).
auto execution_text(Schema schema, const std::string& buffer) -> std::string {
  if (schema == Schema::Project) {
    D::ProjectT p;
    document_text::root<D::Project>(buffer).UnPackTo(&p);
    p.name.clear();
    p.agentExclude.clear();
    return document_text::print(schema, document_text::pack<D::Project>(p));
  }
  D::CardT c;
  document_text::root<D::Card>(buffer).UnPackTo(&c);
  c.displayMode.clear();
  return document_text::print(schema, document_text::pack<D::Card>(c));
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
        relative == "layout.bin")
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
        "tsugumi.lock", lock_source_document("tsugumi.lock", std::move(bytes)));
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
      if (!valid_id(id))
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
  check_structure(*doc);
  doc->execution_fingerprint = execution_fingerprint(*doc);
  return doc;
}

}  // namespace tsugumi
