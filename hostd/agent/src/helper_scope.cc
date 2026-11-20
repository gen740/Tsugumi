#include "agent/helper_scope.hh"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "host/comments.hh"
#include "host/document_edits.hh"
#include "host/failure.hh"
#include "host/save.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi::agent {
namespace {
auto Deny(const std::string& detail) -> void {
  throw host::Failure("permission_denied",
                      "Helper scope denied: " + detail +
                          ". Ask the parent to delegate the required Cards.");
}
// A wire list is a set of edges; its order is not an authority boundary.
// Projection/expansion puts hidden edges first, which must not deny an edit
// depending on where its Card happened to appear in the original list.
auto Difference(const std::string& a, const std::string& b,
                const std::string& path = "") -> std::string {
  if (host::DocumentsEqual(a, b)) return "";
  if (a.starts_with('{') && b.starts_with('{')) {
    const auto left = json::Object(a), right = json::Object(b);
    auto keys = left;
    keys.insert(right.begin(), right.end());
    for (const auto& [key, ignored] : keys) {
      (void)ignored;
      const auto field = path + "/" + key;
      if (!left.contains(key) || !right.contains(key)) return field;
      if (const auto diff = Difference(left.at(key), right.at(key), field);
          !diff.empty())
        return diff;
    }
    return "";
  }
  if (a.starts_with('[') && b.starts_with('[')) {
    const auto left = json::Array(a);
    auto right = json::Array(b);
    if (path == "/canvas/connections") {
      for (const auto& wire : left) {
        const auto found =
            std::find_if(right.begin(), right.end(), [&](const auto& other) {
              return host::DocumentsEqual(wire, other);
            });
        if (found == right.end()) return path;
        right.erase(found);
      }
      return right.empty() ? "" : path;
    }
    for (size_t i = 0; i < std::min(left.size(), right.size()); ++i)
      if (const auto diff =
              Difference(left[i], right[i], path + "/" + std::to_string(i));
          !diff.empty())
        return diff;
  }
  return path.empty() ? "/" : path;
}
auto Card(std::string_view raw) -> std::string {
  const auto value = json::Object(raw);
  std::string path;
  for (const auto& part : json::Array(json::Get(value, "segments", "[]"))) {
    if (!path.empty()) path += '/';
    path += json::String(part);
  }
  return path;
}
auto HasExternal(const std::string& card) -> bool {
  return !card.empty() && json::Object(card).contains("external");
}
auto ValidCard(const std::string& card) -> bool {
  if (!fs::valid_relative_path(card)) return false;
  size_t begin = 0;
  do {
    const auto end = card.find('/', begin);
    const auto part = card.substr(begin, end - begin);
    if (part.empty() || part.size() > 128 ||
        !std::all_of(part.begin(), part.end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        }))
      return false;
    if (end == std::string::npos) return true;
    begin = end + 1;
  } while (true);
}
auto Set(const json::Members& task, const std::string& field)
    -> std::set<std::string> {
  std::set<std::string> result;
  for (const auto& raw : json::Array(json::Get(task, field, "[]"))) {
    const auto card = json::String(raw);
    if (!ValidCard(card))
      throw std::runtime_error("Invalid helper Card path in " + field);
    result.insert(card);
  }
  return result;
}
}  // namespace

HelperScope::HelperScope(const json::Members& task, std::string authorization,
                         std::string scratch_identity)
    : depth_(json::UInt(json::Get(task, "depth", "0"))),
      count_(json::UInt(json::Get(task, "count", "0"))),
      scoped_(task.contains("read_cards") && task.contains("write_cards") &&
              task.contains("run_cards")),
      reads_(Set(task, "read_cards")),
      writes_(Set(task, "write_cards")),
      runs_(Set(task, "run_cards")),
      authorization_(std::move(authorization)),
      scratch_identity_(std::move(scratch_identity)) {}

