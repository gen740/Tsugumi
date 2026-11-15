#include <algorithm>
#include <charconv>
#include <map>
#include <set>

#include "agent/tools.hh"
#include "host/access.hh"
#include "host/failure.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi::agent {
namespace {
using host::Failure;
constexpr uint64_t kTransferBytes = 12u << 20;
const std::set<std::string> kProtected{
    "tsugumi.json", "layout.json", "_card.json",   "_meta.json",
    "_link.json",   "_inputs.fbs", "_outputs.fbs", "_params.fbs"};
auto CardRoot(const std::string& path) -> std::string {
  if (!fs::valid_relative_path(path)) return {};
  auto rest = path;
  size_t prefix = 0;
  while (rest.starts_with("externals/")) {
    const auto end = rest.find('/', 10);
    if (end == std::string::npos) return {};
    prefix += end + 1;
    rest.erase(0, end + 1);
  }
  if (!rest.starts_with("definitions/")) return {};
  const auto end = rest.find('/', 12);
  if (end == std::string::npos || end == 12) return {};
  return path.substr(0, prefix + end);
}
auto SourcePath(const std::string& path, bool shell = false) -> bool {
  const auto root = CardRoot(path);
  if (root.empty()) return false;
  for (const auto& part : std::filesystem::path(path))
    if (part == ".tsugumi" || part == ".git") return false;
  const auto file = path.substr(root.size() + 1);
  if (file == "_card.json" || file == "_meta.json" || file == "_link.json")
    return false;
  return !shell || !kProtected.contains(file);
}
auto Value(const std::string& result) -> json::Members {
  const auto envelope = json::Object(result);
  const auto value = json::Object(json::Get(envelope, "value", "{}"));
  if (json::Text(envelope, "reply") == "Failure")
    throw Failure(json::Text(value, "kind"), json::Text(value, "message"));
  return value;
}
auto Summary(const std::string& path, const std::string& bytes) -> std::string {
  return json::Encode(
      json::Members{{"path", json::Quote(path)},
                    {"size_bytes", json::Quote(std::to_string(bytes.size()))},
                    {"hash", json::Quote(crypto::sha256_hex(bytes))}});
}
auto Read(const std::filesystem::path& root, const std::string& path,
          uint64_t limit = kTransferBytes) -> std::string {
  std::string bytes;
  try {
    fs::stream_beneath(root, path, limit,
                       [&](std::string_view chunk) { bytes.append(chunk); });
  } catch (const fs::Error& error) {
    throw Failure(
        std::string(error.what()).find("transfer limit") != std::string::npos
            ? "resource_limit"
            : "invalid_path",
        std::string(error.what()) +
            ". Copy/save is limited to 12 MiB per operation. For larger data, "
            "let the Card read the attached folder through hostFolders "
            "(/host/<name>), with the person's permission.");
  }
  return bytes;
}
}  // namespace

