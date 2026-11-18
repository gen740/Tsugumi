#include "agent/helpers.hh"

#include <algorithm>

#include "agent/context.hh"
#include "agent/helper_scope.hh"
#include "agent/providers.hh"
#include "agent/store.hh"
#include "agent/tools.hh"
#include "agent/turns.hh"
#include "host/access.hh"
#include "host/failure.hh"
#include "host/settings.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/json_object.hh"

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
  const auto value = json::Object(output);
  std::string report;
  const auto messages = ParseMessages(json::Get(value, "messages", "[]"));
  for (auto message = messages.rbegin(); message != messages.rend();
       ++message) {
    if (message->role != "assistant") continue;
    for (const auto& block : message->content)
      if (block.type == "text") report += block.text;
    if (!report.empty()) break;
  }
  constexpr size_t kReportBytes = 8192;
  const bool shortened = report.size() > kReportBytes;
  if (shortened) {
    const auto marker = "\n[Report clipped; open helper " + conversation + "/" +
                        call + "/" + std::to_string(task) +
                        " for the full trace.]";
    const auto room =
        kReportBytes > marker.size() ? kReportBytes - marker.size() : 0;
    size_t end = std::min(report.size(), room);
    while (end && (static_cast<unsigned char>(report[end]) & 0xc0) == 0x80)
      --end;
    report.resize(end);
    report += marker;
  }
  return json::Encode(json::Members{
      {"name", json::Get(value, "name")},
      {"shortened", shortened ? "true" : "false"},
      {"notice",
       json::Quote("Open this helper in the conversation for the full trace.")},
      {"conversation_id", json::Quote(conversation)},
      {"call_id", json::Quote(call)},
      {"task", std::to_string(task)},
      {"error",
       json::Quote(Turns::Excerpt(json::Text(value, "error"), 0, 1024))},
      {"report", json::Quote(report)}});
}

auto Helpers::Displaced(const std::vector<Message>& previous,
                        const std::vector<Message>& saved)
    -> std::vector<Message> {
  auto displaced = RemovedMessages(previous, saved);
  std::erase_if(displaced,
                [](const auto& message) { return message.context_summary; });
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
  const auto spec = json::Object(task);
  HelperScope::Validate(spec, host_settings->AllowUnscopedHelpers(profile));
  const auto name = json::Text(spec, "name");
  const auto digest = crypto::sha256_hex(task);
  const auto scope = parent_call + "." + std::to_string(index) + ".";
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
          "unscoped. Spawn capability: depth=" +
          json::Get(spec, "depth", "0") +
          "; count=" + json::Get(spec, "count", "0") +
          " children over your lifetime. The host enforces this capability.";
      instruction.resource_ids = authorization.empty()
                                     ? conversations->Resources(conversation)
                                     : std::vector<std::string>{authorization};
      messages.push_back(
          {"user", {instruction}, {}, {}, ProviderAPI::AnthropicMessages});
      auto insert = conversations->ConversationDatabase(conversation)
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
  boundary->identity = scope;
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
    auto old = conversations->ConversationDatabase(conversation)
                   .prepare(
                       "SELECT messages,archive FROM helpers WHERE "
                       "conversation=? AND parent_call=? AND task=?");
    old.bind(1, conversation)
        .bind(2, parent_call)
        .bind(3, static_cast<int>(index));
    if (!old.step()) throw Failure("not_found", "Missing helper checkpoint");
    auto archive = ParseMessages(old.text(1));
    const auto serialized = dependencies.scrub(SerializeMessages(saved));
    const auto removed =
        Displaced(ParseMessages(old.text(0)), ParseMessages(serialized));
    archive.insert(archive.end(), removed.begin(), removed.end());
    // Archive and replacement are committed together, including equal-sized
    // replacements. The result remains the helper's final report.
    auto update = conversations->ConversationDatabase(conversation)
                      .prepare(
                          "UPDATE helpers SET messages=?,archive=? WHERE "
                          "conversation=? AND parent_call=? AND task=?");
    update.bind(1, serialized)
        .bind(2, dependencies.scrub(SerializeMessages(archive)))
        .bind(3, conversation)
        .bind(4, parent_call)
        .bind(5, static_cast<int>(index))
        .run();
    tx.commit();
    dependencies.event(
        conversation,
        json::Encode(json::Members{{"kind", json::Quote("helper")},
                                   {"id", json::Quote(parent_call)},
                                   {"index", std::to_string(index)},
                                   {"event", R"({"kind":"checkpoint"})"}}));
  };
  auto tools = this->tools->catalog.ModelSpecs(
      boundary->scoped() ? ModelRole::ScopedHelper : ModelRole::Helper,
      boundary->depth() > 0 && boundary->count() > 0);
  std::string trusted_project;
  const auto automatic =
      turns->SkillContent(conversation, {}, true, stop, &trusted_project);
  const auto authorize = hooks.authorize_context;
  hooks.authorize_context = [this, authorize, automatic,
                             trusted_project](const auto& context) {
    turns->RequireSkillTrust(trusted_project);
    auto with_skills = context;
    with_skills.push_back(
        {"user", {automatic}, {}, {}, ProviderAPI::AnthropicMessages});
    authorize(with_skills);
  };
  const auto result = Runtime().Run(config, Turns::System() + automatic.text,
                                    messages, tools, stop, hooks);
  std::string output;
  {
    std::lock_guard lock(mutex);
    sql::Transaction transaction(
        conversations->ConversationDatabase(conversation));
    turns->AddUsage(conversation, result.usage);
    auto trace = messages;
    std::erase_if(trace,
                  [](const auto& message) { return message.context_summary; });
    output = json::Encode(json::Members{
        {"name", json::Quote(name)},
        {"messages", dependencies.scrub(SerializeMessages(trace))},
        {"error", json::Quote(result.error)}});
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