auto HelperScope::Validate(const json::Members& task, bool allow_unscoped)
    -> void {
  if ((task.contains("read_cards") || task.contains("write_cards") ||
       task.contains("run_cards")) &&
      !(task.contains("read_cards") && task.contains("write_cards") &&
        task.contains("run_cards")))
    throw std::runtime_error(
        "Supply read_cards, write_cards and run_cards "
        "(empty arrays are allowed)");
  if (!task.contains("read_cards") && !allow_unscoped)
    throw std::runtime_error(
        "Unscoped helpers are disabled; supply "
        "read_cards, write_cards and run_cards");
  for (const auto* key : {"depth", "count"}) {
    const auto value = json::Get(task, key, "0");
    if (value != std::to_string(json::UInt(value)))
      throw std::runtime_error(
          "Helper capability must be a nonnegative integer");
  }
  HelperScope scope(task);
}
auto HelperScope::CheckChild(const json::Members& task) const -> void {
  const HelperScope child(task);
  if (!depth_ || child.depth_ >= depth_)
    Deny("child depth exceeds the parent's capability");
  if (!scoped_) return;
  if (!child.scoped_ ||
      !std::includes(reads_.begin(), reads_.end(), child.reads_.begin(),
                     child.reads_.end()) ||
      !std::includes(writes_.begin(), writes_.end(), child.writes_.begin(),
                     child.writes_.end()) ||
      !std::includes(runs_.begin(), runs_.end(), child.runs_.begin(),
                     child.runs_.end()))
    Deny("child Card sets exceed the parent's sets");
}
auto HelperScope::Require(const std::string& card, bool writing) const -> void {
  if (!(writing ? writes_ : reads_).contains(card))
    Deny(std::string(writing ? "write of " : "read of ") +
         (card.empty() ? "the whole project" : card));
}
auto HelperScope::Ran(const std::string& run_id, const std::string& card)
    -> void {
  if (!run_id.empty()) runs_started_[run_id] = card;
}
auto HelperScope::RequireRun(const std::string& card) const -> void {
  if (!runs_.contains(card))
    Deny("run of " + (card.empty() ? "the whole project" : card));
}
auto HelperScope::Root(const std::string& card, const Reader& read,
                       bool writing) const -> std::string {
  if (const auto found = created_.find(card); found != created_.end())
    return found->second;
  std::string prefix;
  std::vector<std::string> logical_prefixes{""};
  size_t begin = 0;
  for (;;) {
    const auto end = card.find('/', begin);
    const auto id = card.substr(begin, end - begin);
    if (end == std::string::npos) return prefix + "definitions/" + id;
    const auto link =
        json::Object(read(prefix + "definitions/" + id + "/_card.json"));
    const auto external = json::Text(link, "external");
    if (!ValidCard(external) || external.find('/') != std::string::npos)
      Deny("unresolved nested Card " + card);
    // Multiple container Cards may point at the same definitions. Every
    // alias of a shared definition must be assigned, or a file write/read
    // would cross a different Card's boundary through shared storage.
    const auto project = json::Object(read(prefix + "tsugumi.json"));
    const auto canvas = json::Object(json::Get(project, "canvas", "{}"));
    std::set<std::string> aliases{id};
    for (const auto& raw : json::Array(json::Get(canvas, "cards", "[]"))) {
      const auto sibling = json::Text(json::Object(raw), "id");
      if (sibling == id || !ValidCard(sibling)) continue;
      const auto sibling_link =
          read(prefix + "definitions/" + sibling + "/_card.json");
      if (sibling_link.empty()) continue;
      if (json::Text(json::Object(sibling_link), "external") == external)
        aliases.insert(sibling);
    }
    std::vector<std::string> next;
    for (const auto& logical : logical_prefixes)
      for (const auto& alias : aliases) {
        const auto nested = logical + alias + '/';
        Require(nested + card.substr(end + 1), writing);
        next.push_back(nested);
      }
    logical_prefixes = std::move(next);
    prefix += "externals/" + external + '/';
    begin = end + 1;
  }
}
auto HelperScope::File(const std::string& path, bool writing,
                       const Reader& read) const -> void {
  if (!fs::valid_relative_path(path)) Deny("invalid file path");
  if (!writing && (path == "tsugumi.json" || path.ends_with("/tsugumi.json"))) {
    (void)ProjectPrefix(path, read);
    return;
  }
  for (const auto& card : writing ? writes_ : reads_) {
    std::string root;
    try {
      root = Root(card, read, writing);
    } catch (const std::exception&) {
      continue;
    }
    if (path == root || path.starts_with(root + '/')) {
      if (writing && path == root + "/_card.json" && HasExternal(read(path)))
        Deny("replacing a container definition");
      return;
    }
  }
  Deny(std::string(writing ? "write of file " : "read of file ") + path);
}
auto HelperScope::Created(const std::string& id,
                          const std::string& project_path, const Reader& read)
    -> void {
  if (!scoped_ || !ValidCard(id) || id.find('/') != std::string::npos) return;
  // Find the logical parent through its link, rather than assuming that a
  // container's permanent identifier and its external directory are equal.
  std::string parent;
  if (!project_path.empty()) {
    for (const auto& card : writes_) {
      const auto root = Root(card, read, true);
      const auto at = root.rfind("definitions/");
      if (root.substr(0, at) == project_path) {
        const auto slash = card.rfind('/');
        if (slash != std::string::npos) {
          parent = card.substr(0, slash + 1);
          break;
        }
      }
      const auto link_text = read(root + "/_card.json");
      if (link_text.empty()) continue;
      const auto link = json::Object(link_text);
      const auto external = json::Text(link, "external");
      if (root.substr(0, at) + "externals/" + external + '/' == project_path) {
        parent = card + '/';
        break;
      }
    }
    if (parent.empty()) Deny("creating in an unassigned nested project");
  }
  const auto card = parent + id;
  reads_.insert(card);
  writes_.insert(card);
  runs_.insert(card);
  created_[card] = project_path + "definitions/" + id;
}
auto HelperScope::ProjectPrefix(const std::string& path,
                                const Reader& read) const -> std::string {
  if (path == "tsugumi.json") return "";
  for (const auto& card : reads_) {
    const auto root = Root(card, read);
    const auto at = root.rfind("definitions/");
    if (root.substr(0, at) + "tsugumi.json" == path) {
      const auto slash = card.rfind('/');
      return slash == std::string::npos ? "" : card.substr(0, slash + 1);
    }
  }
  Deny("project document outside readable Cards");
  return "";
}
auto HelperScope::FailureResult(const std::string& result,
                                const Reader& read) const -> std::string {
  if (!scoped_) return result;
  std::set<std::string> hidden, documents{"tsugumi.json"};
  for (const auto& card : reads_) {
    const auto root = Root(card, read);
    documents.insert(root.substr(0, root.rfind("definitions/")) +
                     "tsugumi.json");
  }
  for (const auto& path : documents) {
    const auto text = read(path);
    if (text.empty()) continue;
    const auto canvas =
        json::Object(json::Get(json::Object(text), "canvas", "{}"));
    for (const auto& raw : json::Array(json::Get(canvas, "cards", "[]"))) {
      const auto id = json::Text(json::Object(raw), "id");
      bool visible = false;
      for (const auto& card : reads_) {
        const auto root = Root(card, read);
        visible = visible || root == path.substr(0, path.size() - 12) +
                                         "definitions/" + id;
      }
      if (!id.empty() && !visible) hidden.insert(id);
    }
  }
  const auto redact = [&](const std::string& message) {
    std::string filtered;
    const auto word = [](unsigned char c) {
      return std::isalnum(c) || c >= 128 || c == '_' || c == '-' || c == '.' ||
             c == '/' || c == '~';
    };
    for (size_t i = 0; i < message.size();) {
      if (!word(message[i])) {
        filtered += message[i++];
        continue;
      }
      const auto start = i++;
      while (i < message.size() && word(message[i])) ++i;
      auto token = message.substr(start, i - start);
      // Scope-name redaction: hide whole Card/path tokens outside read_cards,
      // including JSON-pointer paths. Retain the rest of the diagnostic so a
      // helper can fix its own call without asking the parent for the failure.
      auto decoded = token;
      for (size_t at = 0; (at = decoded.find("~1", at)) != std::string::npos;)
        decoded.replace(at, 2, "/");
      if (decoded.starts_with('/')) decoded.erase(0, 1);
      bool visible_card = reads_.contains(decoded);
      for (const auto& card : reads_)
        visible_card = visible_card || decoded.starts_with(card + '.');
      bool secret = false;
      if (!visible_card)
        for (const auto& id : hidden)
          secret = secret || token == id || token.starts_with(id + '.') ||
                   token.starts_with(id + '/') || decoded == id;
      const auto dot = decoded.find('.');
      const bool filename =
          dot != std::string::npos && dot > 0 && dot + 1 < decoded.size() &&
          std::isalpha(static_cast<unsigned char>(decoded[dot + 1])) &&
          decoded != "from.port" && decoded != "to.port" &&
          decoded != "_card.json" && decoded != "_meta.json" &&
          decoded != "_outputs.fbs" && decoded != "tsugumi.json";
      const bool path = decoded.find('/') != std::string::npos || filename;
      bool visible_pointer = false;
      for (const auto& card : reads_) {
        const auto root = Root(card, read);
        const auto document =
            root.substr(0, root.rfind("definitions/")) + "tsugumi.json";
        const auto id = root.substr(root.rfind('/') + 1);
        const auto pointer = document + "/canvas/cards/" + id;
        visible_pointer = visible_pointer || decoded == pointer ||
                          decoded.starts_with(pointer + '/') ||
                          decoded == document + "/canvas/connections";
      }
      if (path && !visible_pointer && !reads_.contains(decoded)) {
        try {
          File(decoded, false, read);
        } catch (const std::exception&) {
          secret = true;
        }
      }
      filtered += secret ? "[redacted]" : token;
    }
    return filtered;
  };
  const auto envelope = json::Object(result);
  const auto failure = json::Object(json::Get(envelope, "value", "{}"));
  json::Members safe;
  for (const auto* key : {"kind", "current_rev", "retry_after_ms"})
    if (failure.contains(key)) safe[key] = failure.at(key);
  for (const auto* key : {"message", "diagnostic"})
    if (failure.contains(key))
      safe[key] = json::Quote(redact(json::Text(failure, key)));
  std::vector<std::string> args;
  for (const auto& raw : json::Array(json::Get(failure, "args", "[]"))) {
    auto arg = json::Object(raw);
    if (arg.contains("value"))
      arg["value"] = json::Quote(redact(json::Text(arg, "value")));
    args.push_back(json::Encode(arg));
  }
  if (failure.contains("args")) safe["args"] = json::Encode(args);
  return json::Encode(json::Members{{"reply", json::Quote("Failure")},
                                    {"value", json::Encode(safe)}});
}