auto Tools::FileOperation(const std::string& conversation,
                          const std::string& project, const std::string& name,
                          json::Members& args, const std::string& operation,
                          HelperScope* boundary, std::stop_token stop,
                          std::vector<std::string>& resources) -> std::string {
  if (name == "project_copy" && !args.contains("base_rev"))
    throw Failure("invalid_request",
                  "project_copy requires base_rev from project_inspect");
  if (args.contains("base_rev")) {
    const auto raw = json::String(args.at("base_rev"), args.at("base_rev"));
    uint64_t parsed = 0;
    const auto number =
        std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
    if (number.ec != std::errc{} || number.ptr != raw.data() + raw.size())
      throw Failure("invalid_request",
                    "base_rev must be an unsigned decimal integer");
  }
  const auto snapshot =
      Value(ProjectCall(project, "project_inspect", "{}", stop));
  const auto revision = json::Get(snapshot, "rev");
  if (args.contains("base_rev") &&
      json::UInt(json::String(args.at("base_rev"), args.at("base_rev"))) !=
          json::UInt(json::String(revision, revision)))
    throw Failure("revision_conflict",
                  "The document changed since base_rev; inspect and retry");
  const auto read_scope = HelperReader(
      conversation, stop, revision, boundary ? boundary->authorization() : "");
  auto check = [&](const std::string& path, bool writing) {
    if (!boundary || !boundary->scoped()) return;
    if (writing)
      boundary->Check(
          "project_save",
          {{"changes",
            json::Encode(std::vector<std::string>{json::Encode(json::Members{
                {"value_type", json::Quote("PutSource")},
                {"value", json::Encode(json::Members{
                              {"path", json::Quote(path)}})}})})}},
          read_scope);
    else
      boundary->Check("project_file_read", {{"path", json::Quote(path)}},
                      read_scope);
  };
  std::filesystem::path root;
  {
    std::lock_guard lock(mutex);
    root = registry.Root(project);
  }
  const auto private_path =
      ".tsugumi/agent-files/" + crypto::sha256_hex(conversation);
  const auto staged_path = private_path + "/edits/" + operation;
  const auto scratch_path = private_path + "/scratch";
  std::filesystem::create_directories(root / staged_path);
  std::filesystem::create_directories(root / scratch_path);
  std::map<std::string, std::string> hashes;
  for (const auto& raw : json::Array(json::Get(snapshot, "sources", "[]"))) {
    const auto info = json::Object(raw);
    hashes[json::Text(info, "path")] = json::Text(info, "hash");
  }
  for (const auto& raw :
       json::Array(json::Get(snapshot, "definitions", "[]"))) {
    const auto definition = json::Object(raw);
    const auto dir = "definitions/" + json::Text(definition, "id") + '/';
    for (const auto& [field, file] : {std::pair{"card_json", "_card.json"},
                                      {"meta_json", "_meta.json"},
                                      {"link_json", "_link.json"}}) {
      const auto bytes = json::Text(definition, field);
      if (!bytes.empty()) hashes[dir + file] = crypto::sha256_hex(bytes);
    }
  }
  auto confirmed = [&](const std::string& path) {
    check(path, false);
    const auto found = hashes.find(path);
    if (found == hashes.end())
      throw Failure("not_found", "Not a confirmed Card source: " + path);
    auto bytes = Read(root, path);
    if (crypto::sha256_hex(bytes) != found->second)
      throw Failure(
          "external_change",
          "Source changed outside Tsugumi; Reload before retrying: " + path);
    return bytes;
  };
  std::vector<std::string> transfers, removals, summaries;
  uint64_t total = 0;
  auto stage = [&](const std::string& path, const std::string& bytes) {
    if (!SourcePath(path))
      throw Failure("permission_denied",
                    "Destination must be a Card source file: " + path);
    const auto card = CardRoot(path);
    if (!hashes.contains(card + "/_card.json") &&
        !hashes.contains(card + "/_link.json"))
      throw Failure("not_found", "Destination Card does not exist: " + card);
    check(path, true);
    if (bytes.size() > kTransferBytes - total)
      throw Failure("resource_limit",
                    "Copy/save exceeds 12 MiB per operation. Keep larger data "
                    "in an attached folder and read it from the Card with "
                    "hostFolders (/host/<name>).");
    total += bytes.size();
    const auto from = staged_path + "/" + std::to_string(transfers.size());
    fs::write_atomic(root / from, bytes, 0600);
    json::Members transfer{{"path", json::Quote(path)},
                           {"from", json::Quote(from)}};
    if (hashes.contains(path))
      transfer["base_hash"] = json::Quote(hashes.at(path));
    transfers.push_back(json::Encode(transfer));
    summaries.push_back(Summary(path, bytes));
  };
  json::Members shell_result;
  if (name == "project_copy") {
    const auto source = json::Text(args, "source");
    const auto files = json::Array(json::Get(args, "files", "[]"));
    if (files.empty() || files.size() > 1000)
      throw Failure(
          "invalid_request",
          "Supply an explicit list of 1..1000 files with from and to paths");
    std::set<std::string> destinations;
    for (const auto& raw : files) {
      if (stop.stop_requested()) throw Cancelled();
      const auto file = json::Object(raw);
      const auto from = json::Text(file, "from"), to = json::Text(file, "to");
      if (!destinations.insert(to).second)
        throw Failure("invalid_request", "Duplicate destination: " + to);
      if (!SourcePath(to))
        throw Failure("permission_denied",
                      "Destination must be a Card source: " + to);
      check(to, true);
      if (source == "attached")
        stage(to,
              Read(json::Text(args, "folder"), from, kTransferBytes - total));
      else if (source == "card")
        stage(to, confirmed(from));
      else if (source == "scratch")
        stage(to, Read(root / scratch_path, from, kTransferBytes - total));
      else
        throw Failure("invalid_request",
                      "source must be attached, card or scratch");
    }
  } else {
    const auto work_path = private_path + "/commands/" + operation;
    const auto work = root / work_path;
    // A command has a fresh view; scratch and full output are conversation
    // data.
    if (std::filesystem::exists(work))
      throw Failure(
          "outcome_unknown",
          "This shell workspace already exists; inspect its previous outcome");
    std::filesystem::create_directories(work / "attached");
    std::filesystem::create_directories(work / "scratch");
    if (!boundary || !boundary->scoped()) {
      fs::write_atomic(work / "tsugumi.json",
                       json::Text(snapshot, "project_json"));
      fs::write_atomic(work / "layout.json",
                       json::Text(snapshot, "layout_json", "{}"));
    }
    std::set<std::string> writable, protected_paths;
    std::map<std::string, std::string> initial;
    for (const auto& [path, hash] : hashes) {
      if (CardRoot(path).empty()) continue;
      // Write scope permits creating/replacing sources without exposing an
      // existing file when this Card is absent from the separate read set.
      try {
        check(CardRoot(path) + "/source", true);
        writable.insert(CardRoot(path));
        std::filesystem::create_directories(work / CardRoot(path));
      } catch (const std::exception&) {
      }
      try {
        check(path, false);
      } catch (const std::exception&) {
        continue;
      }
      // Copy confirmed bytes directly; even a large existing source stays out
      // of both the model context and a whole-file host buffer.
      crypto::Sha256 digest;
      fs::copy_beneath(root, path, work / path,
                       [&](std::string_view chunk) { digest.update(chunk); });
      if (digest.finish_hex() != hash)
        throw Failure("external_change",
                      "Reload external edits before running shell: " + path);
      initial[path] = hash;
      bool may_write = SourcePath(path, true);
      if (may_write) try {
          check(path, true);
        } catch (const std::exception&) {
          may_write = false;
        }
      if (may_write)
        writable.insert(CardRoot(path));
      else
        protected_paths.insert(path);
    }
    for (const auto& dir : writable)
      for (const auto& file : kProtected) {
        const auto path = dir + '/' + file;
        if (!std::filesystem::exists(work / path))
          fs::write_atomic(work / path, "");
        protected_paths.insert(path);
        initial[path] = fs::sha256_file(work / path);
      }
    std::vector<std::string> mounts, writable_json, protected_json;
    std::vector<std::string> folder_resources;
    {
      std::lock_guard lock(mutex);
      auto folders = db.prepare(
          "SELECT id,path FROM folders WHERE project=? ORDER BY path");
      folders.bind(1, project);
      while (folders.step()) {
        if (!access_service.HasGrant(folders.text(0), conversation,
                                     Tsugumi::Service::Access::Read))
          continue;
        const auto folder = folders.text(1);
        const auto tag = crypto::sha256_hex(folder).substr(0, 16);
        std::filesystem::create_directories(work / "attached" / tag);
        mounts.push_back(json::Encode(json::Members{
            {"path", json::Quote(folder)},
            {"fallback_path", json::Quote("/run/tsugumi/chat/" + tag)},
            {"name", json::Quote("attached/" + tag)}}));
        folder_resources.push_back(folders.text(0));
        resources.push_back(folders.text(0));
      }
    }
    mounts.push_back(
        json::Encode(json::Members{{"path", json::Quote(scratch_path)},
                                   {"project_relative", "true"},
                                   {"name", json::Quote("scratch")},
                                   {"writable", "true"}}));
    for (const auto& path : writable)
      writable_json.push_back(json::Quote(path));
    for (const auto& path : protected_paths)
      protected_json.push_back(json::Quote(path));
    const auto output_path = private_path + "/output/" + operation;
    std::filesystem::create_directories((root / output_path).parent_path());
    auto shell_args = args;
    auto command_operation = operation;
    command_operation[0] = operation[0] == '0' ? '1' : '0';
    shell_args["op_id"] = json::Quote(command_operation);
    shell_args["working_directory"] = json::Quote(work_path);
    shell_args["writable_paths"] = json::Encode(writable_json);
    shell_args["protected_paths"] = json::Encode(protected_json);
    shell_args["mounts"] = json::Encode(mounts);
    shell_args["workspace_only"] = "true";
    shell_args["output_prefix"] = json::Quote(output_path);
    shell_args.erase("base_rev");
    const auto shell_reply =
        ProjectCall(project, "project_shell", json::Encode(shell_args), stop);
    shell_result = json::Object(shell_reply);
    if (json::Text(shell_result, "reply") == "Failure") return shell_reply;
    auto output = json::Object(json::Get(shell_result, "value", "{}"));
    output["full_output_path"] = json::Quote((root / output_path).string());
    output["changed_files"] = "[]";
    shell_result["value"] = json::Encode(output);
    {
      std::lock_guard lock(mutex);
      for (const auto& resource : folder_resources) {
        if (!access_service.HasGrant(resource, conversation,
                                     Tsugumi::Service::Access::Read))
          throw Failure("permission_denied",
                        "Attached folder grant was revoked during shell");
        conversations->RememberResource(conversation, resource);
      }
    }
    if (stop.stop_requested()) throw Cancelled();
    if (json::Get(output, "exit_code", "-1") != "0" ||
        json::Bool(json::Get(output, "cancelled")) ||
        json::Bool(json::Get(output, "timed_out")))
      return json::Encode(shell_result);
    // Read only the project portion, never walk mounted attached
    // content/scratch.
    std::set<std::string> seen;
    for (const auto* area : {"definitions", "externals"}) {
      if (!std::filesystem::exists(work / area)) continue;
      const auto listing = fs::list_beneath(work, area, 10000);
      if (!listing.links.empty() || !listing.special.empty())
        throw Failure(
            "invalid_path",
            "Shell sources cannot contain symbolic links or special files");
      for (const auto& rel : listing.files) {
        const auto path = std::string(area) + '/' + rel;
        seen.insert(path);
        crypto::Sha256 digest;
        fs::stream_beneath(work, path, UINT64_MAX, [&](std::string_view chunk) {
          digest.update(chunk);
        });
        if (initial.contains(path) && initial.at(path) == digest.finish_hex())
          continue;
        if (!SourcePath(path, true))
          throw Failure("permission_denied",
                        "Shell changed a structural file: " + path);
        stage(path, Read(work, path, kTransferBytes - total));
      }
    }
    for (const auto& [path, hash] : initial)
      if (!seen.contains(path)) {
        if (!SourcePath(path, true))
          throw Failure("permission_denied",
                        "Shell removed a structural file: " + path);
        check(path, true);
        removals.push_back(json::Encode(json::Members{
            {"value_type", json::Quote("RemoveSource")},
            {"value",
             json::Encode(json::Members{{"path", json::Quote(path)},
                                        {"base_hash", json::Quote(hash)}})}}));
        summaries.push_back(
            json::Encode(json::Members{{"path", json::Quote(path)},
                                       {"size_bytes", "\"0\""},
                                       {"removed", "true"}}));
      }
  }
  if (transfers.empty() && removals.empty()) return json::Encode(shell_result);
  {
    std::lock_guard lock(mutex);
    const auto resource = boundary && !boundary->authorization().empty()
                              ? boundary->authorization()
                              : project;
    for (const auto access :
         {Tsugumi::Service::Access::Read, Tsugumi::Service::Access::Write})
      if (!access_service.HasGrant(resource, conversation, access))
        throw Failure("permission_denied",
                      "Project access was revoked before saving");
    if (name == "project_copy" && json::Text(args, "source") == "attached") {
      auto folder =
          db.prepare("SELECT id FROM folders WHERE project=? AND path=?");
      folder.bind(1, project).bind(2, json::Text(args, "folder"));
      if (!folder.step() ||
          !access_service.HasGrant(folder.text(0), conversation,
                                   Tsugumi::Service::Access::Read))
        throw Failure("permission_denied",
                      "Attached-folder Read grant was revoked before saving");
    }
    if (name == "project_shell" &&
        !access_service.HasGrant(project, conversation,
                                 Tsugumi::Service::Access::Shell))
      throw Failure("permission_denied",
                    "Shell grant was revoked before saving");
  }
  args = {{"base_rev", revision},
          {"op_id", json::Quote(operation)},
          {"origin", json::Quote("agent")},
          {"changes", json::Encode(removals)},
          {"transfers", json::Encode(transfers)},
          {"check_permissions", "true"}};
  if (stop.stop_requested()) throw Cancelled();
  auto saved = json::Object(
      ProjectCall(project, "project_save", json::Encode(args), stop));
  if (json::Text(saved, "reply") == "Failure") return json::Encode(saved);
  if (name == "project_shell") {
    auto output = json::Object(json::Get(shell_result, "value", "{}"));
    output["changed_files"] = json::Encode(summaries);
    output["save"] = json::Get(saved, "value", "{}");
    shell_result["value"] = json::Encode(output);
    return json::Encode(shell_result);
  }
  auto result = json::Object(json::Get(saved, "value", "{}"));
  result["files"] = json::Encode(summaries);
  saved["value"] = json::Encode(result);
  return json::Encode(saved);
}
}  // namespace tsugumi::agent
