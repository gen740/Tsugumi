#include "agent/helpers.hh"

#include <algorithm>

#include "agent/helper_scope.hh"
#include "agent/json.hh"
#include "agent/providers.hh"
#include "agent/store.hh"
#include "agent/tools.hh"
#include "agent/turns.hh"
#include "host/access.hh"
#include "host/failure.hh"
#include "host/settings.hh"
#include "util/crypto.hh"

namespace tsugumi::agent {
using host::Failure;
Helpers::Helpers(host::StateGate& gate, ConversationStore& conversations,
                 Providers& providers, host::Settings& settings, Tools& tools,
                 Turns& turns, host::Access& access, Dependencies dependencies)
    : mutex(gate.mutex),
      conversations(&conversations),
      providers(&providers),
      host_settings(&settings),
      tools(&tools),
      turns(&turns),
      access_service(access),
      dependencies(std::move(dependencies)) {}

auto Helpers::HelperResult(const std::string& output,
                           const std::string& conversation,
                           const std::string& call, size_t task)
    -> std::string {
  // Eight results fit below Runtime's 60,000-byte tool-result bound. The
  // original transcript and full result stay in the existing helper record.
  if (output.size() <= 7000) return output;
  const auto value = json::Object(output);
  std::string excerpt;
  const auto messages = ParseMessages(json::Get(value, "messages", "[]"));
  for (auto message = messages.rbegin();
       message != messages.rend() && excerpt.empty(); ++message)
    if (message->role == "assistant")
      for (const auto& block : message->content)
        if (block.type == "text") {
          excerpt = Turns::Excerpt(block.text);
          break;
        }
  json::Members shortened{
      {"name", json::Get(value, "name")},
      {"shortened", "true"},
      {"notice", json::Quote("Helper result shortened. Open this helper in the "
                             "conversation for the full messages and result.")},
      {"conversation_id", json::Quote(conversation)},
      {"call_id", json::Quote(call)},
      {"task", std::to_string(task)},
      {"error", json::Quote(Turns::Excerpt(json::Text(value, "error")))},
      {"excerpt", json::Quote(excerpt)}};
  auto result = json::Encode(shortened);
  if (result.size() > 7000) {
    shortened.erase("excerpt");
    shortened.erase("error");
    result = json::Encode(shortened);
  }
  return result;
}

auto Helpers::Displaced(const std::vector<Message>& previous,
                        const std::vector<Message>& saved)
    -> std::vector<Message> {
  // A compaction saves a summary followed by the messages it kept, which are
  // the last ones of the previous save (their tool results may have been
  // shortened, so they are matched by shape). Any other save displaces nothing.
  if (saved.empty() || !saved.front().context_summary ||
      saved.size() - 1 > previous.size())
    return {};
  const auto count = previous.size() - (saved.size() - 1);
  for (size_t i = 1; i < saved.size(); ++i) {
    const auto& before = previous[count + i - 1];
    if (before.role != saved[i].role ||
        before.content.size() != saved[i].content.size())
      return {};
  }
  std::vector<Message> displaced;
  for (size_t i = 0; i < count; ++i)
    if (!previous[i].context_summary) displaced.push_back(previous[i]);
  return displaced;
}

auto Helpers::RunHelper(const std::string& conversation,
                        const std::string& parent_call, size_t index,
                        const std::string& task, std::stop_token stop,
                        const std::string& authorization) -> std::string {
  const auto profile = providers->Profile(conversation);
  std::shared_ptr<std::mutex> gate;
  {
    std::lock_guard lock(mutex);
    auto& weak = helper_gates[{conversation, parent_call, index}];
    gate = weak.lock();
    if (!gate) {
      gate = std::make_shared<std::mutex>();
      weak = gate;
    }
  }
  std::lock_guard helper_lock(*gate);
  {
    std::unique_lock lock(mutex);
    if (!helpers_changed.wait(lock, stop, [&] {
          return active_helpers[profile] <
                 host_settings->HelperPolicy(profile).first;
        }))
      throw Cancelled();
    ++active_helpers[profile];
  }
  const auto release = [this, profile](void*) {
    std::lock_guard lock(mutex);
    --active_helpers[profile];
    helpers_changed.notify_all();
  };
  std::unique_ptr<void, decltype(release)> slot(this, release);
  const auto spec = json::Object(task);
  HelperScope::Validate(spec, host_settings->HelperPolicy(profile).second);
  const auto name = json::Text(spec, "name");
  const auto digest = crypto::sha256_hex(task);
  const auto scope =
      crypto::sha256_hex(parent_call + ":" + std::to_string(index)) + ":";
  std::vector<Message> messages;
  ProviderConfig config;
  {
    std::lock_guard lock(mutex);
    auto old = conversations->ConversationDatabase(conversation)
                   .prepare(
                       "SELECT digest,messages,result,state FROM helpers WHERE "
                       "conversation=? AND parent_call=? AND task=?");
    old.bind(1, conversation)
        .bind(2, parent_call)
        .bind(3, static_cast<int>(index));
    if (old.step()) {
      if (old.text(0) != digest)
        throw Failure("conflict",
                      "A helper ID was reused for different instructions");
      messages = ParseMessages(old.text(1));
      access_service.AuthorizeContext(conversation, messages);
      if (old.text(3) == "completed")
        return HelperResult(old.text(2), conversation, parent_call, index);
    } else {
      ContentBlock instruction;
      instruction.text =
          json::Text(spec, "instruction") + "\nHelper scope: read_cards=" +
          json::Get(spec, "read_cards", "null") +
          "; write_cards=" + json::Get(spec, "write_cards", "null") +
          "; run_cards=" + json::Get(spec, "run_cards", "null") +
          ". Exact permanent Card paths; no implicit descendants. null means "
          "unscoped.";
      instruction.resource_ids = authorization.empty()
                                     ? conversations->Resources(conversation)
                                     : std::vector<std::string>{authorization};
      messages.push_back(
          {"user", {instruction}, {}, {}, ProviderAPI::AnthropicMessages});
      auto insert =
          conversations->ConversationDatabase(conversation)
              .prepare(
                  "INSERT INTO helpers "
                  "VALUES(?,?,?,?,?,'','running',?,?,'[]')");
      insert.bind(1, conversation)
          .bind(2, parent_call)
          .bind(3, static_cast<int>(index))
          .bind(4, digest)
          .bind(5, dependencies.scrub(SerializeMessages(messages)))
          .bind(6, dependencies.scrub(task))
          .bind(
              7,
              static_cast<int>(
                  json::Array(
                      json::Get(
                          json::Object(conversations->ConversationJson(
                              conversations->GetConversation(conversation))),
                          "archive", "[]"))
                      .size() +
                  std::ranges::count_if(
                      ParseMessages(conversations->GetConversation(conversation)
                                        .messages),
                      [](const auto& message) {
                        return !message.context_summary;
                      })))
          .run();
    }
    config = providers->ForConversation(conversation);
  }
  dependencies.event(conversation, json::Encode(json::Members{
                                       {"kind", json::Quote("helper")},
                                       {"id", json::Quote(parent_call)},
                                       {"name", json::Quote(name)},
                                       {"index", std::to_string(index)},
                                       {"event", R"({"kind":"checkpoint"})"}}));
  auto boundary = std::make_shared<HelperScope>(spec, authorization, scope);
  const auto read = tools->HelperReader(conversation, stop, {}, authorization);
  // Receipt ownership survives both interruption and transcript compaction.
  std::vector<std::string> owned_receipts;
  {
    std::lock_guard lock(mutex);
    auto receipts =
        conversations->ConversationDatabase(conversation)
            .prepare(
                "SELECT result FROM receipts WHERE conversation=? AND "
                "substr(id,1,?)=? AND state='completed' ORDER BY rowid");
    receipts.bind(1, conversation)
        .bind(2, static_cast<int>(scope.size()))
        .bind(3, scope);
    while (receipts.step()) owned_receipts.push_back(receipts.text(0));
  }
  for (const auto& raw : owned_receipts) boundary->Restore(raw, read);
  auto hooks = tools->Hooks(conversation, true, boundary);
  const auto execute = hooks.execute_tool;
  hooks.execute_tool = [execute, scope](ToolCall call, std::stop_token token) {
    call.id = scope + call.id;
    return execute(call, token);
  };
  const auto reconcile = hooks.reconcile_tool;
  hooks.reconcile_tool = [reconcile, scope](ToolCall call) {
    call.id = scope + call.id;
    return reconcile(call);
  };
  hooks.event = [this, conversation, name, parent_call,
                 index](const RuntimeEvent& e) {
    dependencies.event(
        conversation,
        json::Encode(json::Members{
            {"kind", json::Quote("helper")},
            {"name", json::Quote(name)},
            {"id", json::Quote(parent_call)},
            {"index", std::to_string(index)},
            {"event", json::Encode(json::Members{
                          {"kind", json::Quote(e.kind)},
                          {"text", json::Quote(e.text)},
                          {"message", json::Quote(e.text)},
                          {"input", json::Quote(e.text)},
                          {"id", json::Quote(e.id)},
                          {"name", json::Quote(e.name)},
                          {"ok", e.ok ? "true" : "false"},
                          {"index", std::to_string(e.index)}})}}));
  };
  hooks.checkpoint = [this, conversation, parent_call,
                      index](const auto& saved) {
    std::lock_guard lock(mutex);
    sql::Transaction tx(conversations->ConversationDatabase(conversation));
    for (const auto& message : saved)
      for (const auto& block : message.content)
        for (const auto& resource : block.resource_ids)
          conversations->RememberResource(conversation, resource);
    // Compaction replaces the earlier messages with a summary for the model.
    // A person still reads the helper's operations and failures from them, so
    // they move to the archive instead of being overwritten.
    std::string archive;
    if (!saved.empty() && saved.front().context_summary) {
      auto old = conversations->ConversationDatabase(conversation)
                     .prepare(
                         "SELECT messages,archive FROM helpers WHERE "
                         "conversation=? AND parent_call=? AND task=?");
      old.bind(1, conversation)
          .bind(2, parent_call)
          .bind(3, static_cast<int>(index));
      if (old.step()) {
        const auto previous = ParseMessages(old.text(0));
        auto displaced = Displaced(previous, saved);
        if (!displaced.empty()) {
          auto earlier = ParseMessages(old.text(1));
          earlier.insert(earlier.end(),
                         std::make_move_iterator(displaced.begin()),
                         std::make_move_iterator(displaced.end()));
          archive = dependencies.scrub(SerializeMessages(earlier));
        }
      }
    }
    auto update =
        conversations->ConversationDatabase(conversation)
            .prepare(archive.empty()
                         ? "UPDATE helpers SET messages=? WHERE "
                           "conversation=? AND parent_call=? AND task=?"
                         : "UPDATE helpers SET messages=?,archive=? WHERE "
                           "conversation=? AND parent_call=? AND task=?");
    int field = 0;
    update.bind(++field, dependencies.scrub(SerializeMessages(saved)));
    if (!archive.empty()) update.bind(++field, archive);
    update.bind(field + 1, conversation)
        .bind(field + 2, parent_call)
        .bind(field + 3, static_cast<int>(index))
        .run();
    tx.commit();
    dependencies.event(
        conversation,
        json::Encode(json::Members{{"kind", json::Quote("helper")},
                                   {"id", json::Quote(parent_call)},
                                   {"index", std::to_string(index)},
                                   {"event", R"({"kind":"checkpoint"})"}}));
  };
  auto tools = this->tools->Specs();
  std::erase_if(tools,
                [](const auto& tool) { return tool.name == "delegate"; });
  const auto result =
      Runtime()
          .Run(config, Turns::System(), messages, tools, stop, hooks);
  const auto output = json::Encode(json::Members{
      {"name", json::Quote(name)},
      {"messages", dependencies.scrub(SerializeMessages(messages))},
      {"error", json::Quote(result.error)}});
  {
    std::lock_guard lock(mutex);
    sql::Transaction transaction(
        conversations->ConversationDatabase(conversation));
    turns->AddUsage(conversation, result.usage);
    auto update = conversations->ConversationDatabase(conversation)
                      .prepare(
                          "UPDATE helpers SET result=?,state=? WHERE "
                          "conversation=? AND parent_call=? AND task=?");
    update.bind(1, output)
        .bind(2, result.outcome == RunOutcome::Cancelled ? "interrupted"
                                                         : "completed")
        .bind(3, conversation)
        .bind(4, parent_call)
        .bind(5, static_cast<int>(index))
        .run();
    transaction.commit();
  }
  return HelperResult(output, conversation, parent_call, index);
}

}  // namespace tsugumi::agent
