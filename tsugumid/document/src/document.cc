#include "tsugumi/document/document.hh"

#include <regex>
#include <set>

#include "tsugumi/document/graph.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
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

auto Document::is_card_meta_file(const std::string& path) -> bool {
  const auto rel = local_document_path(path);
  return rel.ends_with("/_meta.json") && is_document_file(std::string(rel));
}

bool Document::is_document_file(const std::string& rel) {
  if (rel == "tsugumi.json" || rel == "layout.json") return true;
  if (!rel.starts_with("definitions/")) return false;
  auto rest = rel.substr(12);
  auto slash = rest.find('/');
  if (slash == std::string::npos) return false;
  auto file = rest.substr(slash + 1);
  return file == "_card.json" || file == "_meta.json";
}

auto Document::is_external_document_file(std::string_view path) -> bool {
  return path.starts_with("externals/") &&
         is_document_file(std::string(local_document_path(path)));
}

auto external_document_source(const std::string& path, std::string bytes)
    -> SourceEntry {
  if (!text::valid_utf8(bytes))
    throw DocumentError("invalid_document", path + ": expected UTF-8 JSON text",
                        {{"path", path}});
  const auto local = local_document_path(path);
  if (local.ends_with("/_card.json"))
    parse_card_json(stdfs::path(path).parent_path().filename().string(), bytes);
  if (local.ends_with("/_meta.json")) check_meta_json(bytes);
  return {crypto::sha256_hex(bytes), bytes.size(),
          std::make_shared<const std::string>(std::move(bytes))};
}

auto Document::check_document_size() const -> void {
  size_t json_bytes = project_json.size() + layout_json.size();
  for (const auto& [_, definition] : definitions)
    json_bytes += definition.card_json.size() +
                  (definition.meta_json ? definition.meta_json->size() : 0);
  for (const auto& [_, source] : sources)
    if (source.document_json) json_bytes += source.size;
  // Snapshot and save replies must leave room in the RPC frame for manifests,
  // graph resolution and operation metadata in addition to document text.
  if (json_bytes > kMaxDocumentJsonBytes)
    throw DocumentError(
        "resource_limit",
        "document JSON has " + std::to_string(json_bytes) +
            " bytes; limit is 8388608 bytes (8 MiB). Keep data in an attached "
            "folder and read it through hostFolders (/host/<name>).");
}

bool Document::is_source_area(const std::string& rel) {
  return classify_source_path(rel) == SourcePath::Source;
}