auto HelperScope::CheckBindings(const Reader& before, const Reader& now) const
    -> void {
  auto resolved = *this;
  resolved.created_.clear();
  for (const auto& card : writes_)
    if (resolved.Root(card, before, true) != resolved.Root(card, now, true))
      Deny("changed container binding for " + card);
}

auto HelperScope::Graph(const std::string& text,
                        const std::string& prefix) const -> std::string {
  const auto project = json::Object(text);
  json::Members visible;
  for (const auto* key : {"name", "pkgsPath"})
    if (project.contains(key)) visible[key] = project.at(key);
  const auto canvas = json::Object(json::Get(project, "canvas", "{}"));
  visible["canvas"] = json::Encode(
      json::Members{{"cards", json::Get(canvas, "cards", "[]")},
                    {"connections", json::Get(canvas, "connections", "[]")}});
  if (project.contains("boundary")) {
    const auto boundary = json::Object(project.at("boundary"));
    visible["boundary"] = json::Encode(
        json::Members{{"inputs", json::Get(boundary, "inputs", "{}")},
                      {"outputs", json::Get(boundary, "outputs", "{}")}});
  }
  return json::Encode(host::FilterProjectGraph(
      std::move(visible),
      [&](const auto& id) { return reads_.contains(prefix + id); }));
}

