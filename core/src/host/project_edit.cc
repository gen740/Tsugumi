#include "host/project_edit.hh"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <utility>

#include "host/document_edits.hh"
#include "host/failure.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;
namespace json = agent::json;
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
auto SourceBytes(const std::vector<uint8_t>& value) -> std::string {
  return std::string(value.begin(), value.end());
}
auto EncodedBytes(const std::string& bytes) -> std::string {
  return json::Encode(
      json::Members{{"base64", json::Quote(crypto::base64(bytes))}});
}
auto Change(const std::string& kind, json::Members value) -> std::string {
  return json::Encode(json::Members{{"value_type", json::Quote(kind)},
                                    {"value", json::Encode(value)}});
}
}  // namespace

auto EditProjectField(const std::string& document,
                      const H::ProjectFieldEditT& edit) -> std::string {
  ValidJson(document);
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
      throw Failure("invalid_request",
                    edit.file_path +
                        ": selector requires exactly one field or keyed match");
    json::Members keys;
    if (!step->match_json.empty()) {
      keys = json::Object(step->match_json);
      if (keys.empty() || target->kind != '[')
        throw Failure("invalid_request",
                      edit.file_path +
                          ": keyed match requires an array and nonempty keys");
    } else if (target->kind != '{') {
      throw Failure("invalid_request",
                    edit.file_path + ": field selector requires an object");
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
              edit.file_path + ": selector matched more than one entity");
        target = &child;
        index = n;
      }
    }
    if (!target) {
      if (step_index + 1 != edit.selector.size())
        throw Failure("target_missing",
                      edit.file_path + ": selector parent is missing");
      if (edit.old_value->present)
        throw Failure("value_conflict",
                      edit.file_path + ": expected value is missing");
      if (!edit.new_value->present) return document;
      if (!keys.empty() && !Matches(edit.new_value->json,
                                    Spans(edit.new_value->json).Value(), keys))
        throw Failure(
            "invalid_request",
            edit.file_path +
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
                  edit.file_path + ": selected value differs from old_value");
  auto result = document;
  if (edit.new_value->present) {
    // Replacing a keyed entity must preserve the identity used to select it.
    if (!edit.selector.empty() && !edit.selector.back()->match_json.empty() &&
        !Matches(edit.new_value->json, Spans(edit.new_value->json).Value(),
                 json::Object(edit.selector.back()->match_json)))
      throw Failure(
          "invalid_request",
          edit.file_path +
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

auto PrepareProjectEdits(
    const H::ProjectEditsT& edits, const SaveBase& base,
    const EditFileRead& read,
    const std::function<void(const std::string&)>& authorize) -> json::Members {
  if (edits.edits.empty())
    throw Failure("invalid_request", "project_edit requires at least one edit");
  std::map<std::string, std::optional<std::string>> originals, files;
  auto file = [&](const std::string& path) -> std::optional<std::string>& {
    if (authorize) authorize(path);
    if (!fs::valid_relative_path(path)) throw Failure("invalid_path", path);
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
      throw Failure("source_changed",
                    path + ": expected file hash/presence differs");
  };
  for (const auto& entry : edits.edits) {
    if (!entry) throw Failure("invalid_request", "empty project edit");
    if (const auto* field = entry->value.AsProjectFieldEdit()) {
      auto& text = file(field->file_path);
      if (!text) throw Failure("target_missing", field->file_path);
      text = EditProjectField(*text, *field);
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
  std::vector<std::string> changes;
  json::Members definitions;
  for (const auto& [path, content] : files) {
    if (content == originals.at(path)) continue;
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
      if (!content)
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
