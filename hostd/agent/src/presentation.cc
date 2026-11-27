#include "agent/presentation.hh"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <tuple>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::agent {
namespace A = Tsugumi::Agent;
namespace {
using Rows = std::vector<std::unique_ptr<A::ChatRowT>>;
auto ObjectOrEmpty(std::string_view text) -> json::Members {
  try {
    return json::Object(text);
  } catch (const std::exception&) {
    return {};
  }
}
auto Join(const std::vector<std::string>& values) -> std::string {
  std::string out;
  for (const auto& value : values) {
    if (!out.empty()) out += ", ";
    out += value;
  }
  return out;
}
auto Scope(const json::Members& spec, bool english) -> std::string {
  std::vector<std::string> lines;
  for (const auto& [key, ja, en] :
       std::vector<std::tuple<std::string, std::string, std::string>>{
           {"read_cards", "読む", "Read"},
           {"write_cards", "変更", "Change"},
           {"run_cards", "実行", "Run"}}) {
    const auto raw = json::Get(spec, key);
    std::vector<std::string> cards;
    if (raw != "null")
      for (const auto& card : json::Array(raw))
        cards.push_back(json::String(card));
    lines.push_back((english ? en : ja) + ": " +
                    (raw == "null"   ? (english ? "unrestricted" : "制限なし")
                     : cards.empty() ? (english ? "none" : "なし")
                                     : Join(cards)));
  }
  return Join(lines);
}
auto AddLink(A::ChatRowT& row, const std::string& title, const std::string& url)
    -> void {
  if (std::ranges::any_of(row.links,
                          [&](const auto& link) { return link->url == url; }))
    return;
  auto link = std::make_unique<A::ChatLinkT>();
  link->title = title;
  link->url = url;
  row.links.push_back(std::move(link));
}
auto CardReference(std::string_view raw) -> std::string {
  if (const auto value = json::String(raw); !value.empty()) return value;
  const auto object = ObjectOrEmpty(raw);
  const auto segments = json::Get(object, "segments", "[]");
  std::string path;
  try {
    for (const auto& part : json::Array(segments)) {
      if (!path.empty()) path += '/';
      path += json::String(part);
    }
  } catch (const std::exception&) {
    return {};
  }
  return path;
}
auto OneLine(std::string text) -> std::string {
  for (auto& c : text)
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  return text;
}
// The reason a failure gives, short enough for one line of the chat: its
// first non-empty line, cut at a character boundary. The whole result stays
// in the row (the app's debug panel shows it).
auto ShortReason(std::string_view text) -> std::string {
  constexpr std::size_t kLimit = 160;
  constexpr std::string_view kSpace = " \t\r\n";
  const auto first = text.find_first_not_of(kSpace);
  if (first == std::string_view::npos) return {};
  auto line = text.substr(first, text.find('\n', first) - first);
  line = line.substr(0, line.find_last_not_of(kSpace) + 1);
  const auto rest = first + line.size();
  bool more = text.find_first_not_of(kSpace, rest) != std::string_view::npos;
  if (line.size() > kLimit) {
    auto cut = kLimit;
    // Not inside a UTF-8 sequence.
    while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
      --cut;
    line = line.substr(0, cut);
    more = true;
  }
  return OneLine(std::string(line)) + (more ? "…" : "");
}
auto FailureSummary(A::ChatRowT& row) -> void {
  if (row.kind != "tool" || row.state != "failed") return;
  const auto envelope = ObjectOrEmpty(row.result);
  const auto failure = json::Text(envelope, "reply") == "Failure"
                           ? ObjectOrEmpty(json::Get(envelope, "value"))
                           : envelope;
  auto kind = json::Text(failure, "kind", json::Text(failure, "code"));
  std::ranges::replace(kind, '_', ' ');
  auto diagnostic =
      json::Text(failure, "diagnostic",
                 json::Text(failure, "message", json::Text(failure, "error")));
  if (diagnostic.empty() && envelope.empty()) diagnostic = row.result;
  auto args = ObjectOrEmpty(row.arguments);
  // Typed failure arguments can identify a target absent from the call.
  try {
    for (const auto& raw : json::Array(json::Get(failure, "args", "[]"))) {
      const auto arg = ObjectOrEmpty(raw);
      args.try_emplace(json::Text(arg, "key"), json::Get(arg, "value"));
    }
  } catch (const std::exception&) {
  }
  std::string target;
  for (const auto* key : {"path", "card", "cards", "run_id", "run", "query",
                          "command", "folder", "url"}) {
    const auto value = json::Get(args, key);
    target = CardReference(value);
    if (!target.empty()) break;
    if (value.starts_with('[')) {
      try {
        std::vector<std::string> names;
        for (const auto& item : json::Array(value)) {
          if (names.size() == 3) break;
          if (const auto name = CardReference(item); !name.empty())
            names.push_back(name);
        }
        target = Join(names);
      } catch (const std::exception&) {
      }
      if (!target.empty()) break;
    }
  }
  if (target.empty()) {
    for (const auto* key : {"changes", "files", "edits", "tasks"}) {
      std::vector<std::string> names;
      try {
        for (const auto& raw : json::Array(json::Get(args, key, "[]"))) {
          auto item = ObjectOrEmpty(raw);
          if (item.contains("value"))
            item = ObjectOrEmpty(json::Get(item, "value"));
          for (const auto* field :
               {"path", "file_path", "card", "id", "to", "name"}) {
            if (auto name = CardReference(json::Get(item, field));
                !name.empty()) {
              names.push_back(name);
              break;
            }
          }
          if (names.size() == 3) break;
        }
      } catch (const std::exception&) {
      }
      if (!names.empty()) {
        target = Join(names);
        break;
      }
    }
  }
  const auto subject =
      row.name + (target.empty() ? "" : " · " + OneLine(target));
  const auto detail = diagnostic.empty() ? "" : ": " + ShortReason(diagnostic);
  row.summary_ja =
      subject + " · 失敗" + (kind.empty() ? "" : "（" + kind + "）") + detail;
  row.summary_en =
      subject + " · failed" + (kind.empty() ? "" : " (" + kind + ")") + detail;
}
auto Number(std::string_view raw) -> uint64_t {
  return json::UInt(raw, json::UInt(json::String(raw)));
}
auto Links(A::ChatRowT& row, const json::Members& value) -> void {
  for (const auto& key : {"card", "card_id"}) {
    const auto card = CardReference(json::Get(value, key));
    if (!card.empty()) AddLink(row, card, "card:" + card);
  }
  if (const auto raw = json::Get(value, "cards"); raw != "null") {
    try {
      for (const auto& card : json::Array(raw))
        if (const auto path = CardReference(card); !path.empty())
          AddLink(row, path, "card:" + path);
    } catch (const std::exception&) {
    }
  }
  if (const auto path = json::Text(value, "path");
      path.starts_with("definitions/")) {
    const auto card = path.substr(12, path.find('/', 12) - 12);
    if (!card.empty()) AddLink(row, card, "card:" + card);
  }
  if (const auto changes = json::Get(value, "changes"); changes != "null") {
    try {
      for (const auto& change : json::Array(changes))
        Links(row, ObjectOrEmpty(change));
    } catch (const std::exception&) {
    }
  }
  auto run = json::Text(value, "run_id", json::Text(value, "run"));
  if (run.empty() && row.name == "run") run = json::Text(value, "id");
  if (!run.empty()) {
    AddLink(row, "Run " + run, "run:" + run);
    if (row.name == "log_range") {
      const auto card = CardReference(json::Get(value, "card"));
      auto stream = json::Text(value, "stream");
      if (stream == "output" || stream == "journal" || stream == "build") {
        const auto invocation = Number(json::Get(value, "invocation"));
        const auto query =
            invocation ? "invocation=" + std::to_string(invocation) + "&" : "";
        AddLink(row, "Log",
                "log:" + run + (card.empty() ? "" : "/" + card) + "?" + query +
                    "stream=" + stream + "&offset=" +
                    std::to_string(Number(json::Get(value, "offset"))));
      }
    }
  }
  for (const auto& key : {"value", "result", "run"})
    if (value.contains(key)) Links(row, ObjectOrEmpty(json::Get(value, key)));
}
// Only a later success of the same operation on the same explicit targets is
// evidence of recovery. Different targets must not hide an earlier failure.
auto RetryKey(const A::ChatRowT& row) -> std::string {
  if (row.name == "core") return "receipt:" + row.id;
  json::Members args;
  try {
    args = json::Object(row.arguments);
  } catch (const std::exception&) {
    return row.name + ":" + row.arguments;
  }
  json::Members targets;
  for (const auto& key :
       {"card", "cards", "path", "paths", "scope", "run", "run_id"})
    if (args.contains(key)) targets[key] = json::Get(args, key);
  if (args.contains("changes")) {
    std::vector<std::string> changed;
    bool complete = true;
    try {
      for (const auto& raw : json::Array(json::Get(args, "changes"))) {
        const auto change = ObjectOrEmpty(raw);
        const auto value = ObjectOrEmpty(json::Get(change, "value"));
        const auto target = json::Get(value, "path", json::Get(value, "card"));
        if (target == "null") {
          complete = false;
          break;
        }
        changed.push_back(json::Text(change, "value_type") + ":" + target);
      }
    } catch (const std::exception&) {
      return row.name + ":" + json::Encode(args);
    }
    if (complete && !changed.empty()) {
      std::ranges::sort(changed);
      targets["changes"] = json::Encode(changed);
    }
  }
  if (args.contains("files")) {
    std::vector<std::string> paths;
    try {
      for (const auto& file : json::Array(json::Get(args, "files")))
        paths.push_back(json::Get(ObjectOrEmpty(file), "path"));
    } catch (const std::exception&) {
    }
    if (!paths.empty()) {
      std::ranges::sort(paths);
      targets["files"] = json::Encode(paths);
    }
  }
  return row.name + ":" + json::Encode(targets.empty() ? args : targets);
}
auto Recover(Rows& rows) -> void {
  std::map<std::string, std::vector<A::ChatRowT*>> failures;
  for (auto& row : rows) {
    if (row->kind == "text" && row->role == "user") failures.clear();
    if (row->kind != "step") continue;
    for (auto& call : row->children) {
      if (call->kind != "tool" || call->name == "operation_approval") continue;
      const auto key = RetryKey(*call);
      if (call->state == "failed")
        failures[key].push_back(call.get());
      else if (call->state == "completed") {
        for (auto* failed : failures[key]) failed->state = "recovered";
        failures[key].clear();
      }
    }
  }
}
auto Summarize(Rows& rows) -> void {
  for (auto& row : rows) {
    if (row->kind != "step") continue;
    row->failed = row->recovered = 0;
    for (const auto& call : row->children) {
      if (call->kind == "helper") row->failed += call->failed;
      if (call->kind != "tool" || call->name == "operation_approval") continue;
      row->failed += call->state == "failed";
      row->recovered += call->state == "recovered";
    }
    const auto count = std::ranges::count_if(row->children, [](const auto& c) {
      return c->kind == "tool" && c->name != "operation_approval";
    });
    row->state = std::ranges::any_of(row->children,
                                     [](const auto& child) {
                                       return child->state == "running" ||
                                              child->state == "queued";
                                     })
                     ? "running"
                 : row->failed ? "failed"
                               : "completed";
    row->summary_ja = std::to_string(count) + " 件の操作";
    row->summary_en =
        std::to_string(count) + (count == 1 ? " operation" : " operations");
    if (row->recovered) {
      row->summary_ja +=
          "（" + std::to_string(row->recovered) + " 件失敗→再試行で成功）";
      row->summary_en += " (" + std::to_string(row->recovered) +
                         " failed, then succeeded on retry)";
    }
    if (row->failed) {
      row->summary_ja += "・" + std::to_string(row->failed) + " 件失敗";
      row->summary_en += "; " + std::to_string(row->failed) + " failed";
    }
  }
}
auto BuildRows(const std::vector<const A::ModelMessageT*>& messages,
               size_t archived, const A::ConversationT& conversation,
               const std::string& prefix, bool helper) -> Rows {
  Rows rows;
  std::map<std::string, A::ChatRowT*> calls;
  std::set<std::pair<std::string, uint32_t>> placed;
  std::set<std::string> model_calls;
  // Calls whose result is an error: a task row is built at its call, before
  // the result is reached below.
  std::set<std::string> failed_calls;
  for (const auto* message : messages)
    for (const auto& raw : json::Array(
             message->content_json.empty() ? "[]" : message->content_json)) {
      const auto block = json::Object(raw);
      if (json::Text(block, "type") == "tool_use")
        model_calls.insert(json::Text(block, "id"));
      else if (json::Text(block, "type") == "tool_result" &&
               json::Bool(json::Get(block, "is_error")))
        failed_calls.insert(json::Text(block, "tool_use_id"));
    }
  std::set<std::string> nested_calls;
  for (const auto& parent : conversation.helpers) {
    const auto scope =
        parent->call_id + "." + std::to_string(parent->task) + ".";
    for (const auto& child : conversation.helpers)
      if (child->call_id.starts_with(scope))
        nested_calls.insert(child->call_id);
  }
  const auto taskRow = [&](const A::HelperT& task) {
    auto row = std::make_unique<A::ChatRowT>();
    const auto spec = ObjectOrEmpty(task.spec_json);
    row->id = "helper:" + task.call_id + ":" + std::to_string(task.task);
    row->kind = "helper";
    row->name = json::Text(spec, "name");
    row->state = task.state;
    row->instruction = json::Text(spec, "instruction");
    row->scope_ja = Scope(spec, false);
    row->scope_en = Scope(spec, true);
    std::vector<const A::ModelMessageT*> trace;
    // The first message is the internal instruction, shown separately.
    for (size_t i = 1; i < task.messages.size(); ++i)
      trace.push_back(task.messages[i].get());
    row->children =
        BuildRows(trace, 0, conversation,
                  task.call_id + "." + std::to_string(task.task) + ".", true);
    for (auto it = row->children.rbegin(); it != row->children.rend(); ++it)
      if ((*it)->kind == "text" && (*it)->role == "assistant") {
        row->text = (*it)->text;
        break;
      }
    if (const auto error = json::Text(ObjectOrEmpty(task.result_json), "error");
        !error.empty()) {
      row->state = error == "cancelled" ? "interrupted" : "failed";
      row->text = error;
    }
    if (row->text.empty())
      row->text = task.state == "completed"
                      ? json::Text(ObjectOrEmpty(task.result_json), "report")
                      : "";
    for (const auto& child : row->children) row->failed += child->failed;
    if (row->state == "failed" && !row->failed) row->failed = 1;
    if (row->failed) {
      row->summary_ja = "・" + std::to_string(row->failed) + " 件失敗";
      row->summary_en = "; " + std::to_string(row->failed) + " failed";
    }
    for (const auto& access : {"read_cards", "write_cards", "run_cards"}) {
      const auto raw = json::Get(spec, access);
      if (raw != "null")
        for (const auto& card : json::Array(raw))
          AddLink(*row, json::String(card), "card:" + json::String(card));
    }
    return row;
  };
  if (!helper)
    for (const auto& task : conversation.helpers)
      if (task->anchor == 0 && !model_calls.contains(task->call_id) &&
          !nested_calls.contains(task->call_id)) {
        rows.push_back(taskRow(*task));
        placed.insert({task->call_id, task->task});
      }
  for (size_t i = 0; i < messages.size(); ++i) {
    const auto& message = *messages[i];
    if (message.context_summary) continue;
    auto blocks =
        json::Array(message.content_json.empty() ? "[]" : message.content_json);
    if (!message.display.empty())
      blocks = {
          json::Encode(json::Members{{"type", json::Quote("text")},
                                     {"text", json::Quote(message.display)}})};
    for (size_t b = 0; b < blocks.size(); ++b) {
      const auto block = json::Object(blocks[b]);
      const auto type = json::Text(block, "type");
      if (type == "tool_result") {
        if (auto found = calls.find(json::Text(block, "tool_use_id"));
            found != calls.end()) {
          auto& row = *found->second;
          row.result = json::Text(block, "content");
          row.state =
              json::Bool(json::Get(block, "is_error")) ? "failed" : "completed";
          Links(row, ObjectOrEmpty(row.result));
        }
        continue;
      }
      if (type != "text" && type != "tool_use" && type != "web_search")
        continue;
      auto row = std::make_unique<A::ChatRowT>();
      row->id = prefix + ":" + std::to_string(i) + ":" + std::to_string(b);
      row->archived = i < archived;
      if (type == "text") {
        row->kind = "text";
        row->role = message.role;
        row->text = json::Text(block, "text");
        if (!row->text.empty()) rows.push_back(std::move(row));
        continue;
      }
      row->kind = "tool";
      row->id = prefix + ":call:" + json::Text(block, "id");
      row->name =
          type == "web_search" ? "web_search" : json::Text(block, "name");
      row->arguments = type == "web_search"
                           ? json::Encode(json::Members{
                                 {"query", json::Get(block, "query", "\"\"")}})
                           : json::Text(block, "input");
      if (type == "web_search") {
        for (const auto& raw : json::Array(json::Get(block, "sources", "[]"))) {
          const auto source = ObjectOrEmpty(raw);
          if (!row->result.empty()) row->result += '\n';
          const auto title = json::Text(source, "title");
          row->result +=
              (title.empty() ? "" : title + " — ") + json::Text(source, "url");
        }
        if (const auto error = json::Text(block, "error"); !error.empty())
          row->result = error;
        row->state =
            !json::Text(block, "error").empty() ? "failed" : "completed";
      } else
        row->state = "running";
      Links(*row, ObjectOrEmpty(row->arguments));
      calls[json::Text(block, "id")] = row.get();
      if (rows.empty() || rows.back()->kind != "step" ||
          rows.back()->archived != row->archived) {
        auto step = std::make_unique<A::ChatRowT>();
        step->id = row->id + ":step";
        step->kind = "step";
        step->archived = row->archived;
        rows.push_back(std::move(step));
      }
      auto& children = rows.back()->children;
      const auto call = json::Text(block, "id");
      children.push_back(std::move(row));
      for (const auto& approval : conversation.tools) {
        const auto receipt = prefix.empty() ? call : prefix + call;
        if (approval->name != "operation_approval" ||
            approval->id != "approval:" + receipt)
          continue;
        auto a = std::make_unique<A::ChatRowT>();
        a->kind = "tool";
        a->id = approval->id;
        a->name = approval->name;
        a->result = approval->result_json;
        a->state = approval->state == "pending" ? "running"
                   : approval->is_error         ? "failed"
                                                : "completed";
        children.push_back(std::move(a));
      }
      {
        const auto parent_call = prefix.empty() ? call : prefix + call;
        std::map<uint32_t, const A::HelperT*> tasks;
        std::vector<std::unique_ptr<A::HelperT>> queued;
        // A delegate that ended in an error starts no further task. One
        // refused as a whole failed before any task; one that had started
        // stopped before reaching the rest.
        const bool started = std::ranges::any_of(
            conversation.helpers,
            [&](const auto& task) { return task->call_id == parent_call; });
        if (calls[call]->name == "delegate") {
          const auto args = ObjectOrEmpty(calls[call]->arguments);
          try {
            uint32_t index = 0;
            for (const auto& raw :
                 json::Array(json::Get(args, "tasks", "[]"))) {
              const auto spec = ObjectOrEmpty(raw);
              if (!json::Text(spec, "name").empty() &&
                  !json::Text(spec, "instruction").empty()) {
                auto task = std::make_unique<A::HelperT>();
                task->call_id = parent_call;
                task->task = index;
                task->spec_json = raw;
                task->state = !failed_calls.contains(call) ? "queued"
                              : started                    ? "interrupted"
                                                           : "failed";
                tasks[index] = task.get();
                queued.push_back(std::move(task));
              }
              ++index;
            }
          } catch (const std::exception&) {
          }
        }
        for (const auto& task : conversation.helpers)
          if (task->call_id == parent_call) tasks[task->task] = task.get();
        for (const auto& [index, task] : tasks) {
          try {
            children.push_back(taskRow(*task));
            placed.insert({task->call_id, index});
            // The refusal is one failure, counted on the delegate call; its
            // tasks never ran and add none of their own.
            if (std::ranges::any_of(queued, [&](const auto& unstarted) {
                  return unstarted.get() == task;
                })) {
              children.back()->failed = 0;
              children.back()->summary_ja.clear();
              children.back()->summary_en.clear();
            }
          } catch (
              const std::exception&) { /* Invalid undeployed tasks are described
                                          by the failed delegate call. */
          }
        }
      }
    }
    if (!helper)
      for (const auto& task : conversation.helpers)
        if (task->anchor == i + 1 &&
            !placed.contains({task->call_id, task->task})) {
          // A direct Skill helper has no delegate call; its anchor is the user
          // message.
          if (!model_calls.contains(task->call_id) &&
              !nested_calls.contains(task->call_id)) {
            rows.push_back(taskRow(*task));
            placed.insert({task->call_id, task->task});
          }
        }
  }
  if (!helper) {
    const auto contains = [](auto&& self, const Rows& nested,
                             const std::string& id) -> bool {
      for (const auto& row : nested)
        if (row->id == id || self(self, row->children, id)) return true;
      return false;
    };
    // External operations need not have a model call. Keep their records
    // visible without inventing a parent or hiding them after a restore.
    for (const auto& record : conversation.tools) {
      if (record->name != "operation_approval" ||
          contains(contains, rows, record->id))
        continue;
      auto row = std::make_unique<A::ChatRowT>();
      row->id = record->id;
      row->kind = "tool";
      row->name = record->name;
      row->result = record->result_json;
      row->state = record->state == "pending" ? "running"
                   : record->is_error         ? "failed"
                                              : "completed";
      rows.push_back(std::move(row));
    }
    if (rows.empty() && !conversation.tools.empty()) {
      auto step = std::make_unique<A::ChatRowT>();
      step->id = "external-operations";
      step->kind = "step";
      for (const auto& record : conversation.tools) {
        auto row = std::make_unique<A::ChatRowT>();
        row->id = record->id;
        row->kind = "tool";
        row->name = record->name;
        row->arguments = record->arguments_json;
        row->result = record->result_json;
        row->summary_ja = "操作の記録";
        row->summary_en = "Operation record";
        row->state = record->state == "completed"
                         ? (record->is_error ? "failed" : "completed")
                         : "running";
        step->children.push_back(std::move(row));
      }
      rows.push_back(std::move(step));
    }
  }
  for (auto& row : rows) {
    FailureSummary(*row);
    for (auto& child : row->children) FailureSummary(*child);
  }
  Recover(rows);
  Summarize(rows);
  return rows;
}
}  // namespace

