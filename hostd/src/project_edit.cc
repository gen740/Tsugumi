#include "host/project_edit.hh"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <utility>

#include "host/document_edits.hh"
#include "host/failure.hh"
#include "host/layout_saving.hh"
#include "tsugumi/document/document.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;
namespace json = ::tsugumi::json;
struct Span {
  size_t start = 0, end = 0, member_start = 0;
  std::string key;
  char kind = 0;
  std::vector<Span> children;
};
// Grammar and UTF-8 have already been validated by schema::decode. The scanner
// only locates spans; it never converts a JSON number to a machine number.
class Spans {
 public:
  explicit Spans(const std::string& text) : text_(text) {}
  auto Value() -> Span {
    Space();
    Span node;
    node.start = pos_;
    node.member_start = pos_;
    node.kind = text_.at(pos_);
    if (node.kind == '{' || node.kind == '[') {
      const auto end = node.kind == '{' ? '}' : ']';
      ++pos_;
      Space();
      while (text_.at(pos_) != end) {
        const auto start = pos_;
        std::string key;
        if (node.kind == '{') {
          const auto key_start = pos_;
          String();
          key = json::String(text_.substr(key_start, pos_ - key_start));
          Space();
          ++pos_;
        }
        auto child = Value();
        child.key = std::move(key);
        child.member_start = start;
        node.children.push_back(std::move(child));
        Space();
        if (text_.at(pos_) == end) break;
        ++pos_;
        Space();
      }
      ++pos_;
    } else if (node.kind == '"') {
      String();
    } else {
      while (pos_ < text_.size() &&
             !std::isspace(static_cast<unsigned char>(text_[pos_])) &&
             text_[pos_] != ',' && text_[pos_] != ']' && text_[pos_] != '}')
        ++pos_;
    }
    node.end = pos_;
    return node;
  }

 private:
  auto Space() -> void {
    while (pos_ < text_.size() &&
           std::isspace(static_cast<unsigned char>(text_[pos_])))
      ++pos_;
  }
  auto String() -> void {
    ++pos_;
    while (text_.at(pos_) != '"') {
      if (text_[pos_] == '\\') ++pos_;
      ++pos_;
    }
    ++pos_;
  }
  const std::string& text_;
  size_t pos_ = 0;
};
auto ValidJson(const std::string& value) -> void {
  schema::Json parsed;
  if (auto error = schema::decode(value, parsed))
    throw Failure("validation_failed", error->message);
}
auto Matches(const std::string& document, const Span& child,
             const json::Members& keys) -> bool {
  if (child.kind != '{') return false;
  for (const auto& [key, value] : keys) {
    auto found =
        std::find_if(child.children.begin(), child.children.end(),
                     [&](const auto& member) { return member.key == key; });
    if (found == child.children.end() ||
        !DocumentsEqual(
            document.substr(found->start, found->end - found->start), value))
      return false;
  }
  return true;
}
// Remember explicitly authored input ends, including a no-op port "" edit.
// Looking at only the final document would lose that explicit choice.
auto MentionedInputs(const std::string& document,
                     const H::ProjectFieldEditT& edit)
    -> std::set<std::string> {
  const auto tree = Spans(document).Value();
  const Span* target = &tree;
  for (const auto& step : edit.selector) {
    if (!step) return {};
    const Span* next = nullptr;
    for (const auto& child : target->children)
      if (step->match_json.empty()
              ? child.key == step->field
              : Matches(document, child, json::Object(step->match_json))) {
        next = &child;
        break;
      }
    if (!next) return {};
    target = next;
  }
  std::set<std::string> cards;
  for (const auto& canvas : tree.children) {
    if (canvas.key != "canvas") continue;
    for (const auto& connections : canvas.children) {
      if (connections.key != "connections") continue;
      for (const auto& wire : connections.children)
        for (const auto& end : wire.children)
          if (end.key == "to" && target->start < end.end &&
              end.start < target->end)
            cards.insert(json::Text(
                json::Object(document.substr(end.start, end.end - end.start)),
                "card"));
    }
  }
  return cards;
}
auto SourceBytes(const std::vector<uint8_t>& value) -> std::string {
  return std::string(value.begin(), value.end());
}
auto EncodedBytes(const std::string& bytes) -> std::string {
  return json::Encode(
      json::Members{{"base64", json::Quote(crypto::base64(bytes))}});
}
auto Paths(const SaveBase& base) -> std::set<std::string> {
  std::set<std::string> paths;
  for (const auto& [path, ignored] : base.files) {
    (void)ignored;
    paths.insert(path);
  }
  for (const auto& raw : json::Array(json::Get(base.snapshot, "sources", "[]")))
    paths.insert(json::Text(json::Object(raw), "path"));
  return paths;
}
auto TreePrefix(const std::string& prefix) -> bool {
  if (!prefix.ends_with('/') || !fs::valid_relative_path(prefix + "file"))
    return false;
  const auto at = prefix.rfind("definitions/");
  return IsExternalDocument(prefix + "tsugumi.json", "tsugumi.json") ||
         (at != std::string::npos &&
          (at == 0 || IsExternalDocument(prefix.substr(0, at) + "tsugumi.json",
                                         "tsugumi.json")) &&
          prefix.find('/', at + 12) == prefix.size() - 1);
}
auto Change(const std::string& kind, json::Members value) -> std::string {
  return json::Encode(json::Members{{"value_type", json::Quote(kind)},
                                    {"value", json::Encode(value)}});
}
}  // namespace

