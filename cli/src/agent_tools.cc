#include "agent_tools.hh"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "agent_catalog.hh"
#include "rpc.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::cli {
namespace A = Tsugumi::Agent;
namespace C = tsugumi::client;

namespace {
// `open -g -a <app> <folder>`: the app opens the project in a window of its
// own (behind the frontmost app) and reaches the same host.
auto OpenApp(const std::filesystem::path& app, const std::string& folder) -> bool {
#ifdef __APPLE__
  const pid_t child = ::fork();
  if (child < 0) return false;
  if (child == 0) {
    const int null = ::open("/dev/null", O_RDWR);
    ::dup2(null, 0);
    ::dup2(null, 1);
    ::dup2(null, 2);
    ::execl("/usr/bin/open", "open", "-g", "-a", app.c_str(), folder.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  return ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
  (void)app;
  (void)folder;
  return false;
#endif
}
}  // namespace

AgentTools::AgentTools(Launch launch) : launch_(std::move(launch)) {
  const auto [schema, size] = AgentSchema();
  encoder_.opts.strict_json = true;
  decoder_.opts.strict_json = true;
  decoder_.opts.output_default_scalars_in_json = true;
  decoder_.opts.indent_step = -1;
  if (!encoder_.Deserialize(schema, size) || !decoder_.Deserialize(schema, size))
    throw std::runtime_error("cannot load the Agent schema");
}

// Starting waits on connect_ alone: callers that only read state go on.
auto AgentTools::Agent() -> std::shared_ptr<C::Connection> {
  std::lock_guard connecting(connect_);
  {
    std::lock_guard lock(mutex_);
    if (agent_ && !stale_->load()) return agent_;
  }
  // A new host (after a restart) or a closed socket: connect again.
  auto ready = EnsureHost(launch_);
  auto stale = std::make_shared<std::atomic<bool>>(false);
  auto agent = std::shared_ptr<C::Connection>(ConnectAgent(ready, false));
  agent->OnClosed([stale](C::Error) { *stale = true; });
  std::lock_guard lock(mutex_);
  agent_ = agent;
  stale_ = stale;
  return agent;
}

auto AgentTools::Catalog() -> std::vector<ToolInfo> {
  std::vector<ToolInfo> tools;
  const auto catalog = AgentCall<A::ToolCatalog>(*Agent(), A::ToolCatalogRequestT{});
  if (catalog.view->tools())
    for (const auto* tool : *catalog.view->tools())
      tools.push_back({tool->name()->str(), tool->description() ? tool->description()->str() : "",
                       tool->input_schema_json() ? tool->input_schema_json()->str() : "{}"});
  for (const auto& op : AgentOperations()) tools.push_back({op.tool, op.description, op.input_schema});
  return tools;
}

auto AgentTools::Show(const std::string& path, bool create, bool browser) -> Shown {
  A::ShowRequestT request;
  request.path = path;
  request.create = create;
  // The Mac app's command shows the project in the app, not in a browser.
  request.browser = browser && launch_.app.empty();
  const auto reply = AgentCall<A::Shown>(*Agent(), std::move(request));
  const auto* o = reply.view;
  Shown shown{o->path() ? o->path()->str() : "", o->project_id() ? o->project_id()->str() : "",
              o->url() ? o->url()->str() : "", o->shown(), o->opened()};
  if (browser && !shown.shown && !launch_.app.empty() && !shown.path.empty()) {
    shown.opened = OpenApp(launch_.app, shown.path);
    shown.app = shown.opened;
  }
  std::lock_guard lock(mutex_);
  if (!shown.project_id.empty()) current_ = shown;
  return shown;
}

auto AgentTools::Current() -> std::optional<Shown> {
  std::lock_guard lock(mutex_);
  return current_;
}

auto AgentTools::Call(const std::string& name, const std::string& arguments, const Track& track) -> Content {
  if (name.starts_with("agent_")) {
    for (const auto& op : AgentOperations())
      if (op.tool == name) return {Request(op.request, arguments, track), false, {}};
    throw std::runtime_error("Unknown Agent operation: " + name);
  }
  const auto project = Current();
  if (!project) return {"No Tsugumi project is open: call tsugumi_open with its folder first.", true, {}};
  const auto connection = Agent();
  auto& agent = *connection;
  const auto conversation = "external-" + project->project_id;
  A::ConversationCreateRequestT create;
  create.conversation_id = conversation;
  create.project_id = project->project_id;
  create.role = "external";
  AgentCall<A::ConversationResult>(agent, std::move(create));
  A::CallIdRequestT reserve;
  reserve.conversation_id = conversation;
  reserve.name = name;
  reserve.arguments_json = arguments;
  const auto op = AgentCall<A::CallId>(agent, std::move(reserve)).view->id()->str();
  A::ToolCallRequestT call;
  call.conversation_id = conversation;
  call.project_id = project->project_id;
  call.name = name;
  call.arguments_json = arguments;
  call.op_id = op;
  try {
    const auto result = AgentCall<A::ToolResult>(agent, std::move(call), [&](uint64_t request) {
      if (track) track(connection, request);
    });
    Content out{result.view->text() ? result.view->text()->str() : "", result.view->is_error(), {}};
    if (result.view->images())
      for (const auto* image : *result.view->images())
        out.images.emplace_back(image->media_type()->str(), image->data()->str());
    return out;
  } catch (const RpcFailure& e) {
    // A decision waits for the person in the Tsugumi app or Web UI.
    if (e.code != "decision_required" || e.request_id.empty()) throw;
    return {json::Encode(json::Members{{"code", json::Quote(e.code)},
                                       {"message", json::Quote(e.what())},
                                       {"request_id", json::Quote(e.request_id)}}),
            true,
            {}};
  }
}

auto AgentTools::Request(const std::string& request, const std::string& arguments, const Track& track)
    -> std::string {
  const auto connection = Agent();
  auto& agent = *connection;
  const auto frame = [&](uint64_t id) {
    const auto text = "{\"request_id\":" + std::to_string(id) +
                      ",\"payload_type\":\"Request\",\"payload\":{\"value_type\":" + json::Quote(request) +
                      ",\"value\":" + arguments + "}}";
    std::lock_guard lock(codec_);
    if (!encoder_.ParseJson(text.c_str())) throw std::runtime_error("invalid arguments: " + encoder_.error_);
    const auto* begin = encoder_.builder_.GetBufferPointer();
    return C::Bytes(begin, begin + encoder_.builder_.GetSize());
  };
  frame(1);  // the arguments are checked before anything is sent
  auto result = agent
                    .Call([&](uint64_t id) {
                      if (track) track(connection, id);
                      return frame(id);
                    })
                    .get();
  if (result.error) throw std::runtime_error("Tsugumi host connection: " + result.error.message);
  std::string text;
  {
    std::lock_guard lock(codec_);
    if (const auto* failed = flatbuffers::GenerateText(decoder_, result.bytes.data(), &text))
      throw std::runtime_error(std::string("cannot read the reply: ") + failed);
  }
  const auto payload = json::Object(json::Get(json::Object(text), "payload", "{}"));
  const auto value = json::Get(payload, "value", "{}");
  if (json::Text(payload, "value_type") == "Failure") {
    const auto failure = json::Object(value);
    throw RpcFailure(json::Text(failure, "code"), json::Text(failure, "message"), json::Text(failure, "request_id"));
  }
  return value;
}

auto AgentTools::Cancel(const std::weak_ptr<C::Connection>& weak, uint64_t request) -> void {
  const auto agent = weak.lock();
  if (!agent) return;
  A::ToolCallCancelRequestT cancel;
  cancel.request_id = request;
  agent->Call(
      [cancel](uint64_t id) {
        A::RequestT value;
        value.value.Set(A::ToolCallCancelRequestT(cancel));
        A::MessageT message;
        message.request_id = id;
        message.payload.Set(std::move(value));
        flatbuffers::FlatBufferBuilder b;
        A::FinishMessageBuffer(b, A::Message::Pack(b, &message));
        return C::Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
      },
      [](C::Result) {});
}

}  // namespace tsugumi::cli