auto SettleStopped(Rows& rows) -> void {
  for (auto& row : rows) {
    SettleStopped(row->children);
    if (row->state == "running" || row->state == "queued")
      row->state = "interrupted";
  }
}

auto Presentation(const A::ConversationT& conversation) -> Rows {
  std::vector<const A::ModelMessageT*> messages;
  for (const auto& message : conversation.archive)
    messages.push_back(message.get());
  for (const auto& message : conversation.messages)
    messages.push_back(message.get());
  return BuildRows(messages, conversation.archive.size(), conversation, "",
                   false);
}
auto AccessGroups(const std::vector<std::string>& asked)
    -> std::vector<std::unique_ptr<A::AccessGroupT>> {
  std::vector<std::unique_ptr<A::AccessGroupT>> groups;
  for (const auto& request : asked) {
    if (!request.starts_with("project_access {")) continue;
    const auto value = ObjectOrEmpty(request.substr(15));
    if (value.empty()) continue;
    const auto path = json::Text(value, "path"),
               title = json::Text(value, "title");
    auto group = std::ranges::find_if(groups, [&](const auto& g) {
      return g->path == path && g->title == title;
    });
    if (group == groups.end()) {
      auto g = std::make_unique<A::AccessGroupT>();
      g->title = title;
      g->path = path;
      g->display_path = path;
      const auto home = std::getenv("HOME");
      if (home && (path == home || path.starts_with(std::string(home) + "/")))
        g->display_path = "~" + path.substr(std::string(home).size());
      groups.push_back(std::move(g));
      group = std::prev(groups.end());
    }
    auto line = std::make_unique<A::AccessLineT>();
    line->request = request;
    line->operation = json::Text(value, "access");
    line->needed_now = json::Bool(json::Get(value, "needed_now", "true"));
    (*group)->lines.push_back(std::move(line));
  }
  return groups;
}

}  // namespace tsugumi::agent
