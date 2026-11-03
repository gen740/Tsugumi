#include "document.hh"

#include <set>

#include "util/crypto.hh"
#include "util/fs.hh"

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
  return file == "_card.json" || file == "_meta.json" || file == "_link.json";
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
  // externals/<project>/...: whole projects for Card in Card, read here as
  // sources (their own .tsugumi is never part of it).
  if (rel.starts_with("externals/"))
    return rel.find("/.tsugumi/") == std::string::npos && !rel.ends_with("/.tsugumi");
  return rel.starts_with("skills/");
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

void check_graph_shape(const schema::Graph& g, const std::string& where) {
  if (!g.cards->has_value()) invalid(where + ": \"cards\" is required");
  if (!g.connections->has_value()) invalid(where + ": \"connections\" is required");
  const auto& cards = g.cards->value();
  for (size_t i = 0; i < cards.size(); ++i)
    if (!cards[i].id->has_value() || !valid_id(cards[i].id->value()))
      invalid(where + ".cards[" + std::to_string(i) + "]: card id must match [A-Za-z0-9_.-]{1,128}");
  const auto& conns = g.connections->value();
  for (size_t i = 0; i < conns.size(); ++i) {
    std::string w = where + ".connections[" + std::to_string(i) + "]";
    if (!conns[i].from->has_value() || !conns[i].to->has_value()) invalid(w + " needs from and to");
    check_endpoint(conns[i].from->value(), w + ".from");
    check_endpoint(conns[i].to->value(), w + ".to");
  }
}

void check_boundary(const schema::Project& p, const std::string& where) {
  if (const auto* b = schema::get(p.boundary)) {
    if (const auto* ins = schema::get(b->inputs))
      for (const auto& [name, ep] : ins->entries()) check_endpoint(ep, where + " boundary.inputs." + name);
    if (const auto* outs = schema::get(b->outputs))
      for (const auto& [name, ep] : outs->entries()) check_endpoint(ep, where + " boundary.outputs." + name);
  }
}

}  // namespace

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
  check_graph_shape(p.canvas->value(), "tsugumi.json canvas");
  check_boundary(p, "tsugumi.json");
  return p;
}

schema::Project parse_external_project(const std::string& rel, const std::string& text) {
  auto p = decode_doc<schema::Project>(rel, text);
  if (!p.canvas->has_value()) invalid(rel + ": \"canvas\" is required", {{"path", rel}});
  check_graph_shape(p.canvas->value(), rel + " canvas");
  check_boundary(p, rel);
  return p;
}

schema::CardLink parse_link_json(const std::string& id, const std::string& text) {
  std::string rel = "definitions/" + id + "/_link.json";
  auto l = decode_doc<schema::CardLink>(rel, text);
  const auto* ext = schema::get(l.external);
  if (!ext || !valid_id(*ext)) invalid(rel + ": \"external\" must name a folder of externals/", {{"path", rel}});
  return l;
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
  const auto& graph = doc.project.canvas->value();
  std::set<std::string> ids;
  int triggers = 0;
  for (const auto& card : graph.cards->value()) {
    const auto& id = card.id->value();
    if (!ids.insert(id).second)
      throw DocumentError("invalid_document", "duplicate card id " + id, {{"card", id}});
    auto it = doc.definitions.find(id);
    if (it == doc.definitions.end())
      throw DocumentError("invalid_document", "card " + id + " has no definition (definitions/" + id + "/)",
                          {{"card", id}, {"definition", id}});
    if (const auto* t = schema::get(it->second.card.template_name); t && *t == "trigger") ++triggers;
  }
  if (triggers > 1) throw DocumentError("invalid_document", "the canvas has more than one trigger card");
  for (const auto& c : graph.connections->value()) {
    for (const auto* ep : {&c.from->value(), &c.to->value()}) {
      const auto& card = ep->card->value();
      if (!ids.count(card))
        throw DocumentError("invalid_document", "connection refers to unknown card " + card, {{"card", card}});
    }
  }
}

std::string execution_fingerprint(const Document& doc) {
  crypto::Sha256 h;
  h.update(doc.project_json);
  for (const auto& [id, def] : doc.definitions) {
    h.update("\0def\0", 5);
    h.update(id);
    h.update(def.card_json);
    if (def.link_json) h.update(*def.link_json);
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
      auto link_text = fs::read_file(entry.path() / "_link.json");
      if (card_text.has_value() == link_text.has_value())
        throw DocumentError("invalid_document", "definitions/" + id + " needs _card.json or _link.json (one of them)");
      DefinitionDoc d;
      if (card_text) {
        d.card = parse_card_json(id, *card_text);
        d.card_json = *card_text;
        doc->disk_hashes["definitions/" + id + "/_card.json"] = crypto::sha256_hex(*card_text);
      } else {
        d.link = parse_link_json(id, *link_text).external->value();
        d.link_json = *link_text;
        doc->disk_hashes["definitions/" + id + "/_link.json"] = crypto::sha256_hex(*link_text);
      }
      if (auto meta = fs::read_file(entry.path() / "_meta.json")) {
        d.meta = parse_meta_json(*meta);
        d.meta_json = *meta;
        doc->disk_hashes["definitions/" + id + "/_meta.json"] = crypto::sha256_hex(*meta);
      }
      doc->definitions.emplace(id, std::move(d));
    }
  }
  for (const char* area : {"definitions", "skills", "externals"}) {
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