auto HelperScope::ExpandSave(json::Members args, const Reader& read) const
    -> json::Members {
  if (!scoped_) return args;
  auto changes = json::Array(json::Get(args, "changes", "[]"));
  for (auto& raw : changes) {
    auto change = json::Object(raw);
    const auto type = json::Text(change, "value_type");
    auto value = json::Object(json::Get(change, "value", "{}"));
    const auto path =
        type == "SetProject" ? "tsugumi.json" : json::Text(value, "path");
    if (type != "SetProject" &&
        !(type == "PutSource" && path.ends_with("/tsugumi.json")))
      continue;
    std::string logical;
    if (path != "tsugumi.json") {
      bool found = false;
      for (const auto& card : writes_) {
        const auto root = Root(card, read, true);
        const auto end = root.rfind("definitions/");
        if (root.substr(0, end) + "tsugumi.json" != path) continue;
        const auto slash = card.rfind('/');
        logical = slash == std::string::npos ? "" : card.substr(0, slash + 1);
        found = true;
        break;
      }
      if (!found) Deny("graph outside assigned Cards");
    }
    const auto original = read(path);
    auto full = json::Object(original);
    auto visible = *this;
    visible.reads_.insert(writes_.begin(), writes_.end());
    auto before = json::Object(visible.Graph(original, logical));
    const auto bytes = type == "SetProject" ? json::Text(value, "json")
                                            : json::Text(value, "bytes");
    auto after = json::Object(bytes);
    const auto bg = json::Object(json::Get(before, "canvas", "{}"));
    const auto ag = json::Object(json::Get(after, "canvas", "{}"));
    if (before.contains("boundary") || after.contains("boundary")) {
      const auto bb = json::Object(json::Get(before, "boundary", "{}"));
      const auto ab = json::Object(json::Get(after, "boundary", "{}"));
      auto boundary = json::Object(json::Get(full, "boundary", "{}"));
      for (const auto* side : {"inputs", "outputs"}) {
        const auto bp = json::Object(json::Get(bb, side, "{}")),
                   ap = json::Object(json::Get(ab, side, "{}"));
        if (bp.size() != ap.size()) Deny("changing boundary port names");
        auto ports = json::Object(json::Get(boundary, side, "{}"));
        for (const auto& [name, raw] : ap) {
          if (!bp.contains(name)) Deny("adding a boundary port");
          if (!host::DocumentsEqual(bp.at(name), raw)) {
            Require(logical + json::Text(json::Object(bp.at(name)), "card"),
                    true);
            Require(logical + json::Text(json::Object(raw), "card"), true);
            ports[name] = raw;
          }
        }
        boundary[side] = json::Encode(ports);
      }
      if (bb.size() != ab.size()) Deny("changing boundary fields");
      full["boundary"] = json::Encode(boundary);
      before.erase("boundary");
      after.erase("boundary");
    }
    before.erase("canvas");
    after.erase("canvas");
    if (const auto field =
            Difference(json::Encode(before), json::Encode(after));
        !field.empty())
      Deny("project field " + path + field);
    auto bfields = bg, afields = ag;
    for (const auto* key : {"cards", "connections"}) {
      bfields.erase(key);
      afields.erase(key);
    }
    if (const auto field =
            Difference(json::Encode(bfields), json::Encode(afields));
        !field.empty())
      Deny("project field " + path + "/canvas" + field);
    const auto records = [](const std::string& text) {
      json::Members out;
      for (const auto& c : json::Array(text)) {
        const auto id = json::Text(json::Object(c), "id");
        if (!out.emplace(id, c).second) Deny("duplicate Card " + id);
      }
      return out;
    };
    const auto bc = records(json::Get(bg, "cards", "[]")),
               ac = records(json::Get(ag, "cards", "[]"));
    for (const auto& [id, c] : ac) {
      if (!bc.contains(id)) {
        if (!created_.contains(logical + id))
          Deny("adding an unreserved Card " + logical + id);
        if (json::Object(c) != json::Members{{"id", json::Quote(id)}})
          Deny("new Card fields");
      } else if (!host::DocumentsEqual(bc.at(id), c) &&
                 !writes_.contains(logical + id)) {
        Deny("project field " + path + "/canvas/cards/" + id +
             Difference(bc.at(id), c));
      }
    }
    for (const auto& [id, ignored] : bc) {
      (void)ignored;
      if (!ac.contains(id) && !writes_.contains(logical + id))
        Deny("project field " + path + "/canvas/cards/" + id);
    }
    const auto bw = json::Array(json::Get(bg, "connections", "[]"));
    const auto aw = json::Array(json::Get(ag, "connections", "[]"));
    auto check_wire = [&](const std::string& wire) {
      const auto w = json::Object(wire);
      bool writable = false;
      for (const auto* side : {"from", "to"}) {
        const auto id =
            logical +
            json::Text(json::Object(json::Get(w, side, "{}")), "card");
        if (!reads_.contains(id) && !writes_.contains(id))
          Deny("project field " + path + "/canvas/connections/" + side +
               "/card (" + id + ")");
        writable = writable || writes_.contains(id);
      }
      if (!writable)
        Deny("project field " + path +
             "/canvas/connections (wire between read-only Cards)");
    };
    const auto has_wire = [](const auto& wires, const auto& wire) {
      return std::ranges::any_of(wires, [&](const auto& other) {
        return host::DocumentsEqual(wire, other);
      });
    };
    for (const auto& w : aw)
      if (!has_wire(bw, w)) check_wire(w);
    for (const auto& w : bw)
      if (!has_wire(aw, w)) check_wire(w);
    auto graph = json::Object(json::Get(full, "canvas", "{}"));
    std::vector<std::string> cards, wires;
    for (const auto& c : json::Array(json::Get(graph, "cards", "[]"))) {
      const auto id = json::Text(json::Object(c), "id");
      if (!bc.contains(id))
        cards.push_back(c);
      else if (ac.contains(id))
        cards.push_back(ac.at(id));
    }
    for (const auto& [id, c] : ac)
      if (!bc.contains(id)) cards.push_back(c);
    for (const auto& w : json::Array(json::Get(graph, "connections", "[]")))
      if (std::find(bw.begin(), bw.end(), w) == bw.end()) wires.push_back(w);
    wires.insert(wires.end(), aw.begin(), aw.end());
    graph["cards"] = json::Encode(cards);
    graph["connections"] = json::Encode(wires);
    full["canvas"] = json::Encode(graph);
    value[type == "SetProject" ? "json" : "bytes"] =
        json::Quote(json::Encode(full));
    change["value"] = json::Encode(value);
    raw = json::Encode(change);
  }
  args["changes"] = json::Encode(changes);
  return args;
}

