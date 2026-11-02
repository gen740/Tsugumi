#include "document.hpp"

#include <set>

#include "util/crypto.hpp"
#include "util/fs.hpp"

namespace tsugumi {

namespace stdfs = std::filesystem;

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
    p.push_back(key.substr(s, e == std::string::npos ? std::string::npos : e - s));
    if (e == std::string::npos) break;
    s = e + 1;
  }
  return p;
}

std::string path_display(const CardPath& p) { return p.empty() ? "<root>" : path_key(p); }

bool valid_id(const std::string& id) {
  if (id.empty() || id.size() > 128 || id == "." || id == "..") return false;
  for (char c : id) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.';
    if (!ok) return false;
  }
  return true;
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

bool Document::is_source_area(const std::string& rel) {
  if (!fs::valid_relative_path(rel)) return false;
  if (is_document_file(rel)) return false;
  if (rel.starts_with(".tsugumi/") || rel == ".tsugumi") return false;
  if (rel.find(".tsugumi-tmp-") != std::string::npos) return false;
  if (rel.starts_with("definitions/")) {
    auto rest = rel.substr(12);
    auto slash = rest.find('/');
    return slash != std::string::npos && slash > 0 && valid_id(rest.substr(0, slash));
  }
  return rel.starts_with("skills/") || rel.starts_with("graphs/");
}

namespace {

[[noreturn]] void invalid(const std::string& message, std::map<std::string, std::string> args = {}) {
  throw DocumentError("invalid_document", message, std::move(args));
}

template <class S>
S decode_doc(const std::string& what, const std::string& text) {
  S out;
  if (auto err = schema::decode(text, out))
    throw DocumentError("invalid_document", what + ": " + err->message + " at byte " + std::to_string(err->offset),
                        {{"path", what}, {"offset", std::to_string(err->offset)}});
  return out;
}

void check_endpoint(const schema::Endpoint& ep, const std::string& where) {
  if (!ep.card->has_value() || !ep.port->has_value() || ep.port->value().empty())
    invalid(where + " needs card and port");
}

void check_graph_shape(const schema::Graph& g, const std::string& where, int depth);

void check_card_shape(const schema::CardInstance& card, const std::string& where, int depth) {
  if (!card.id->has_value() || !valid_id(card.id->value()))
    invalid(where + ": card id must match [A-Za-z0-9_.-]{1,128}");
  if (card.canvas->has_value()) {
    if (depth >= 16) invalid(where + ": graph nesting deeper than 16");
    check_graph_shape(inner_graph(card), where + ".canvas", depth + 1);
  }
  if (const auto* proj = schema::get(card.project)) {
    if (!fs::valid_relative_path(*proj) || !proj->starts_with("graphs/"))
      invalid(where + ": project must be a graphs/ relative path");
  }
  if (const auto* b = schema::get(card.boundary)) {
    if (const auto* ins = schema::get(b->inputs))
      for (const auto& [name, ep] : ins->entries()) check_endpoint(ep, where + ".boundary.inputs." + name);
    if (const auto* outs = schema::get(b->outputs))
      for (const auto& [name, ep] : outs->entries()) check_endpoint(ep, where + ".boundary.outputs." + name);
  }
}

void check_graph_shape(const schema::Graph& g, const std::string& where, int depth) {
  if (!g.cards->has_value()) invalid(where + ": \"cards\" is required");
  if (!g.connections->has_value()) invalid(where + ": \"connections\" is required");
  const auto& cards = g.cards->value();
  for (size_t i = 0; i < cards.size(); ++i)
    check_card_shape(cards[i], where + ".cards[" + std::to_string(i) + "]", depth);
  const auto& conns = g.connections->value();
  for (size_t i = 0; i < conns.size(); ++i) {
    std::string w = where + ".connections[" + std::to_string(i) + "]";
    if (!conns[i].from->has_value() || !conns[i].to->has_value()) invalid(w + " needs from and to");
    check_endpoint(conns[i].from->value(), w + ".from");
    check_endpoint(conns[i].to->value(), w + ".to");
  }
}

CardPath child(const CardPath& scope, const std::string& id) {
  CardPath p = scope;
  p.push_back(id);
  return p;
}

const std::string* template_of(const Document& doc, const std::string& def_id) {
  auto it = doc.definitions.find(def_id);
  if (it == doc.definitions.end()) return nullptr;
  return schema::get(it->second.card.template_name);
}

// Checks ids and connections of one scope (and nested inline scopes).
void check_scope(const Document& doc, const schema::Graph& graph, const CardPath& scope,
                 std::set<std::string>& graph_files) {
  std::set<std::string> ids;
  int triggers = 0;
  for (const auto& card : graph.cards->value()) {
    const auto& id = card.id->value();
    CardPath path = child(scope, id);
    if (!ids.insert(id).second)
      throw DocumentError("invalid_document", "duplicate card id " + id + " in " + path_display(scope),
                          {{"card", path_key(path)}});
    if (const auto* def = schema::get(card.definition)) {
      if (!doc.definitions.count(*def))
        throw DocumentError("invalid_document",
                            "card " + path_key(path) + " references definition " + *def + ", which does not exist",
                            {{"card", path_key(path)}, {"definition", *def}});
      if (const auto* t = template_of(doc, *def); t && *t == "trigger") ++triggers;
    }
    if (card.canvas->has_value() && card.project->has_value())
      invalid("card " + path_key(path) + " cannot have both canvas and project", {{"card", path_key(path)}});
    if (card.canvas->has_value()) check_scope(doc, inner_graph(card), path, graph_files);
    if (const auto* proj = schema::get(card.project)) graph_files.insert(*proj);
  }
  if (triggers > 1)
    throw DocumentError("invalid_document", "graph " + path_display(scope) + " has more than one trigger card");
  for (const auto& c : graph.connections->value()) {
    for (const auto* ep : {&c.from->value(), &c.to->value()}) {
      const auto& card = ep->card->value();
      if (!ids.count(card))
        throw DocumentError("invalid_document",
                            "connection refers to unknown card " + card + " in " + path_display(scope),
                            {{"card", path_key(child(scope, card))}});
    }
  }
}

}  // namespace