auto EditProjectField(const std::string& document,
                      const H::ProjectFieldEditT& edit) -> std::string {
  ValidJson(document);
  std::string target_name = edit.file_path;
  for (const auto& step : edit.selector) {
    if (!step) continue;
    target_name += step->match_json.empty() ? "." + step->field
                                            : "[" + step->match_json + "]";
  }
  if (!edit.old_value || !edit.new_value)
    throw Failure("invalid_request",
                  "old_value and new_value require explicit presence");
  if (edit.old_value->present) ValidJson(edit.old_value->json);
  if (edit.new_value->present) ValidJson(edit.new_value->json);
  auto tree = Spans(document).Value();
  const Span* target = &tree;
  const Span* parent = nullptr;
  size_t index = 0;
  for (size_t step_index = 0; step_index < edit.selector.size(); ++step_index) {
    const auto& step = edit.selector[step_index];
    if (!step || (step->field.empty() == step->match_json.empty()))
      throw Failure(
          "invalid_request",
          target_name + ": selector requires exactly one field or keyed match");
    json::Members keys;
    if (!step->match_json.empty()) {
      keys = json::Object(step->match_json, edit.file_path + ".selector[" +
                                                std::to_string(step_index) +
                                                "].match");
      if (keys.empty() || target->kind != '[')
        throw Failure(
            "invalid_request",
            target_name + ": keyed match requires an array and nonempty keys");
    } else if (target->kind != '{') {
      throw Failure("invalid_request",
                    target_name + ": field selector requires an object");
    }
    parent = target;
    target = nullptr;
    for (size_t n = 0; n < parent->children.size(); ++n) {
      const auto& child = parent->children[n];
      if (keys.empty() ? child.key == step->field
                       : Matches(document, child, keys)) {
        if (target)
          throw Failure(
              "value_conflict",
              target_name + ": selector matched more than one entity");
        target = &child;
        index = n;
      }
    }
    if (!target) {
      if (step_index + 1 != edit.selector.size()) {
        if (edit.old_value->present)
          throw Failure("target_missing",
                        target_name + ": selector parent is missing");
        if (!edit.new_value->present) return document;
        // An absent leaf may create its absent containers. On reinspection
        // existing containers are traversed, so another entity is preserved.
        auto inserted = edit;
        auto value = edit.new_value->json;
        for (size_t i = edit.selector.size(); i > step_index + 1; --i) {
          const auto& child = edit.selector[i - 1];
          if (!child || (child->field.empty() == child->match_json.empty()))
            throw Failure("invalid_request",
                          target_name + ": invalid selector");
          if (child->match_json.empty())
            value = json::Encode(json::Members{{child->field, value}});
          else {
            if (!Matches(value, Spans(value).Value(),
                         json::Object(child->match_json)))
              throw Failure(
                  "invalid_request",
                  target_name +
                      ": inserted entity does not match its selector");
            value = json::Encode(std::vector<std::string>{value});
          }
        }
        inserted.selector.resize(step_index + 1);
        inserted.new_value->json = value;
        return EditProjectField(document, inserted);
      }
      if (edit.old_value->present)
        throw Failure("value_conflict",
                      target_name +
                          ": selected value is missing; write old_value as "
                          "{\"present\":false} to insert it");
      if (!edit.new_value->present) return document;
      if (!keys.empty() && !Matches(edit.new_value->json,
                                    Spans(edit.new_value->json).Value(), keys))
        throw Failure(
            "invalid_request",
            target_name +
                ": inserted entity does not match its keyed selector");
      auto inserted = (parent->children.empty() ? "" : ",") +
                      (keys.empty() ? json::Quote(step->field) + ':' : "") +
                      edit.new_value->json;
      auto result = document;
      result.insert(parent->end - 1, inserted);
      return result;
    }
  }
  const auto current =
      document.substr(target->start, target->end - target->start);
  if (!edit.old_value->present ||
      !DocumentsEqual(current, edit.old_value->json))
    throw Failure("value_conflict",
                  target_name +
                      ": selected value differs from old_value; reread the "
                      "selected field and write old_value as {\"present\":true,"
                      "\"value\":<current value>} before reapplying new_value");
  auto result = document;
  if (edit.new_value->present) {
    // Replacing a keyed entity must preserve the identity used to select it.
    if (!edit.selector.empty() && !edit.selector.back()->match_json.empty() &&
        !Matches(edit.new_value->json, Spans(edit.new_value->json).Value(),
                 json::Object(edit.selector.back()->match_json)))
      throw Failure(
          "invalid_request",
          target_name +
              ": replacement changes its permanent selector identity");
    result.replace(target->start, target->end - target->start,
                   edit.new_value->json);
  } else {
    if (!parent)
      throw Failure("invalid_request",
                    "use guarded file removal to delete a document");
    auto start = target->member_start;
    auto end = target->end;
    if (index + 1 < parent->children.size())
      end = parent->children[index + 1].member_start;
    else if (index > 0)
      start = parent->children[index - 1].end;
    result.erase(start, end - start);
  }
  ValidJson(result);
  return result;
}