auto HelperScope::CheckCurrentSave(const json::Members& args,
                                   const Reader& read) const -> void {
  if (!scoped_) return;
  auto visible = *this;
  visible.reads_.insert(writes_.begin(), writes_.end());
  auto projected = args;
  std::vector<std::string> changes;
  std::map<std::string, std::string> complete;
  for (const auto& raw : json::Array(json::Get(args, "changes", "[]"))) {
    auto change = json::Object(raw);
    const auto type = json::Text(change, "value_type");
    auto value = json::Object(json::Get(change, "value", "{}"));
    const auto path =
        type == "SetProject" ? "tsugumi.json" : json::Text(value, "path");
    if (type == "SetLayout" ||
        (type == "PutSource" && path.ends_with("/layout.json")))
      continue;
    if (type == "SetProject" ||
        (type == "PutSource" && path.ends_with("/tsugumi.json"))) {
      const auto key = type == "SetProject" ? "json" : "bytes";
      const auto text = json::Text(value, key);
      complete[path] = text;
      value[key] =
          json::Quote(visible.Graph(text, visible.ProjectPrefix(path, read)));
      change["value"] = json::Encode(value);
      changes.push_back(json::Encode(change));
    } else
      changes.push_back(raw);
  }
  projected["changes"] = json::Encode(changes);
  const auto expanded = ExpandSave(projected, read);
  for (const auto& raw : json::Array(json::Get(expanded, "changes", "[]"))) {
    const auto change = json::Object(raw);
    const auto type = json::Text(change, "value_type");
    const auto value = json::Object(json::Get(change, "value", "{}"));
    const auto path =
        type == "SetProject" ? "tsugumi.json" : json::Text(value, "path");
    if (complete.contains(path)) {
      const auto field = Difference(
          complete.at(path),
          json::Text(value, type == "SetProject" ? "json" : "bytes"));
      if (!field.empty())
        Deny("project field " + path + field +
             " is outside the assigned Cards");
    }
  }
  Check("project_save", projected, read);
}

