#include "agent/helper_scope.hh"

#include <algorithm>
#include <stdexcept>

#include "util/fs.hh"

namespace tsugumi::agent {
namespace {
auto Deny(const std::string &detail) -> void {
  throw std::runtime_error("Helper scope denied: " + detail +
                           ". Ask the parent to delegate the required Cards.");
}
auto Card(std::string_view raw) -> std::string {
  const auto value = json::Object(raw);
  std::string path;
  for (const auto &part : json::Array(json::Get(value, "segments", "[]"))) {
    if (!path.empty())
      path += '/';
    path += json::String(part);
  }
  return path;
}
auto ValidCard(const std::string &card) -> bool {
  if (!fs::valid_relative_path(card))
    return false;
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
    if (end == std::string::npos)
      return true;
    begin = end + 1;
  } while (true);
}
auto Set(const json::Members &task, const std::string &field)
    -> std::set<std::string> {
  std::set<std::string> result;
  for (const auto &raw : json::Array(json::Get(task, field, "[]"))) {
    const auto card = json::String(raw);
    if (!ValidCard(card))
      throw std::runtime_error("Invalid helper Card path in " + field);
    result.insert(card);
  }
  return result;
}
} // namespace

HelperScope::HelperScope(const json::Members &task)
    : scoped_(task.contains("read_cards") && task.contains("write_cards")),
      reads_(Set(task, "read_cards")), writes_(Set(task, "write_cards")) {}