schema::Graph inner_graph(const schema::CardInstance& card) {
  schema::Graph g;
  if (auto err = schema::decode(card.canvas->value(), g)) invalid("canvas: " + err->message);
  return g;
}

schema::Graph parse_graph_file(const std::string& rel, const std::string& text) {
  auto g = decode_doc<schema::Graph>(rel, text);
  check_graph_shape(g, rel, 1);
  return g;
}

schema::Project parse_project_json(const std::string& text) {
  auto p = decode_doc<schema::Project>("tsugumi.json", text);
  if (!p.name->has_value()) invalid("tsugumi.json: \"name\" is required");
  if (!p.pkgs_path->has_value()) invalid("tsugumi.json: \"pkgsPath\" is required");
  if (!p.database_connections->has_value()) invalid("tsugumi.json: \"databaseConnections\" is required");
  if (!p.canvas->has_value()) invalid("tsugumi.json: \"canvas\" is required");
  std::set<std::string> names;
  for (const auto& db : p.database_connections->value()) {
    if (!db.name->has_value() || !db.url->has_value()) invalid("databaseConnections entries need name and url");
    if (!names.insert(db.name->value()).second) invalid("duplicate database connection " + db.name->value());
  }
  check_graph_shape(p.canvas->value(), "tsugumi.json canvas", 0);
  return p;
}

schema::Layout parse_layout_json(const std::string& text) {
  auto l = decode_doc<schema::Layout>("layout.json", text);
  if (const auto* cards = schema::get(l.cards))
    for (const auto& [key, pos] : cards->entries())
      if (!pos.x->has_value() || !pos.y->has_value()) invalid("layout.json: card " + key + " needs x and y");
  if (const auto* notes = schema::get(l.notes))
    for (const auto& n : *notes)
      if (!n.id->has_value() || !n.text->has_value() || !n.x->has_value() || !n.y->has_value())
        invalid("layout.json: notes need id, text, x and y");
  if (const auto* displays = schema::get(l.displays))
    for (const auto& [key, d] : displays->entries())
      if (!d.x->has_value() || !d.y->has_value() || !d.width->has_value() || !d.height->has_value())
        invalid("layout.json: display " + key + " needs x, y, width and height");
  return l;
}