auto HelperScope::Check(const std::string& tool, const json::Members& args,
                        const Reader& read) const -> void {
  if (!scoped_) return;
  if (tool == "project_copy" || tool == "project_shell" ||
      tool == "attached_folder_read")
    return;  // Tools::FileOperation checks each materialized
             // source/destination.
  if (tool == "project_inspect" || tool == "output_types" ||
      tool == "project_file_format")
    return;
  if (tool == "input_types") {
    Require(Card(json::Get(args, "card", "{}")), false);
    return;
  }
  if (tool == "card_id") {
    const auto project_path = json::Text(args, "project_path");
    if (project_path.empty()) return;
    // Validate the parent without reserving or granting a real identifier.
    auto copy = *this;
    copy.Created("scope-probe", project_path, read);
    return;
  }
  if (tool == "project_file_read" || tool == "project_file_stat" ||
      tool == "project_file_list") {
    File(json::Text(args, "path"), false, read);
    return;
  }
  if (tool == "project_save") {
    (void)ExpandSave(args, read);
    for (const auto& raw : json::Array(json::Get(args, "changes", "[]"))) {
      const auto change = json::Object(raw);
      const auto type = json::Text(change, "value_type");
      const auto value = json::Object(json::Get(change, "value", "{}"));
      if (type == "SetProject") {
        continue;
      } else if (type == "PutDefinition" || type == "RemoveDefinition") {
        const auto id = json::Text(value, "id");
        Require(id, true);
        const auto old_card = read(Root(id, read, true) + "/_card.json");
        if (HasExternal(old_card)) Deny("replacing a container definition");
        if (HasExternal(json::Text(value, "card_json")))
          Deny("changing a container link");
      } else if (type == "PutSource" || type == "PatchSource" ||
                 type == "RemoveSource") {
        const auto path = json::Text(value, "path");
        if (type == "PutSource" && path.ends_with("/tsugumi.json")) continue;
        File(path, true, read);
        if (type != "RemoveSource" && path.ends_with("/_card.json") &&
            comments::is_meta(path.substr(0, path.size() - 10) +
                                    "_meta.json")) {
          const host::SourcePatches patches;
          const auto card =
              type == "PutSource"
                  ? host::DecodeSourceBytes(json::Get(value, "bytes"))
                  : patches.Project(read(path),
                                    json::Array(json::Get(value, "patches")));
          if (HasExternal(card)) Deny("changing a container link");
        }
      } else {
        Deny(
            "project-wide save (the parent handles canvas and layout changes)");
      }
    }
    return;
  }
  if (tool == "project_validate") {
    if (args.contains("changes")) Deny("project-wide proposed changes");
    const auto targets = json::Array(json::Get(args, "targets", "[]"));
    if (targets.empty()) Deny("project-wide validation");
    for (const auto& target : targets) Require(Card(target), false);
    return;
  }
  if (tool == "run_cancel" || tool == "run_status") {
    const auto found = runs_started_.find(json::Text(args, "run_id"));
    if (found == runs_started_.end()) Deny("unowned run");
    RequireRun(found->second);
    return;
  }
  if (tool == "run" || tool == "card_s_d_k_prepare") {
    RequireRun(Card(json::Get(args, "card", "{}")));
    return;
  }
  if (tool == "run_plan" || tool == "run_history" ||
      tool == "run_invocations" || tool == "run_record" ||
      tool == "run_value_read" || tool == "artifact_list" ||
      tool == "log_range") {
    Require(Card(json::Get(args, "card", "{}")), false);
    return;
  }
  // Tools addressing a project, run, operation or artifact without a Card
  // cannot establish this boundary. Keep the existing catalog and approvals;
  // refuse only this scoped invocation, including future unclassified tools.
  Deny("tool " + tool + " has no enforceable Card boundary");
}
auto HelperScope::Completed(const std::string& tool, const json::Members& args,
                            const std::string& result, const Reader& read)
    -> void {
  if (tool != "card_id" && tool != "run") return;
  const auto reply = json::Object(result);
  const auto value = json::Object(json::Get(reply, "value", "{}"));
  if (tool == "card_id" && json::Text(reply, "reply") == "CardIdResult")
    Created(json::Text(value, "id"), json::Text(args, "project_path"), read);
  if (tool == "run" && json::Text(reply, "reply") == "RunAccepted")
    Ran(json::Text(json::Object(json::Get(value, "run", "{}")), "run_id"),
        Card(json::Get(args, "card", "{}")));
}
auto HelperScope::Restore(const std::string& receipt, const Reader& read)
    -> void {
  const auto saved = json::Object(receipt);
  if (saved.contains("helper_project_path"))
    Completed("card_id", {{"project_path", saved.at("helper_project_path")}},
              receipt, read);
  if (saved.contains("helper_run_card"))
    Completed("run", {{"card", saved.at("helper_run_card")}}, receipt, read);
}
auto HelperScope::CheckResult(const std::string& result,
                              const Reader& read) const -> void {
  if (!scoped_) return;
  if (result.empty()) return;
  if (result.front() == '[') {
    for (const auto& item : json::Array(result)) CheckResult(item, read);
  } else if (result.front() == '{') {
    for (const auto& [key, value] : json::Object(result)) {
      if ((key == "card" || key == "target") && value.starts_with('{'))
        Require(Card(value), false);
      if (key == "path") {
        const auto path = json::String(value);
        if (path.starts_with("definitions/") || path.starts_with("externals/"))
          File(path, false, read);
      }
      CheckResult(value, read);
    }
  }
}
auto HelperScope::CheckPlan(const std::string& result, bool executing) const
    -> void {
  if (!scoped_) return;
  const auto envelope = json::Object(result);
  if (json::Text(envelope, "reply") != "RunPlanResult")
    Deny("run plan unavailable");
  const auto value = json::Object(json::Get(envelope, "value", "{}"));
  for (const auto* field : {"targets", "starts", "build_candidates"})
    for (const auto& raw : json::Array(json::Get(value, field, "[]"))) {
      if (executing)
        RequireRun(Card(raw));
      else
        Require(Card(raw), false);
    }
}
auto HelperScope::SaveResult(const std::string& result,
                             const Reader& read) const -> std::string {
  auto envelope = json::Object(result);
  if (json::Text(envelope, "reply") != "SaveResult") return result;
  auto value = json::Object(json::Get(envelope, "value", "{}"));
  if (value.contains("delta") && value.at("delta") != "null") {
    auto delta = json::Object(value.at("delta"));
    auto snapshot = delta;
    snapshot["definitions"] = json::Get(delta, "definitions_upsert", "[]");
    snapshot["sources"] = json::Get(delta, "sources_upsert", "[]");
    const auto filtered = json::Object(json::Get(
        json::Object(Inspect(json::Encode(json::Members{
                                 {"reply", json::Quote("ProjectSnapshot")},
                                 {"value", json::Encode(snapshot)}}),
                             read)),
        "value"));
    delta["definitions_upsert"] = json::Get(filtered, "definitions", "[]");
    delta["sources_upsert"] = json::Get(filtered, "sources", "[]");
    delta["resolution"] = json::Get(filtered, "resolution", "[]");
    delta["diagnostics"] = "[]";
    if (delta.contains("project_json"))
      delta["project_json"] = filtered.at("project_json");
    delta.erase("layout_json");
    for (const auto* field : {"definitions_removed", "sources_removed"}) {
      std::vector<std::string> kept;
      for (const auto& raw : json::Array(json::Get(delta, field, "[]"))) {
        const auto id = json::String(raw);
        try {
          if (std::string_view(field) == "sources_removed")
            File(id, false, read);
          else
            Require(id, false);
          kept.push_back(raw);
        } catch (const std::exception&) {
        }
      }
      delta[field] = json::Encode(kept);
    }
    value["delta"] = json::Encode(delta);
  }
  if (value.contains("comments")) {
    std::vector<std::string> kept;
    for (const auto& raw : json::Array(value.at("comments")))
      if (reads_.contains(json::Text(json::Object(raw), "card")))
        kept.push_back(raw);
    value["comments"] = json::Encode(kept);
  }
  envelope["value"] = json::Encode(value);
  return json::Encode(envelope);
}

