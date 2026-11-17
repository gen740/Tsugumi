#include "host/skills_service.hh"

#include <filesystem>

#include "tsugumi/util/json_object.hh"
#include "agent/wire.hh"
#include "host/failure.hh"
#include "host/settings.hh"
#include "host/skills.hh"

namespace tsugumi::host {
namespace H = Tsugumi::Host;
namespace json = ::tsugumi::json;
namespace wire = agent::wire;
namespace {
auto Text(const flatbuffers::String* value) -> std::string {
  return value ? value->str() : "";
}
auto Human(bool human) -> void {
  if (!human)
    throw Failure("permission_denied",
                  "This operation requires the authenticated user");
}
}  // namespace
SkillsService::SkillsService(sql::Db& db, Settings& settings)
    : db(db), settings(settings) {}

auto SkillsService::Handle(const RequestIn& request, bool human,
                           std::unique_lock<std::recursive_mutex>& lock)
    -> std::string {
  const auto* r = H::GetMessage(request.frame->data())->payload_as_Request();
  Human(human);
  if (const auto* trust = r->value_as_ProjectTrustRequest()) {
    const auto path = std::filesystem::canonical(trust->path()->str()).string();
    if (!std::filesystem::is_regular_file(std::filesystem::path(path) /
                                          "tsugumi.json"))
      throw Failure("invalid_request", "Choose a project folder");
    if (trust->decision() > H::ProjectTrustDecision::MAX)
      throw Failure("invalid_request", "Unknown project trust decision");
    if (trust->decision() != H::ProjectTrustDecision::Unknown) {
      SetProjectTrust(db, path, trust->decision());
    }
    H::ProjectTrustT result;
    const auto folders = settings.TrustedFolders();
    result.decision = ProjectTrust(db, path, folders);
    result.trusted_folder = TrustedParent(path, folders);
    return wire::HostReply(request.request_id, std::move(result));
  }
  const auto* p = r->value_as_SkillsListRequest();
  std::vector<std::string> folders;
  if (p->folders())
    for (const auto* folder : *p->folders())
      folders.push_back(
          std::filesystem::path(folder->str()).lexically_normal().string());
  const auto path = Text(p->project_path()).empty()
                        ? ""
                        : std::filesystem::path(p->project_path()->str())
                              .lexically_normal()
                              .string();
  if (!Text(p->conversation_id()).empty()) {
    std::vector<std::string> encoded;
    for (const auto& folder : folders) encoded.push_back(json::Quote(folder));
    auto q = db.prepare("INSERT OR REPLACE INTO skill_catalogs VALUES(?,?,?)");
    q.bind(1, p->conversation_id()->str())
        .bind(2, path)
        .bind(3, json::Encode(encoded))
        .run();
  }
  const auto trusted = ProjectTrusted(db, path, settings.TrustedFolders());
  lock.unlock();
  return wire::HostReply(request.request_id,
                         host::ListSkills(path, folders, trusted));
}
}  // namespace tsugumi::host