auto Document::classify_source_path(const std::string& rel) -> SourcePath {
  if (!fs::valid_relative_path(rel)) return SourcePath::Invalid;
  if (is_document_file(rel) || rel.starts_with(".tsugumi/") ||
      rel == ".tsugumi" || rel.find(".tsugumi-tmp-") != std::string::npos)
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

[[noreturn]] void invalid(const std::string& message,
                          std::map<std::string, std::string> args = {}) {
  throw DocumentError("invalid_document", message, std::move(args));
}

template <class S>
S decode_doc(const std::string& what, const std::string& text) {
  S out;
  if (auto err = schema::decode(text, out))
    throw DocumentError(
        "invalid_document",
        what + ": " + err->message + " at byte " + std::to_string(err->offset),
        {{"path", what}, {"offset", std::to_string(err->offset)}});
  return out;
}

void check_endpoint(const schema::Endpoint& ep, const std::string& where) {
  if (!ep.card->has_value() || !ep.port->has_value())
    invalid(where + " needs card and port");
}

void check_graph_shape(const schema::Graph& g, const std::string& where) {
  if (!g.cards->has_value()) invalid(where + ": \"cards\" is required");
  if (!g.connections->has_value())
    invalid(where + ": \"connections\" is required");
  const auto& cards = g.cards->value();
  for (size_t i = 0; i < cards.size(); ++i)
    if (!cards[i].id->has_value() || !valid_id(cards[i].id->value()))
      invalid(where + ".cards[" + std::to_string(i) +
              "]: card id must match [A-Za-z0-9_.-]{1,128}");
  const auto& conns = g.connections->value();
  for (size_t i = 0; i < conns.size(); ++i) {
    std::string w = where + ".connections[" + std::to_string(i) + "]";
    if (!conns[i].from->has_value() || !conns[i].to->has_value())
      invalid(w + " needs from and to");
    check_endpoint(conns[i].from->value(), w + ".from");
    check_endpoint(conns[i].to->value(), w + ".to");
  }
}

void check_boundary(const schema::Project& p, const std::string& where) {
  if (const auto* b = schema::get(p.boundary)) {
    if (const auto* ins = schema::get(b->inputs))
      for (const auto& [name, ep] : ins->entries())
        check_endpoint(ep, where + " boundary.inputs." + name);
    if (const auto* outs = schema::get(b->outputs))
      for (const auto& [name, ep] : outs->entries())
        check_endpoint(ep, where + " boundary.outputs." + name);
  }
}

auto check_nix_sources(const schema::Project& p) -> void {
  if (p.nix_sources->has_value()) {
    static const std::regex name_re(R"(^[A-Za-z_][A-Za-z0-9_-]*$)");
    static const std::regex ref_re(
        R"(^(github:|gitlab:|sourcehut:|git\+https://|https://)[^\s"\\$]*[?&]narHash=sha256-[A-Za-z0-9%+/=]+.*$)");
    std::set<std::string> sources;
    for (const auto& s : p.nix_sources->value()) {
      if (!s.name->has_value() || !s.type->has_value() || !s.ref->has_value())
        invalid("nixSources entries need name, type and ref");
      const auto& name = s.name->value();
      if (!std::regex_match(name, name_re))
        invalid("nixSources: \"" + name + "\" is not a name");
      if (s.type->value() != "flake")
        invalid("nixSources: " + name + ": type must be flake");
      if (!std::regex_match(s.ref->value(), ref_re))
        invalid("nixSources: " + name +
                ": ref must be a locked flake reference with a narHash");
      if (const auto* overrides = schema::get(s.overrides)) {
        schema::v::Mapping<schema::v::String,
                           schema::Obj<schema::ExternalPackageOverride>>
            checked;
        if (auto error = schema::decode(*overrides, checked))
          invalid("nixSources: " + name +
                  ": invalid overrides: " + error->message);
      }
      if (!sources.insert(name).second)
        invalid("nixSources: duplicate name " + name);
    }
  }
}

auto check_version_paths(const schema::Project& p) -> void {
  if (const auto* paths = schema::get(p.version_paths))
    for (const auto& path : *paths)
      if (!Document::valid_version_path(path))
        invalid(
            "versionPaths: expected a relative file or folder outside .git "
            "and .tsugumi",
            {{"path", path}});
}

}  // namespace

auto initial_project_json(const std::string& name) -> std::string {
  const std::string rev = TSUGUMI_CARD_NIXPKGS_REV;
  const std::string hash = TSUGUMI_CARD_NIXPKGS_NARHASH;
  if (rev.empty() || hash.empty())
    throw DocumentError("project_defaults_unavailable",
                        "This build has no default Card Nixpkgs pin");
  const auto display_name = schema::string_value(name);
  const auto pin = schema::string_value(
      "https://github.com/NixOS/nixpkgs/archive/" + rev + ".tar.gz#" + hash);
  const auto canvas = schema::capture(R"({"cards":[],"connections":[]})");
  return schema::object_text({{"name", &display_name},
                              {"pkgsPath", &pin},
                              {"canvas", &canvas}}) +
         '\n';
}

schema::Project parse_project_json(const std::string& text) {
  auto p = decode_doc<schema::Project>("tsugumi.json", text);
  if (!p.pkgs_path->has_value())
    invalid("tsugumi.json: \"pkgsPath\" is required");
  if (!p.canvas->has_value()) invalid("tsugumi.json: \"canvas\" is required");
  check_nix_sources(p);
  check_version_paths(p);
  check_graph_shape(p.canvas->value(), "tsugumi.json canvas");
  check_boundary(p, "tsugumi.json");
  return p;
}