auto HelperScope::Inspect(const std::string& result, const Reader& read) const
    -> std::string {
  if (!scoped_) return result;
  auto envelope = json::Object(result);
  if (json::Text(envelope, "reply") != "ProjectSnapshot") return result;
  const auto snapshot = json::Object(json::Get(envelope, "value", "{}"));
  json::Members filtered;
  for (const auto* key :
       {"project_id", "rev", "execution_rev", "execution_services"})
    if (snapshot.contains(key)) filtered[key] = snapshot.at(key);
  // A scoped inspection reveals neither other Cards, their diagnostics,
  // layout, run state nor references embedded in the project document.
  std::vector<std::string> definitions, sources, resolution;
  for (const auto& raw : json::Array(json::Get(snapshot, "definitions", "[]")))
    if (reads_.contains(json::Text(json::Object(raw), "id")))
      definitions.push_back(raw);
  for (const auto& raw : json::Array(json::Get(snapshot, "sources", "[]"))) {
    try {
      File(json::Text(json::Object(raw), "path"), false, read);
      sources.push_back(raw);
    } catch (const std::exception&) {
    }
  }
  for (const auto& raw : json::Array(json::Get(snapshot, "resolution", "[]"))) {
    auto value = json::Object(raw);
    if (!reads_.contains(Card(json::Get(value, "card", "{}")))) continue;
    value["diagnostics"] = "[]";
    std::vector<std::string> references;
    for (const auto& ref :
         json::Array(json::Get(value, "nix_references", "[]")))
      if (reads_.contains(Card(json::Get(json::Object(ref), "target", "{}"))))
        references.push_back(ref);
    value["nix_references"] = json::Encode(references);
    resolution.push_back(json::Encode(value));
  }
  std::vector<std::string> readable, writable, runnable;
  for (const auto& card : reads_) readable.push_back(json::Quote(card));
  for (const auto& card : writes_) writable.push_back(json::Quote(card));
  for (const auto& card : runs_) runnable.push_back(json::Quote(card));
  filtered["helper_scope"] =
      json::Encode(json::Members{{"read_cards", json::Encode(readable)},
                                 {"write_cards", json::Encode(writable)},
                                 {"run_cards", json::Encode(runnable)}});
  filtered["definitions"] = json::Encode(definitions);
  filtered["sources"] = json::Encode(sources);
  filtered["resolution"] = json::Encode(resolution);
  filtered["diagnostics"] = "[]";
  filtered["layout_json"] = json::Quote("{}");
  filtered["run"] = "{}";
  filtered["project_json"] =
      json::Quote(Graph(json::Text(snapshot, "project_json", "{}"), ""));
  envelope["value"] = json::Encode(filtered);
  return json::Encode(envelope);
}
}  // namespace tsugumi::agent
