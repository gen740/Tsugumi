#include "host/versions.hh"

#include "agent/tools.hh"
#include "agent/wire.hh"
#include "host/failure.hh"
#include "host/version_text.hh"
#include "host/wire_helpers.hh"
#include "tsugumi/protocol/tsugumi_generated.h"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::host {
namespace H = Tsugumi::Host;
namespace R = Tsugumi::RPC;
namespace json = ::tsugumi::json;
namespace wire = agent::wire;
namespace {
template <class Table>
auto VersionCall(agent::Tools& tools, const std::string& project,
                 const std::string& method, const json::Members& args)
    -> std::unique_ptr<typename Table::NativeTableType> {
  std::string bytes;
  const auto envelope = json::Object(
      tools.RawProjectCall(project, method, json::Encode(args), {}, &bytes));
  if (json::Text(envelope, "reply") == "Failure") {
    const auto failure = json::Object(json::Get(envelope, "value"));
    throw Failure(json::Text(failure, "kind"),
                  json::Text(failure, "diagnostic"));
  }
  const auto* reply = R::GetMessage(bytes.data())->payload_as_Reply();
  const auto* value = reply ? reply->value_as<Table>() : nullptr;
  if (!value)
    throw Failure("unavailable", "Unexpected project reply to " + method);
  return std::unique_ptr<typename Table::NativeTableType>(value->UnPack());
}
}  // namespace

Versions::Versions(agent::Tools& tools) : tools(&tools) {}

auto Versions::Note(const RequestIn& request, bool human,
                    std::unique_lock<std::recursive_mutex>& lock)
    -> std::string {
  const auto* r = H::GetMessage(request.frame->data())->payload_as_Request();
  Human(human);
  const auto* p = r->value_as_VersionNoteRequest();
  const auto project = p->project_id()->str();
  const bool english = p->language()->str() == "en";
  lock.unlock();
  // The change record only: browsing versions never invokes a model.
  const auto note = VersionCall<R::VersionNote>(
      *tools, project, "version_note",
      {{"commit", json::Quote(p->commit()->str())}});
  Tsugumi::Service::TextResultT result;
  result.text = host::ChangeText(*note->changes, english);
  return wire::HostReply(request.request_id, std::move(result));
}

auto Versions::HandleHost(const RequestIn& request, bool human,
                          std::unique_lock<std::recursive_mutex>& lock)
    -> std::string {
  const auto* r = H::GetMessage(request.frame->data())->payload_as_Request();
  if (r->value_as_VersionNoteRequest()) return Note(request, human, lock);
  if (const auto* graph = r->value_as_HostVersionGraphRequest()) {
    const auto project = graph->project()->str();
    const bool english = Text(graph->language()) == "en";
    const auto limit = graph->limit();
    lock.unlock();
    auto result = VersionCall<R::VersionGraph>(
        *tools, project, "version_graph", {{"limit", std::to_string(limit)}});
    host::PresentGraph(*result, english);
    return wire::HostReply(request.request_id, std::move(*result));
  }
  if (const auto* diff = r->value_as_HostVersionDiffRequest()) {
    lock.unlock();
    auto result = VersionCall<R::VersionDiff>(
        *tools, diff->project()->str(), "version_diff",
        {{"commit", json::Quote(diff->commit()->str())},
         {"from_commit", json::Quote(Text(diff->from_commit()))}});
    return wire::HostReply(request.request_id, std::move(*result));
  }
  Human(human);
  const auto* p = r->value_as_VersionCommentsRequest();
  lock.unlock();
  auto result = VersionCall<R::VersionComments>(
      *tools, p->project()->str(), "version_comments",
      {{"commit", json::Quote(p->commit()->str())}});
  return wire::HostReply(request.request_id, std::move(*result));
}
}  // namespace tsugumi::host