schema::CardDefinition parse_card_json(const std::string& id, const std::string& text) {
  return decode_doc<schema::CardDefinition>("definitions/" + id + "/_card.json", text);
}

schema::CardMeta parse_meta_json(const std::string& text) { return decode_doc<schema::CardMeta>("_meta.json", text); }

void check_structure(const Document& doc) {
  std::set<std::string> graph_files;
  check_scope(doc, doc.project.canvas->value(), {}, graph_files);
  for (const auto& gf : graph_files)
    if (!doc.sources.count(gf))
      throw DocumentError("invalid_document", "subgraph project file " + gf + " does not exist", {{"path", gf}});
}

std::string execution_fingerprint(const Document& doc) {
  crypto::Sha256 h;
  h.update(doc.project_json);
  for (const auto& [id, def] : doc.definitions) {
    h.update("\0def\0", 5);
    h.update(id);
    h.update(def.card_json);
  }
  for (const auto& [path, src] : doc.sources) {
    if (path.starts_with("skills/")) continue;
    h.update("\0src\0", 5);
    h.update(path);
    h.update(src.hash);
  }
  return h.finish_hex();
}

std::shared_ptr<Document> load_from_disk(const stdfs::path& root) {
  auto doc = std::make_shared<Document>();
  auto project_text = fs::read_file(root / "tsugumi.json");
  if (!project_text) throw DocumentError("invalid_document", "tsugumi.json is missing");
  doc->project = parse_project_json(*project_text);
  doc->project_json = *project_text;
  doc->disk_hashes["tsugumi.json"] = crypto::sha256_hex(*project_text);

  auto layout_text = fs::read_file(root / "layout.json");
  doc->layout_json = layout_text ? *layout_text : kEmptyLayout;
  doc->layout = parse_layout_json(doc->layout_json);
  if (layout_text) doc->disk_hashes["layout.json"] = crypto::sha256_hex(*layout_text);

  std::error_code ec;
  if (stdfs::is_directory(root / "definitions", ec)) {
    for (const auto& entry : stdfs::directory_iterator(root / "definitions")) {
      if (entry.is_symlink()) throw DocumentError("invalid_path", "definitions may not be symlinks");
      if (!entry.is_directory()) continue;
      std::string id = entry.path().filename().string();
      if (!valid_id(id)) throw DocumentError("invalid_document", "invalid definition id " + id);
      auto card_text = fs::read_file(entry.path() / "_card.json");
      if (!card_text) throw DocumentError("invalid_document", "definitions/" + id + "/_card.json is missing");
      DefinitionDoc d;
      d.card = parse_card_json(id, *card_text);
      d.card_json = *card_text;
      doc->disk_hashes["definitions/" + id + "/_card.json"] = crypto::sha256_hex(*card_text);
      if (auto meta = fs::read_file(entry.path() / "_meta.json")) {
        d.meta = parse_meta_json(*meta);
        d.meta_json = *meta;
        doc->disk_hashes["definitions/" + id + "/_meta.json"] = crypto::sha256_hex(*meta);
      }
      doc->definitions.emplace(id, std::move(d));
    }
  }
  for (const char* area : {"definitions", "skills", "graphs"}) {
    auto listing = fs::list_tree(root / area);
    if (!listing.links.empty())
      throw DocumentError("invalid_path",
                          std::string(area) + "/" + listing.links.front() + " is a symlink; links are not allowed",
                          {{"path", std::string(area) + "/" + listing.links.front()}});
    for (const auto& f : listing.files) {
      std::string rel = std::string(area) + "/" + f;
      if (!Document::is_source_area(rel)) continue;
      auto data = fs::read_file_or_throw(root / rel);
      doc->sources[rel] = SourceEntry{crypto::sha256_hex(data), data.size()};
    }
  }
  check_structure(*doc);
  doc->execution_fingerprint = execution_fingerprint(*doc);
  return doc;
}

}  // namespace tsugumi