auto PreserveProjectBytes(const std::string& before, const std::string& after)
    -> std::string {
  if (DocumentsEqual(before, after)) return before;
  if (!before.starts_with('{') &&
      before.find_first_not_of(" \n\r\t") == std::string::npos)
    return after;
  const auto old_tree = Spans(before).Value();
  const auto new_tree = Spans(after).Value();
  if (old_tree.kind == '[' && new_tree.kind == '[' &&
      old_tree.children.size() == new_tree.children.size()) {
    auto result = before;
    for (size_t i = old_tree.children.size(); i > 0; --i) {
      const auto& old_item = old_tree.children[i - 1];
      const auto& new_item = new_tree.children[i - 1];
      result.replace(
          old_item.start, old_item.end - old_item.start,
          PreserveProjectBytes(
              before.substr(old_item.start, old_item.end - old_item.start),
              after.substr(new_item.start, new_item.end - new_item.start)));
    }
    return result;
  }
  if (old_tree.kind != '{' || new_tree.kind != '{') return after;
  auto result = before;
  const auto old_fields = json::Object(before),
             new_fields = json::Object(after);
  std::set<std::string> keys;
  for (const auto& [key, value] : old_fields) {
    (void)value;
    keys.insert(key);
  }
  for (const auto& [key, value] : new_fields) {
    (void)value;
    keys.insert(key);
  }
  for (const auto& key : keys) {
    H::ProjectFieldEditT edit;
    auto selector = std::make_unique<H::ProjectSelectorT>();
    selector->field = key;
    edit.selector.push_back(std::move(selector));
    edit.old_value = std::make_unique<H::ProjectValueT>();
    edit.new_value = std::make_unique<H::ProjectValueT>();
    edit.old_value->present = old_fields.contains(key);
    edit.new_value->present = new_fields.contains(key);
    if (edit.old_value->present) edit.old_value->json = old_fields.at(key);
    if (edit.new_value->present) {
      edit.new_value->json = new_fields.at(key);
      if (edit.old_value->present)
        edit.new_value->json =
            PreserveProjectBytes(edit.old_value->json, edit.new_value->json);
    }
    if (edit.old_value->present && edit.new_value->present &&
        edit.old_value->json == edit.new_value->json)
      continue;
    result = EditProjectField(result, edit);
  }
  return result;
}