schema::Project parse_external_project(const std::string& rel,
                                       const std::string& text) {
  auto p = decode_doc<schema::Project>(rel, text);
  check_nix_sources(p);
  check_version_paths(p);
  if (!p.canvas->has_value())
    invalid(rel + ": \"canvas\" is required", {{"path", rel}});
  check_graph_shape(p.canvas->value(), rel + " canvas");
  check_boundary(p, rel);
  return p;
}

schema::CardDefinition parse_card_json(const std::string& id,
                                       const std::string& text) {
  const auto rel = "definitions/" + id + "/_card.json";
  auto card = decode_doc<schema::CardDefinition>(rel, text);
  if (const auto* external = schema::get(card.external)) {
    if (!valid_id(*external))
      invalid(rel + ": \"external\" must name a folder of externals/",
              {{"path", rel}});
    const auto members =
        decode_doc<schema::v::Mapping<schema::v::String, schema::Json>>(rel,
                                                                        text);
    for (const auto& [field, value] : members.value())
      if (field != "external" && field != "displayMode")
        invalid(rel + ": execution field " + field + " on a Card in Card",
                {{"path", rel}});
  }
  return card;
}

void check_meta_json(const std::string& text) {
  if (text.size() > (1u << 20)) invalid("_meta.json exceeds 1 MiB");
  decode_doc<schema::v::Mapping<schema::v::String, schema::Json>>("_meta.json",
                                                                  text);
}

void check_structure(const Document& doc) {
  const auto& graph = doc.project.canvas->value();
  std::set<std::string> ids;
  int triggers = 0;
  for (const auto& card : graph.cards->value()) {
    const auto& id = card.id->value();
    if (!ids.insert(id).second)
      throw DocumentError("invalid_document", "duplicate card id " + id,
                          {{"card", id}});
    auto it = doc.definitions.find(id);
    if (it == doc.definitions.end())
      throw DocumentError(
          "invalid_document",
          "card " + id + " has no definition (definitions/" + id + "/)",
          {{"card", id}, {"definition", id}});
    if (template_of(&it->second) == Template::Trigger) ++triggers;
  }
  if (triggers > 1)
    throw DocumentError("invalid_document",
                        "the canvas has more than one trigger card");
  for (const auto& c : graph.connections->value()) {
    for (const auto* ep : {&c.from->value(), &c.to->value()}) {
      const auto& card = ep->card->value();
      if (!ids.count(card))
        throw DocumentError("invalid_document",
                            "connection refers to unknown card " + card,
                            {{"card", card}});
    }
  }
}

namespace {
// Project/Card documents are hashed by value: a client's JSON writer may
// reorder or reindent the rest while changing a presentation field.
auto execution_value(const hosq::json::GenericDom<>& value) -> std::string {
  auto raw = schema::text(value);
  auto first = raw.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  if (raw[first] == '{') {
    schema::v::Mapping<schema::v::String, schema::Json> fields;
    if (auto error = schema::decode(value, fields))
      throw DocumentError("invalid_document", error->message);
    std::map<std::string, std::string> sorted;
    for (const auto& [key, item] : fields.value())
      sorted[key] = execution_value(item);
    std::string out = "{";
    for (const auto& [key, item] : sorted)
      out += (out.size() > 1 ? "," : "") +
             schema::text(schema::string_value(key)) + ":" + item;
    return out + "}";
  }
  if (raw[first] == '[') {
    schema::v::Array<schema::Json> items;
    if (auto error = schema::decode(value, items))
      throw DocumentError("invalid_document", error->message);
    std::string out = "[";
    for (const auto& item : items.value())
      out += (out.size() > 1 ? "," : "") + execution_value(item);
    return out + "]";
  }
  if (raw[first] == '"') {
    schema::v::String text;
    if (auto error = schema::decode(value, text))
      throw DocumentError("invalid_document", error->message);
    return schema::encode(text);
  }
  return raw.substr(first, raw.find_last_not_of(" \t\r\n") - first + 1);
}
auto execution_json(const std::string& text, const char* omitted)
    -> std::string {
  schema::v::Mapping<schema::v::String, schema::Json> object;
  if (auto error = schema::decode(text, object))
    throw DocumentError("invalid_document", error->message);
  std::vector<std::pair<std::string, const hosq::json::GenericDom<>*>> fields;
  for (const auto& [key, value] : object.value())
    if (key != omitted &&
        !(std::string_view(omitted) == "name" && key == "agentExclude"))
      fields.emplace_back(key, &value);
  return execution_value(schema::capture(schema::object_text(fields)));
}
}  // namespace

