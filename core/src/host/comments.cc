#include "host/comments.hh"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

#include "schema.hh"
#include "util/crypto.hh"
#include <git2.h>

namespace tsugumi::host::comments {

namespace {

using Object = schema::v::Mapping<schema::v::String, schema::Json>;
constexpr const char *kAgent = "agent";

// By id, the Card's own first: a copied Card in Card has comments with the same
// ids.
const Comment *find(const std::vector<Comment> &list, const std::string &card,
                    const std::string &id) {
  const Comment *other = nullptr;
  for (const auto &c : list) {
    if (c.id != id)
      continue;
    if (c.card == card)
      return &c;
    if (!other)
      other = &c;
  }
  return other;
}

void set_field(auto &comment, const std::string &name,
               const std::optional<hosq::json::GenericDom<>> &value) {
  std::remove_reference_t<decltype(comment)> next;
  for (const auto &[key, field] : comment)
    if (key != name)
      next.emplace_back(key, field);
  if (value)
    next.emplace_back(name, *value);
  comment = std::move(next);
}

std::optional<hosq::json::GenericDom<>> text_value(const std::string &s) {
  if (s.empty())
    return std::nullopt;
  return schema::string_value(s);
}

} // namespace

// _meta.json with its comments stamped; the same text when there is nothing to
// stamp.
std::string stamped(const std::string &text, const std::string &card,
                    const std::vector<Comment> &before,
                    const std::vector<std::string> &changed) {
  Object root;
  if (schema::decode(schema::capture(text), root))
    return text;
  auto now = parse(card, text);
  bool any = false;
  for (auto &[name, value] : root.value()) {
    if (name != "comments")
      continue;
    schema::v::Array<Object> list;
    if (schema::decode(value, list) || list.value().size() != now.size())
      return text;
    std::set<std::string> ids;
    for (size_t i = 0; i < now.size(); ++i) {
      auto &c = now[i];
      auto &field = list.value()[i];
      std::string id = c.id;
      if (id.empty() || !ids.insert(id).second) {
        id = crypto::uuid4();
        ids.insert(id);
      }
      const Comment *was = id == c.id ? find(before, card, c.id) : nullptr;
      std::string author = was ? was->author : kAgent;
      std::string edited_by =
          was ? (was->text != c.text ? kAgent : was->edited_by) : std::string();
      bool state_changed = was ? was->resolved != c.resolved : c.resolved;
      std::string state_by = state_changed ? kAgent
                             : was         ? was->state_by
                                           : std::string();
      auto cards = state_changed
                       ? (c.resolved ? changed : std::vector<std::string>{})
                   : was ? was->cards
                         : std::vector<std::string>{};
      if (id == c.id && author == c.author && edited_by == c.edited_by &&
          state_by == c.state_by && cards == c.cards)
        continue;
      any = true;
      if (id != c.id)
        set_field(field, "id", text_value(id));
      set_field(field, "author", text_value(author));
      set_field(field, "editedBy", text_value(edited_by));
      set_field(field, "stateBy", text_value(state_by));
      if (cards.empty()) {
        set_field(field, "cards", std::nullopt);
      } else {
        schema::v::Array<schema::v::String> names;
        for (const auto &k : cards)
          names.value().push_back(k);
        set_field(field, "cards", schema::capture(schema::encode(names)));
      }
    }
    if (any)
      value = schema::capture(schema::encode(list));
  }
  return any ? schema::pretty_text(schema::encode(root)) : text;
}

std::vector<Comment> parse(const std::string &card,
                           const std::string &meta_json) {
  std::vector<Comment> out;
  using namespace agent;
  try {
    auto root = json::Object(meta_json);
    for (const auto &raw : json::Array(json::Get(root, "comments", "[]"))) {
      const auto c = json::Object(raw);
      Comment o;
      o.card = card;
      o.id = json::Text(c, "id");
      o.text = json::Text(c, "text");
      o.resolved = json::Bool(json::Get(c, "resolved"));
      o.resolution = json::Text(c, "resolution");
      o.author = json::Text(c, "author");
      o.edited_by = json::Text(c, "editedBy");
      o.state_by = json::Text(c, "stateBy");
      for (const auto &k : json::Array(json::Get(c, "cards", "[]")))
        o.cards.push_back(json::String(k));
      out.push_back(std::move(o));
    }
  } catch (const std::exception &) {
    return {};
  }
  return out;
}

auto of(const Files &files) -> std::vector<Comment> {
  std::vector<Comment> out;
  for (const auto &[path, text] : files) {
    if (!is_meta(path))
      continue;
    if (auto card = card_of(path, files)) {
      auto list = parse(*card, text);
      out.insert(out.end(), list.begin(), list.end());
    }
  }
  return out;
}

std::vector<Change> changes(const std::vector<Comment> &before,
                            const std::vector<Comment> &after) {
  std::vector<Change> out;
  for (const auto &c : after) {
    const Comment *was = find(before, c.card, c.id);
    if (!was) {
      if (c.resolved)
        out.push_back(
            {c.card, c.text, "resolved", c.state_by, c.resolution, c.cards});
      else
        out.push_back({c.card, c.text, "added", c.author, "", {}});
    } else if (was->resolved != c.resolved) {
      if (c.resolved)
        out.push_back(
            {c.card, c.text, "resolved", c.state_by, c.resolution, c.cards});
      else
        out.push_back({c.card, c.text, "reopened", c.state_by, "", {}});
    } else if (was->text != c.text) {
      out.push_back({c.card,
                     c.text,
                     "edited",
                     c.edited_by,
                     c.resolved ? c.resolution : "",
                     {}});
    }
  }
  for (const auto &c : before)
    if (!find(after, c.card, c.id))
      out.push_back({c.card, c.text, "removed", "", "", {}});
  std::stable_sort(
      out.begin(), out.end(),
      [](const Change &a, const Change &b) { return a.card < b.card; });
  return out;
}

auto is_meta(const std::string &path) -> bool {
  auto rel = path;
  while (rel.starts_with("externals/")) {
    const auto slash = rel.find('/', 10);
    if (slash == std::string::npos)
      return false;
    rel.erase(0, slash + 1);
  }
  if (!rel.starts_with("definitions/"))
    return false;
  const auto slash = rel.find('/', 12);
  return slash != std::string::npos && rel.substr(slash + 1) == "_meta.json";
}

auto card_of(const std::string &path, const Files &files)
    -> std::optional<std::string> {
  std::string rest = path, prefix, directory;
  while (rest.starts_with("externals/")) {
    auto slash = rest.find('/', 10);
    if (slash == std::string::npos)
      return std::nullopt;
    const auto external = rest.substr(10, slash - 10);
    auto id = external;
    for (const auto &[link, text] : files) {
      if (!link.starts_with(directory + "definitions/") ||
          !link.ends_with("/_link.json"))
        continue;
      auto local = link.substr(directory.size());
      if (local.find('/', 12) != local.size() - 11)
        continue;
      try {
        if (agent::json::Text(agent::json::Object(text), "external") ==
            external) {
          id = local.substr(12, local.size() - 23);
          break;
        }
      } catch (const std::exception &) {
      }
    }
    prefix += id + "/";
    directory += "externals/" + external + "/";
    rest.erase(0, slash + 1);
  }
  if (!rest.starts_with("definitions/"))
    return std::nullopt;
  const auto slash = rest.find('/', 12);
  if (slash == std::string::npos)
    return std::nullopt;
  return prefix + rest.substr(12, slash - 12);
}

auto record_json(const std::vector<Change> &changes) -> std::string {
  using namespace agent;
  std::vector<std::string> out;
  for (const auto &c : changes) {
    json::Members value{{"card", json::Quote(c.card)},
                        {"text", json::Quote(c.text)},
                        {"change", json::Quote(c.change)}};
    if (!c.by.empty())
      value["by"] = json::Quote(c.by);
    if (!c.resolution.empty())
      value["resolution"] = json::Quote(c.resolution);
    std::vector<std::string> cards;
    for (const auto &id : c.cards)
      cards.push_back(json::Quote(id));
    value["cards"] = json::Encode(cards);
    out.push_back(json::Encode(value));
  }
  return json::Encode(out);
}

auto version(const std::filesystem::path &root, const std::string &id)
    -> std::vector<Change> {
  if (id.size() != 40 ||
      id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    throw std::runtime_error("Use a full version object ID");
  git_libgit2_init();
  struct Repository {
    git_repository *value = nullptr;
    ~Repository() {
      git_repository_free(value);
      git_libgit2_shutdown();
    }
  } repo;
  if (git_repository_open_ext(&repo.value, root.c_str(),
                              GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr))
    throw std::runtime_error("Project has no version repository");
  git_oid oid;
  git_oid_fromstr(&oid, id.c_str());
  struct Commit {
    git_commit *value = nullptr;
    ~Commit() { git_commit_free(value); }
  } commit, parent;
  if (git_commit_lookup(&commit.value, repo.value, &oid))
    throw std::runtime_error("Version not found");
  struct Tree {
    git_tree *value = nullptr;
    ~Tree() { git_tree_free(value); }
  } now, before;
  if (git_commit_tree(&now.value, commit.value))
    throw std::runtime_error("Cannot read version tree");
  if (git_commit_parentcount(commit.value)) {
    if (git_commit_parent(&parent.value, commit.value, 0) ||
        git_commit_tree(&before.value, parent.value))
      throw std::runtime_error("Cannot read parent version tree");
  }
  auto read = [&](git_tree *tree) {
    Files files;
    if (!tree)
      return files;
    struct Walk {
      git_repository *repo;
      Files *files;
    } walk{repo.value, &files};
    git_tree_walk(
        tree, GIT_TREEWALK_PRE,
        [](const char *prefix, const git_tree_entry *entry, void *payload) {
          auto &w = *static_cast<Walk *>(payload);
          std::string path = std::string(prefix) + git_tree_entry_name(entry);
          if ((!path.ends_with("/_meta.json") &&
               !path.ends_with("/_link.json")) ||
              !card_of(path) || git_tree_entry_type(entry) != GIT_OBJECT_BLOB)
            return 0;
          git_blob *blob = nullptr;
          if (!git_blob_lookup(&blob, w.repo, git_tree_entry_id(entry))) {
            (*w.files)[path] = std::string(
                static_cast<const char *>(git_blob_rawcontent(blob)),
                git_blob_rawsize(blob));
            git_blob_free(blob);
          }
          return 0;
        },
        &walk);
    return files;
  };
  return changes(of(read(before.value)), of(read(now.value)));
}
} // namespace tsugumi::host::comments