namespace {
// JSON scalars remain text, including worker values above 2^53.
auto ReverseFields(const std::string& path,
                   const std::optional<std::string>& now,
                   const std::optional<std::string>& previous,
                   const std::vector<H::ProjectSelectorT>& selector,
                   H::ProjectEditsT& result) -> void {
  if (now && previous && DocumentsEqual(*now, *previous)) return;
  auto kind = [](const std::string& text) {
    const auto at = text.find_first_not_of(" \n\r\t");
    return at == std::string::npos ? char{} : text[at];
  };
  if ((now.has_value() != previous.has_value()) &&
      kind(now ? *now : *previous) == '[') {
    const auto items = json::Array(now ? *now : *previous);
    auto keyed = [&](const std::string& key) {
      std::set<std::string> ids;
      for (const auto& raw : items) {
        if (kind(raw) != '{') return false;
        const auto fields = json::Object(raw);
        if (!fields.contains(key) || !ids.insert(fields.at(key)).second)
          return false;
      }
      return !items.empty();
    };
    if (keyed("id") || keyed("name")) {
      ReverseFields(path, now.value_or("[]"), previous.value_or("[]"), selector,
                    result);
      return;
    }
  }
  if (now && previous && kind(*now) == '{' && kind(*previous) == '{') {
    const auto a = json::Object(*now), b = json::Object(*previous);
    std::set<std::string> keys;
    for (const auto& [key, ignored] : a) {
      (void)ignored;
      keys.insert(key);
    }
    for (const auto& [key, ignored] : b) {
      (void)ignored;
      keys.insert(key);
    }
    for (const auto& key : keys) {
      auto steps = selector;
      H::ProjectSelectorT step;
      step.field = key;
      steps.push_back(std::move(step));
      ReverseFields(path,
                    a.contains(key) ? std::optional(a.at(key)) : std::nullopt,
                    b.contains(key) ? std::optional(b.at(key)) : std::nullopt,
                    steps, result);
    }
    return;
  }
  if (now && previous && kind(*now) == '[' && kind(*previous) == '[') {
    const auto a = json::Array(*now), b = json::Array(*previous);
    std::map<std::string, std::string> aa, bb;
    std::string identity = "id";
    auto has_key = [&](const std::string& key) {
      for (const auto* list : {&a, &b})
        for (const auto& item : *list)
          if (kind(item) != '{' || !json::Object(item).contains(key))
            return false;
      return true;
    };
    if (!has_key(identity) && has_key("name")) identity = "name";
    auto keyed = [&](const auto& list, auto& indexed) {
      for (const auto& item : list) {
        if (kind(item) != '{') return false;
        const auto fields = json::Object(item);
        if (!fields.contains(identity) ||
            !indexed.emplace(fields.at(identity), item).second)
          return false;
      }
      return true;
    };
    // Reordering is a change too. Use keyed fields only when the shared
    // entities retain their relative order; otherwise guard the entire array.
    auto shared_order = [&](const auto& list, const auto& other) {
      std::vector<std::string> ids;
      for (const auto& item : list) {
        const auto id = json::Get(json::Object(item), identity);
        if (other.contains(id)) ids.push_back(id);
      }
      return ids;
    };
    if (keyed(a, aa) && keyed(b, bb) &&
        shared_order(a, bb) == shared_order(b, aa)) {
      // Insertions at a specific array offset cannot be expressed by a keyed
      // selector. A restore of deleted entities must retain document order.
      bool appends_only = true, missing_seen = false;
      for (const auto& item : b) {
        const auto id = json::Get(json::Object(item), identity);
        if (!aa.contains(id))
          missing_seen = true;
        else if (missing_seen)
          appends_only = false;
      }
      if (appends_only) {
        std::vector<std::string> ids;
        for (const auto& item : a)
          ids.push_back(json::Get(json::Object(item), identity));
        for (const auto& item : b) {
          const auto id = json::Get(json::Object(item), identity);
          if (!aa.contains(id)) ids.push_back(id);
        }
        for (const auto& id : ids) {
          auto steps = selector;
          H::ProjectSelectorT step;
          step.match_json = json::Encode(json::Members{{identity, id}});
          steps.push_back(std::move(step));
          ReverseFields(
              path, aa.contains(id) ? std::optional(aa.at(id)) : std::nullopt,
              bb.contains(id) ? std::optional(bb.at(id)) : std::nullopt, steps,
              result);
        }
        return;
      }
    }
  }
  H::ProjectFieldEditT edit;
  edit.file_path = path;
  for (const auto& step : selector)
    edit.selector.push_back(std::make_unique<H::ProjectSelectorT>(step));
  edit.old_value = std::make_unique<H::ProjectValueT>();
  edit.new_value = std::make_unique<H::ProjectValueT>();
  edit.old_value->present = now.has_value();
  edit.new_value->present = previous.has_value();
  if (now) edit.old_value->json = *now;
  if (previous) edit.new_value->json = *previous;
  auto entry = std::make_unique<H::ProjectEditT>();
  entry->value.Set(std::move(edit));
  result.edits.push_back(std::move(entry));
}
}  // namespace