std::string execution_fingerprint(const Document& doc) {
  crypto::Sha256 h;
  h.update(execution_json(doc.project_json, "name"));
  for (const auto& [id, def] : doc.definitions) {
    h.update("\0def\0", 5);
    h.update(id);
    h.update(execution_json(def.card_json, "displayMode"));
  }
  for (const auto& [path, src] : doc.sources) {
    if (path.starts_with("skills/") || Document::is_card_meta_file(path))
      continue;
    // External projects carry layout as a source file; it never identifies a
    // run.
    const auto relative = local_document_path(path);
    if (relative == "layout.json") continue;
    h.update("\0src\0", 5);
    h.update(path);
    if (src.document_json)
      h.update(execution_json(*src.document_json,
                              relative == "tsugumi.json" ? "name"
                                                         : "displayMode"));
    else
      h.update(src.hash);
  }
  return h.finish_hex();
}

std::shared_ptr<Document> load_from_disk(const stdfs::path& root) {
  auto doc = std::make_shared<Document>();
  auto project_text = fs::read_file(root / "tsugumi.json");
  if (!project_text)
    throw DocumentError("invalid_document", "tsugumi.json is missing");
  doc->project = parse_project_json(*project_text);
  doc->project_json = *project_text;
  doc->disk_hashes["tsugumi.json"] = crypto::sha256_hex(*project_text);

  auto layout_text = fs::read_file(root / "layout.json");
  doc->layout_json = layout_text ? *layout_text : kEmptyLayout;
  if (layout_text)
    doc->disk_hashes["layout.json"] = crypto::sha256_hex(*layout_text);

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
      DefinitionDoc d;
      d.card = parse_card_json(id, *card_text);
      d.card_json = *card_text;
      doc->disk_hashes["definitions/" + id + "/_card.json"] =
          crypto::sha256_hex(*card_text);
      if (auto meta = fs::read_file(entry.path() / "_meta.json")) {
        check_meta_json(*meta);
        d.meta_json = *meta;
        doc->disk_hashes["definitions/" + id + "/_meta.json"] =
            crypto::sha256_hex(*meta);
      }
      doc->definitions.emplace(id, std::move(d));
    }
  }
  for (const char* area : {"definitions", "skills", "externals"}) {
    auto listing = fs::list_tree(root / area);
    if (!listing.links.empty())
      throw DocumentError(
          "invalid_path",
          std::string(area) + "/" + listing.links.front() +
              " is a symlink; links are not allowed",
          {{"path", std::string(area) + "/" + listing.links.front()}});
    for (const auto& f : listing.files) {
      std::string rel = std::string(area) + "/" + f;
      if (!Document::is_source_area(rel)) continue;
      if (Document::is_external_document_file(rel)) {
        std::string bytes;
        fs::stream_beneath(
            root, rel, kMaxDocumentJsonBytes,
            [&](std::string_view chunk) { bytes.append(chunk); });
        doc->sources.emplace(rel,
                             external_document_source(rel, std::move(bytes)));
        continue;
      }
      crypto::Sha256 hash;
      const auto size = fs::stream_beneath(
          root, rel, UINT64_MAX,
          [&](std::string_view chunk) { hash.update(chunk); });
      doc->sources[rel] = SourceEntry{hash.finish_hex(), size};
    }
  }
  doc->check_document_size();
  check_structure(*doc);
  doc->execution_fingerprint = execution_fingerprint(*doc);
  return doc;
}

}  // namespace tsugumi