auto HelperScope::Validate(const json::Members &task, bool allow_unscoped)
    -> void {
  if (task.contains("read_cards") != task.contains("write_cards"))
    throw std::runtime_error(
        "Supply both read_cards and write_cards (empty arrays are allowed)");
  if (!task.contains("read_cards") && !allow_unscoped)
    throw std::runtime_error(
        "Unscoped helpers are disabled; supply read_cards and write_cards");
  HelperScope scope(task);
}
auto HelperScope::Require(const std::string &card, bool writing) const -> void {
  if (!(writing ? writes_ : reads_).contains(card))
    Deny(std::string(writing ? "write or run of " : "read of ") +
         (card.empty() ? "the whole project" : card));
}
auto HelperScope::Root(const std::string &card, const Reader &read,
                       bool writing) const -> std::string {
  if (const auto found = created_.find(card); found != created_.end())
    return found->second;
  std::string prefix;
  std::vector<std::string> logical_prefixes{""};
  size_t begin = 0;
  for (;;) {
    const auto end = card.find('/', begin);
    const auto id = card.substr(begin, end - begin);
    if (end == std::string::npos)
      return prefix + "definitions/" + id;
    const auto link =
        json::Object(read(prefix + "definitions/" + id + "/_link.json"));
    const auto external = json::Text(link, "external");
    if (!ValidCard(external) || external.find('/') != std::string::npos)
      Deny("unresolved nested Card " + card);
    // Multiple container Cards may point at the same definitions. Every
    // alias of a shared definition must be assigned, or a file write/read
    // would cross a different Card's boundary through shared storage.
    const auto project = json::Object(read(prefix + "tsugumi.json"));
    const auto canvas = json::Object(json::Get(project, "canvas", "{}"));
    std::set<std::string> aliases{id};
    for (const auto &raw : json::Array(json::Get(canvas, "cards", "[]"))) {
      const auto sibling = json::Text(json::Object(raw), "id");
      if (sibling == id || !ValidCard(sibling))
        continue;
      const auto sibling_link =
          read(prefix + "definitions/" + sibling + "/_link.json");
      if (sibling_link.empty())
        continue;
      if (json::Text(json::Object(sibling_link), "external") == external)
        aliases.insert(sibling);
    }
    std::vector<std::string> next;
    for (const auto &logical : logical_prefixes)
      for (const auto &alias : aliases) {
        const auto nested = logical + alias + '/';
        Require(nested + card.substr(end + 1), writing);
        next.push_back(nested);
      }
    logical_prefixes = std::move(next);
    prefix += "externals/" + external + '/';
    begin = end + 1;
  }
}
auto HelperScope::File(const std::string &path, bool writing,
                       const Reader &read) const -> void {
  if (!fs::valid_relative_path(path))
    Deny("invalid file path");
  for (const auto &card : writing ? writes_ : reads_) {
    std::string root;
    try {
      root = Root(card, read, writing);
    } catch (const std::exception &) {
      continue;
    }
    if (path == root || path.starts_with(root + '/')) {
      // Replacing a container's link changes the meaning of all nested paths.
      if (writing && path == root + "/_link.json")
        Deny("changing a container link");
      if (writing && path == root + "/_card.json" &&
          !read(root + "/_link.json").empty())
        Deny("replacing a container definition");
      return;
    }
  }
  Deny(std::string(writing ? "write of file " : "read of file ") + path);
}
auto HelperScope::Created(const std::string &id,
                          const std::string &project_path, const Reader &read)
    -> void {
  if (!scoped_ || !ValidCard(id) || id.find('/') != std::string::npos)
    return;
  // Find the logical parent through its link, rather than assuming that a
  // container's permanent identifier and its external directory are equal.
  std::string parent;
  if (!project_path.empty()) {
    for (const auto &card : writes_) {
      const auto root = Root(card, read, true);
      const auto at = root.rfind("definitions/");
      if (root.substr(0, at) == project_path) {
        const auto slash = card.rfind('/');
        if (slash != std::string::npos) {
          parent = card.substr(0, slash + 1);
          break;
        }
      }
      const auto link_text = read(root + "/_link.json");
      if (link_text.empty())
        continue;
      const auto link = json::Object(link_text);
      const auto external = json::Text(link, "external");
      if (root.substr(0, at) + "externals/" + external + '/' == project_path) {
        parent = card + '/';
        break;
      }
    }
    if (parent.empty())
      Deny("creating in an unassigned nested project");
  }
  const auto card = parent + id;
  reads_.insert(card);
  writes_.insert(card);
  created_[card] = project_path + "definitions/" + id;
}
auto HelperScope::Check(const std::string &tool, const json::Members &args,
                        const Reader &read) const -> void {
  if (!scoped_)
    return;
  if (tool == "project_inspect" || tool == "output_types" ||
      tool == "project_file_format")
    return;
  if (tool == "card_id") {
    const auto project_path = json::Text(args, "project_path");
    if (project_path.empty())
      return;
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
    for (const auto &raw : json::Array(json::Get(args, "changes", "[]"))) {
      const auto change = json::Object(raw);
      const auto type = json::Text(change, "value_type");
      const auto value = json::Object(json::Get(change, "value", "{}"));
      if (type == "PutDefinition" || type == "RemoveDefinition") {
        const auto id = json::Text(value, "id");
        Require(id, true);
        const auto old_link = read(Root(id, read, true) + "/_link.json");
        if (!old_link.empty())
          Deny("replacing a container definition");
        if (!json::Text(value, "link_json").empty())
          Deny("changing a container link");
      } else if (type == "PutSource" || type == "PatchSource" ||
                 type == "RemoveSource") {
        File(json::Text(value, "path"), true, read);
      } else {
        Deny(
            "project-wide save (the parent handles canvas and layout changes)");
      }
    }
    return;
  }
  if (tool == "project_validate") {
    if (args.contains("changes"))
      Deny("project-wide proposed changes");
    const auto targets = json::Array(json::Get(args, "targets", "[]"));
    if (targets.empty())
      Deny("project-wide validation");
    for (const auto &target : targets)
      Require(Card(target), false);
    return;
  }
  if (tool == "run" || tool == "card_s_d_k_prepare") {
    Require(Card(json::Get(args, "card", "{}")), true);
    Require(Card(json::Get(args, "card", "{}")), false);
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
auto HelperScope::CheckResult(const std::string &result,
                              const Reader &read) const -> void {
  if (!scoped_)
    return;
  if (result.empty())
    return;
  if (result.front() == '[') {
    for (const auto &item : json::Array(result))
      CheckResult(item, read);
  } else if (result.front() == '{') {
    for (const auto &[key, value] : json::Object(result)) {
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
auto HelperScope::CheckPlan(const std::string &result, bool writing) const
    -> void {
  if (!scoped_)
    return;
  const auto envelope = json::Object(result);
  if (json::Text(envelope, "reply") != "RunPlanResult")
    Deny("run plan unavailable");
  const auto value = json::Object(json::Get(envelope, "value", "{}"));
  for (const auto *field : {"targets", "starts", "build_candidates"})
    for (const auto &raw : json::Array(json::Get(value, field, "[]"))) {
      Require(Card(raw), false);
      if (writing)
        Require(Card(raw), true);
    }
}
auto HelperScope::Inspect(const std::string &result, const Reader &read) const
    -> std::string {
  if (!scoped_)
    return result;
  auto envelope = json::Object(result);
  if (json::Text(envelope, "reply") != "ProjectSnapshot")
    return result;
  const auto snapshot = json::Object(json::Get(envelope, "value", "{}"));
  json::Members filtered;
  for (const auto *key :
       {"project_id", "rev", "execution_rev", "execution_services"})
    if (snapshot.contains(key))
      filtered[key] = snapshot.at(key);
  // A scoped inspection reveals neither other Cards, their diagnostics,
  // layout, run state nor references embedded in the project document.
  std::vector<std::string> definitions, sources, resolution;
  for (const auto &raw : json::Array(json::Get(snapshot, "definitions", "[]")))
    if (reads_.contains(json::Text(json::Object(raw), "id")))
      definitions.push_back(raw);
  for (const auto &raw : json::Array(json::Get(snapshot, "sources", "[]"))) {
    try {
      File(json::Text(json::Object(raw), "path"), false, read);
      sources.push_back(raw);
    } catch (const std::exception &) {
    }
  }
  for (const auto &raw : json::Array(json::Get(snapshot, "resolution", "[]"))) {
    auto value = json::Object(raw);
    if (!reads_.contains(Card(json::Get(value, "card", "{}"))))
      continue;
    value["diagnostics"] = "[]";
    std::vector<std::string> references;
    for (const auto &ref :
         json::Array(json::Get(value, "nix_references", "[]")))
      if (reads_.contains(Card(json::Get(json::Object(ref), "target", "{}"))))
        references.push_back(ref);
    value["nix_references"] = json::Encode(references);
    resolution.push_back(json::Encode(value));
  }
  std::vector<std::string> readable, writable;
  for (const auto &card : reads_)
    readable.push_back(json::Quote(card));
  for (const auto &card : writes_)
    writable.push_back(json::Quote(card));
  filtered["helper_scope"] =
      json::Encode(json::Members{{"read_cards", json::Encode(readable)},
                                 {"write_cards", json::Encode(writable)}});
  filtered["definitions"] = json::Encode(definitions);
  filtered["sources"] = json::Encode(sources);
  filtered["resolution"] = json::Encode(resolution);
  filtered["diagnostics"] = "[]";
  filtered["layout_json"] = json::Quote("{}");
  filtered["run"] = "{}";
  auto project = json::Object(json::Text(snapshot, "project_json", "{}"));
  json::Members visible;
  for (const auto *key : {"name", "pkgsPath"})
    if (project.contains(key))
      visible[key] = project.at(key);
  std::vector<std::string> cards, wires;
  const auto canvas = json::Object(json::Get(project, "canvas", "{}"));
  for (const auto &raw : json::Array(json::Get(canvas, "cards", "[]")))
    if (reads_.contains(json::Text(json::Object(raw), "id")))
      cards.push_back(raw);
  for (const auto &raw : json::Array(json::Get(canvas, "connections", "[]"))) {
    const auto wire = json::Object(raw);
    if (reads_.contains(
            json::Text(json::Object(json::Get(wire, "from", "{}")), "card")) &&
        reads_.contains(
            json::Text(json::Object(json::Get(wire, "to", "{}")), "card")))
      wires.push_back(raw);
  }
  visible["canvas"] = json::Encode(json::Members{
      {"cards", json::Encode(cards)}, {"connections", json::Encode(wires)}});
  filtered["project_json"] = json::Quote(json::Encode(visible));
  envelope["value"] = json::Encode(filtered);
  return json::Encode(envelope);
}
} // namespace tsugumi::agent