auto ReverseProjectChanges(const json::Members& plan, const SaveBase& base,
                           const EditFileRead& read) -> H::ProjectEditsT {
  std::map<std::string, std::optional<std::string>> after;
  auto original = [&](const std::string& path) -> std::optional<std::string> {
    if (base.files.contains(path)) return base.files.at(path);
    return read(path);
  };
  for (const auto& raw : json::Array(json::Get(plan, "changes", "[]"))) {
    const auto change = json::Object(raw);
    const auto type = json::Text(change, "value_type");
    const auto value = json::Object(json::Get(change, "value", "{}"));
    if (type == "SetProject" || type == "SetLayout")
      after[type == "SetProject" ? "tsugumi.json" : "layout.json"] =
          json::Text(value, "json");
    else if (type == "PutDefinition") {
      const auto dir = "definitions/" + json::Text(value, "id") + '/';
      for (const auto& [key, file] : {std::pair{"card_json", "_card.json"},
                                      {"meta_json", "_meta.json"},
                                      {"link_json", "_link.json"}}) {
        // PutDefinition replaces all three documents; omission means removal.
        const auto bytes = json::Text(value, key);
        after[dir + file] = bytes.empty() ? std::nullopt : std::optional(bytes);
      }
    } else if (type == "RemoveDefinition") {
      const auto dir = "definitions/" + json::Text(value, "id") + '/';
      for (const auto& [path, ignored] : base.files) {
        (void)ignored;
        if (path.starts_with(dir)) after[path] = std::nullopt;
      }
      for (const auto& raw_source :
           json::Array(json::Get(base.snapshot, "sources", "[]"))) {
        const auto path = json::Text(json::Object(raw_source), "path");
        if (path.starts_with(dir)) after[path] = std::nullopt;
      }
    } else if (type == "PutSource") {
      const auto bytes = json::Get(value, "bytes");
      std::string decoded;
      if (bytes.starts_with('"'))
        decoded = json::String(bytes);
      else if (!crypto::base64_decode(json::Text(json::Object(bytes), "base64"),
                                      decoded))
        throw Failure("invalid_request",
                      "invalid file bytes in operation plan");
      after[json::Text(value, "path")] = std::move(decoded);
    } else if (type == "RemoveSource")
      after[json::Text(value, "path")] = std::nullopt;
    else
      throw Failure("invalid_request", "unsupported operation change: " + type);
  }
  H::ProjectEditsT result;
  // Creation/removal of a structural root reverses its whole folder, including
  // files added by a later writer. Guard the closure as well as every byte.
  std::set<std::string> trees;
  for (const auto& [path, now] : after) {
    if (now.has_value() == original(path).has_value()) continue;
    std::string prefix;
    if (IsExternalDocument(path, "tsugumi.json"))
      prefix = path.substr(0, path.size() - 12);
    else if (path.ends_with("/_card.json") || path.ends_with("/_link.json"))
      prefix = path.substr(0, path.size() - 10);
    if (!prefix.empty()) trees.insert(prefix);
  }
  auto paths = Paths(base);
  for (const auto& [path, ignored] : after) {
    (void)ignored;
    paths.insert(path);
  }
  std::set<std::string> covered;
  for (const auto& prefix : trees) {
    if (std::any_of(covered.begin(), covered.end(), [&](const auto& parent) {
          return prefix.starts_with(parent);
        }))
      continue;
    H::ProjectTreeEditT tree;
    tree.prefix = prefix;
    for (const auto& path : paths) {
      if (!path.starts_with(prefix)) continue;
      const auto previous = original(path);
      const auto now = after.contains(path) ? after.at(path) : previous;
      if (now) {
        auto hash = std::make_unique<H::ProjectFileHashT>();
        hash->path = path;
        hash->hash = crypto::sha256_hex(*now);
        tree.expected.push_back(std::move(hash));
      }
      if (previous) {
        auto file = std::make_unique<H::ProjectFileT>();
        file->path = path;
        file->bytes.assign(previous->begin(), previous->end());
        tree.files.push_back(std::move(file));
      }
    }
    auto entry = std::make_unique<H::ProjectEditT>();
    entry->value.Set(std::move(tree));
    result.edits.push_back(std::move(entry));
    covered.insert(prefix);
  }
  for (const auto& [path, now] : after) {
    if (std::any_of(covered.begin(), covered.end(), [&](const auto& prefix) {
          return path.starts_with(prefix);
        }))
      continue;
    const auto previous = original(path);
    if (now == previous) continue;
    const bool document = path == "tsugumi.json" || path == "layout.json" ||
                          IsExternalDocument(path, "tsugumi.json") ||
                          IsExternalDocument(path, "layout.json") ||
                          path.ends_with("/_card.json") ||
                          path.ends_with("/_meta.json") ||
                          path.ends_with("/_link.json");
    if (document && now && previous) {
      ReverseFields(path, now, previous, {}, result);
    } else {
      auto entry = std::make_unique<H::ProjectEditT>();
      if (previous) {
        H::ProjectFilePutT edit;
        edit.file_path = path;
        edit.expect_missing = !now;
        if (now) edit.expected_hash = crypto::sha256_hex(*now);
        edit.bytes.assign(previous->begin(), previous->end());
        entry->value.Set(std::move(edit));
      } else {
        H::ProjectFileRemoveT edit;
        edit.file_path = path;
        edit.expected_hash = crypto::sha256_hex(*now);
        entry->value.Set(std::move(edit));
      }
      result.edits.push_back(std::move(entry));
    }
  }
  return result;
}

auto PrepareProjectEdits(
    const H::ProjectEditsT& edits, const SaveBase& base,
    const EditFileRead& read,
    const std::function<void(const std::string&)>& authorize) -> json::Members {
  // Expand form edits here so Mac and Web share field/entity selection.
  H::ProjectEditsT expanded;
  bool has_json = false;
  for (const auto& entry : edits.edits) {
    if (entry && entry->value.AsProjectJSONEdit()) {
      has_json = true;
      const auto& edit = *entry->value.AsProjectJSONEdit();
      if (authorize) authorize(edit.file_path);
      if (!edit.old_value || !edit.new_value)
        throw Failure("invalid_request", "missing JSON edit values");
      const auto before = edit.old_value->present
                              ? std::optional(edit.old_value->json)
                              : std::nullopt;
      const auto after = edit.new_value->present
                             ? std::optional(edit.new_value->json)
                             : std::nullopt;
      const auto current = base.files.contains(edit.file_path)
                               ? std::optional(base.files.at(edit.file_path))
                               : read(edit.file_path);
      if (!current && after) {
        if (before) throw Failure("target_missing", edit.file_path);
        H::ProjectFilePutT put;
        put.file_path = edit.file_path;
        put.expect_missing = true;
        put.bytes.assign(after->begin(), after->end());
        auto value = std::make_unique<H::ProjectEditT>();
        value->value.Set(std::move(put));
        expanded.edits.push_back(std::move(value));
      } else if (before && !after) {
        if (!current || !DocumentsEqual(*current, *before))
          throw Failure("value_conflict",
                        edit.file_path + ": document changed");
        H::ProjectFileRemoveT remove;
        remove.file_path = edit.file_path;
        remove.expected_hash = crypto::sha256_hex(*current);
        auto value = std::make_unique<H::ProjectEditT>();
        value->value.Set(std::move(remove));
        expanded.edits.push_back(std::move(value));
      } else {
        const auto selected =
            !before && current && after &&
                    after->find_first_not_of(" \n\r\t") != std::string::npos &&
                    (*after)[after->find_first_not_of(" \n\r\t")] == '{'
                ? std::optional<std::string>("{}")
                : before;
        ReverseFields(edit.file_path, selected, after, {}, expanded);
      }
    } else if (entry) {
      expanded.edits.push_back(std::make_unique<H::ProjectEditT>(*entry));
    } else {
      throw Failure("invalid_request", "empty project edit");
    }
  }
  if (has_json) {
    if (expanded.edits.empty())
      return {{"base_rev", json::Get(base.snapshot, "rev")}, {"changes", "[]"}};
    return PrepareProjectEdits(expanded, base, read, authorize);
  }
  if (edits.edits.empty())
    throw Failure("invalid_request", "project_edit requires at least one edit");
  std::map<std::string, std::optional<std::string>> originals, files;
  std::map<std::string, std::set<std::string>> mentioned_inputs;
  std::set<std::string> replaced_projects, removed_definitions;
  const auto known = Paths(base);
  auto file = [&](const std::string& path) -> std::optional<std::string>& {
    if (authorize) authorize(path);
    if (!files.contains(path)) {
      const auto found = base.files.find(path);
      originals[path] =
          found == base.files.end() ? read(path) : std::optional(found->second);
      files[path] = originals[path];
    }
    return files.at(path);
  };
  auto guard = [&](const std::string& path,
                   const std::optional<std::string>& value,
                   const std::string& hash, bool missing) {
    if (missing
            ? (!hash.empty() || value.has_value())
            : (hash.empty() || !value || crypto::sha256_hex(*value) != hash))
      throw Failure(
          "source_changed",
          path + (!value
                      ? ": file is missing; resend put with "
                        "expect_missing:true and no expected_hash"
                      : (hash.empty() ? ": file already exists; read it and "
                                        "send its expected_hash"
                                      : ": file exists with other content; "
                                        "read it and send its expected_hash")));
  };
  for (const auto& entry : edits.edits) {
    if (!entry) throw Failure("invalid_request", "empty project edit");
    if (!entry->value.AsProjectFieldEdit()) {
      const auto path = entry->value.AsProjectTextEdit()
                            ? entry->value.AsProjectTextEdit()->file_path
                        : entry->value.AsProjectFilePut()
                            ? entry->value.AsProjectFilePut()->file_path
                            : std::string{};
      if (path == "tsugumi.json" || path.ends_with("/tsugumi.json"))
        replaced_projects.insert(path);
    }
    if (const auto* tree = entry->value.AsProjectTreeEdit()) {
      if (!TreePrefix(tree->prefix))
        throw Failure("invalid_path", tree->prefix);
      std::map<std::string, std::string> expected, replacement;
      for (const auto& hash : tree->expected) {
        if (!hash || !hash->path.starts_with(tree->prefix) ||
            !expected.emplace(hash->path, hash->hash).second)
          throw Failure("invalid_request",
                        tree->prefix + ": invalid file closure");
      }
      std::set<std::string> actual;
      for (const auto& path : known)
        if (path.starts_with(tree->prefix)) actual.insert(path);
      for (const auto& [path, ignored] : expected) {
        (void)ignored;
        actual.erase(path);
      }
      if (!actual.empty())
        throw Failure("source_changed",
                      *actual.begin() + ": folder has a later file");
      for (const auto& [path, hash] : expected)
        guard(path, file(path), hash, false);
      for (const auto& item : tree->files) {
        if (!item || !item->path.starts_with(tree->prefix) ||
            !replacement.emplace(item->path, SourceBytes(item->bytes)).second)
          throw Failure("invalid_request",
                        tree->prefix + ": invalid replacement file");
        file(item->path);
      }
      for (const auto& [path, ignored] : expected) {
        (void)ignored;
        files[path].reset();
      }
      for (const auto& [path, bytes] : replacement) files[path] = bytes;
      if (tree->prefix.starts_with("definitions/") &&
          std::count(tree->prefix.begin(), tree->prefix.end(), '/') == 2 &&
          !replacement.contains(tree->prefix + "_card.json") &&
          !replacement.contains(tree->prefix + "_link.json")) {
        if (!replacement.empty())
          throw Failure("invalid_request",
                        tree->prefix + ": definition root is required");
        removed_definitions.insert(
            tree->prefix.substr(12, tree->prefix.size() - 13));
      }
    } else if (const auto* field = entry->value.AsProjectFieldEdit()) {
      auto& text = file(field->file_path);
      if (!text) throw Failure("target_missing", field->file_path);
      auto edited = EditProjectField(*text, *field);
      if (field->file_path == "tsugumi.json" ||
          field->file_path.ends_with("/tsugumi.json")) {
        for (const auto* version : {&*text, &edited}) {
          const auto mentioned = MentionedInputs(*version, *field);
          mentioned_inputs[field->file_path].insert(mentioned.begin(),
                                                    mentioned.end());
        }
      }
      text = std::move(edited);
    } else if (const auto* replace = entry->value.AsProjectTextEdit()) {
      auto& text = file(replace->file_path);
      if (!text) throw Failure("target_missing", replace->file_path);
      if (replace->old_string.empty() || replace->expected_matches == 0)
        throw Failure("invalid_request",
                      "old_string and expected_matches must be nonempty");
      size_t count = 0, offset = 0;
      while ((offset = text->find(replace->old_string, offset)) !=
             std::string::npos) {
        ++count;
        offset += replace->old_string.size();
      }
      if (count != replace->expected_matches)
        throw Failure("value_conflict",
                      replace->file_path + ": old_string match count differs");
      offset = 0;
      for (size_t n = 0; n < count; ++n) {
        offset = text->find(replace->old_string, offset);
        text->replace(offset, replace->old_string.size(), replace->new_string);
        offset += replace->new_string.size();
      }
    } else if (const auto* put = entry->value.AsProjectFilePut()) {
      auto& text = file(put->file_path);
      if (text || !put->expected_hash.empty() || put->expect_missing)
        guard(put->file_path, text, put->expected_hash, put->expect_missing);
      text = SourceBytes(put->bytes);
    } else if (const auto* remove = entry->value.AsProjectFileRemove()) {
      auto& text = file(remove->file_path);
      guard(remove->file_path, text, remove->expected_hash, false);
      text.reset();
    } else if (const auto* move = entry->value.AsProjectFileMove()) {
      auto& from = file(move->file_path);
      auto& to = file(move->to);
      if (move->file_path == move->to)
        throw Failure("invalid_request",
                      "file move source and destination are identical");
      guard(move->file_path, from, move->expected_hash, false);
      guard(move->to, to, move->expected_destination_hash,
            move->destination_missing);
      to = std::move(from);
      from.reset();
    } else
      throw Failure("invalid_request", "missing project edit body");
  }
  // A single input and a single incoming wire leave no routing choice.
  // Do this in the host's atomic plan only for Draft -> code, never in core
  // resolution, and never override an explicitly authored input end.
  std::vector<std::string> implemented;
  for (const auto& [path, content] : files)
    if (path.ends_with("/_card.json") &&
        Document::is_card_meta_file(path.substr(0, path.size() - 10) +
                                    "_meta.json") &&
        content && originals.at(path) &&
        json::Text(json::Object(*originals.at(path), path), "template") ==
            "draft")
      implemented.push_back(path);
  for (const auto& path : implemented) {
    const auto definition = json::Object(*files.at(path), path);
    const auto kind = json::Text(definition, "template");
    if (!kind.empty() && kind != "none") continue;
    const auto inputs =
        json::Array(json::Get(definition, "inputs", "[]"), path + ".inputs");
    if (inputs.size() != 1) continue;
    const auto port =
        json::Text(json::Object(inputs.front(), path + ".inputs[0]"), "name");
    if (!valid_id(port)) continue;
    const auto at = path.rfind("definitions/");
    if (at == std::string::npos) continue;
    const auto id = path.substr(at + 12, path.size() - at - 23);
    if (id.find('/') != std::string::npos) continue;
    const auto project_path = path.substr(0, at) + "tsugumi.json";
    if (replaced_projects.contains(project_path) ||
        mentioned_inputs[project_path].contains(id))
      continue;
    auto& project = file(project_path);
    if (!project || file(path.substr(0, path.size() - 10) + "_link.json"))
      continue;
    auto root = json::Object(*project, project_path);
    auto canvas =
        json::Object(json::Get(root, "canvas", "{}"), project_path + ".canvas");
    auto wires = json::Array(json::Get(canvas, "connections", "[]"),
                             project_path + ".canvas.connections");
    size_t count = 0, candidate = 0;
    for (size_t i = 0; i < wires.size(); ++i) {
      const auto to =
          json::Object(json::Get(json::Object(wires[i]), "to", "{}"));
      if (json::Text(to, "card") == id) {
        ++count;
        candidate = i;
      }
    }
    if (count != 1) continue;
    auto wire = json::Object(wires[candidate]);
    auto to = json::Object(json::Get(wire, "to", "{}"));
    if (!json::Text(to, "port").empty()) continue;
    to["port"] = json::Quote(port);
    wire["to"] = json::Encode(to);
    wires[candidate] = json::Encode(wire);
    canvas["connections"] = json::Encode(wires);
    root["canvas"] = json::Encode(canvas);
    project = PreserveProjectBytes(*project, json::Encode(root));
  }
  std::vector<std::string> changes;
  json::Members definitions;
  for (const auto& [path, content] : files) {
    if (content == originals.at(path)) continue;
    if (path.starts_with("definitions/")) {
      const auto slash = path.find('/', 12);
      if (removed_definitions.contains(path.substr(12, slash - 12))) continue;
    }
    if (path == "tsugumi.json" || path == "layout.json") {
      if (!content)
        throw Failure("invalid_request", "root document cannot be removed");
      changes.push_back(
          Change(path == "tsugumi.json" ? "SetProject" : "SetLayout",
                 {{"json", json::Quote(*content)}}));
    } else if (path.starts_with("definitions/") &&
               std::count(path.begin(), path.end(), '/') == 2 &&
               (path.ends_with("/_card.json") ||
                path.ends_with("/_meta.json") ||
                path.ends_with("/_link.json"))) {
      if (!content && !path.ends_with("/_meta.json"))
        throw Failure("invalid_request",
                      "definition documents cannot be removed independently");
      const auto slash = path.find('/', 12);
      const auto id = path.substr(12, slash - 12);
      definitions[id] = "{}";
    } else {
      json::Members value{{"path", json::Quote(path)}};
      if (originals.at(path))
        value["base_hash"] =
            json::Quote(crypto::sha256_hex(*originals.at(path)));
      if (content) value["bytes"] = EncodedBytes(*content);
      changes.push_back(
          Change(content ? "PutSource" : "RemoveSource", std::move(value)));
    }
  }
  for (const auto& id : removed_definitions)
    changes.push_back(Change("RemoveDefinition", {{"id", json::Quote(id)}}));
  for (const auto& [id, ignored] : definitions) {
    (void)ignored;
    json::Members value{{"id", json::Quote(id)},
                        {"card_json", json::Quote("")}};
    for (const auto& [name, key] : {std::pair{"_card.json", "card_json"},
                                    {"_meta.json", "meta_json"},
                                    {"_link.json", "link_json"}}) {
      const auto path = "definitions/" + id + '/' + name;
      if (files.contains(path)) {
        if (files.at(path)) value[key] = json::Quote(*files.at(path));
      } else if (base.files.contains(path))
        value[key] = json::Quote(base.files.at(path));
    }
    changes.push_back(Change("PutDefinition", std::move(value)));
  }
  return {{"base_rev", json::Get(base.snapshot, "rev")},
          {"changes", json::Encode(changes)}};
}
}  // namespace tsugumi::host
