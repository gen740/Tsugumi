#include <algorithm>
#include <charconv>
#include <map>
#include <set>

#include "agent/context.hh"
#include "agent/tools.hh"
#include "host/access.hh"
#include "host/failure.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/text.hh"

namespace tsugumi::agent {
namespace {
using host::Failure;
namespace H = Tsugumi::Host;
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
  if (file.empty() || file == "_card.json" || file == "_meta.json" ||
      file == "_link.json")
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
}  // namespace

// Shape only the provider view. The saved transcript keeps the original bytes.
auto FileResultForModel(std::string_view raw, size_t limit,
                        std::string_view reference) -> std::string {
  auto envelope = json::Object(raw);
  const auto reply = json::Text(envelope, "reply");
  if (reply != "ProjectShellResult" && reply != "FileContent")
    return ClipMiddle(raw, limit, reference);
  const auto original_value = json::Get(envelope, "value", "{}");
  const bool shell = reply == "ProjectShellResult";
  auto bytes = [](const std::string& encoded) {
    if (encoded.starts_with('"')) return json::String(encoded);
    std::string decoded;
    if (!crypto::base64_decode(json::Text(json::Object(encoded), "base64"),
                               decoded))
      throw std::runtime_error("Invalid file result bytes");
    return decoded;
  };
  auto encode = [](const std::string& data) {
    return text::valid_utf8(data) && data.find('\0') == std::string::npos
               ? json::Quote(data)
               : json::Encode(json::Members{
                     {"base64", json::Quote(crypto::base64(data))}});
  };
  // Budget encoded bytes, including JSON escaping or base64, for both streams
  // and metadata. The same rule also fits a smaller overflow-recovery request.
  for (size_t page_bytes = limit / 4;; page_bytes /= 2) {
    auto value = json::Object(original_value);
    for (const auto* field : {"bytes", "stdout_bytes", "stderr_bytes"}) {
      if (!value.contains(field)) continue;
      const auto original = bytes(value.at(field));
      size_t count = original.size();
      std::string encoded = value.at(field);
      size_t head = count, tail = 0;
      while (encoded.size() > page_bytes && count) {
        count /= 2;
        head = shell ? count / 2 : count;
        tail = shell ? count - head : 0;
        if (text::valid_utf8(original)) {
          while (head && head < original.size() &&
                 (static_cast<unsigned char>(original[head]) & 0xc0) == 0x80)
            --head;
          while (tail &&
                 (static_cast<unsigned char>(original[original.size() - tail]) &
                  0xc0) == 0x80)
            --tail;
        }
        encoded = encode(original.substr(0, head) +
                         original.substr(original.size() - tail));
      }
      value[field] = encoded;
      if (head + tail == original.size()) continue;
      if (shell) {
        const auto prefix = std::string(field).substr(0, 6);
        value[prefix + "_truncated"] = "true";
        value[prefix + "_head_bytes"] = std::to_string(head);
        value[prefix + "_tail_bytes"] = std::to_string(tail);
      } else {
        const auto offset = json::Get(value, "offset", "0");
        const auto begin = json::UInt(json::String(offset, offset));
        value["next_offset"] = json::Quote(std::to_string(begin + head));
        value["eof"] = "false";
      }
    }
    value["hint"] =
        json::Quote(shell ? "For more output, " + std::string(reference) +
                                "; full_output_path holds the complete .stdout "
                                "and .stderr files."
                          : "Continue the same file read at next_offset; "
                            "size_bytes is the full file size. "
                            "Use project_copy to move files without reading "
                            "them, or project_shell to examine large files.");
    envelope["value"] = json::Encode(value);
    if (json::Encode(envelope).size() > limit && shell) {
      // A save can have thousands of changed files. Keep its outcome and
      // totals; the saved result and project_inspect expose the complete
      // manifest.
      const auto files = json::Array(json::Get(value, "changed_files", "[]"));
      value["changed_files_count"] = std::to_string(files.size());
      value["changed_files"] = "[]";
      if (value.contains("save")) {
        const auto save = json::Object(value.at("save"));
        json::Members summary;
        for (const auto* key : {"rev", "execution_rev", "op_id", "replayed"})
          if (save.contains(key)) summary[key] = save.at(key);
        value["save"] = json::Encode(summary);
      }
      value["hint"] = json::Quote(
          json::Text(value, "hint") +
          " Use project_inspect for the complete changed-file manifest.");
      envelope["value"] = json::Encode(value);
    }
    const auto shaped = json::Encode(envelope);
    // If metadata alone exceeds the cap, use the general saved-result view of
    // the original receipt, never cut an already shaped page or its offsets.
    if (shaped.size() <= limit) return shaped;
    if (page_bytes <= 2) return ClipMiddle(raw, limit, reference);
  }
}

auto Tools::FileOperation(const std::string& conversation,
                          const std::string& project, const std::string& name,
                          json::Members& args, const std::string& operation,
                          HelperScope* boundary, std::stop_token stop,
                          std::vector<std::string>& resources) -> std::string {
  auto check_cancelled = [&] { if (stop.stop_requested()) throw Cancelled(); };
  check_cancelled();
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
  const auto policy = Exclusions(project, stop);
  const auto snapshot =
      Value(ProjectCall(project, "project_inspect", "{}", stop));
  const auto revision = json::Get(snapshot, "rev");
  if (args.contains("base_rev") &&
      json::UInt(json::String(args.at("base_rev"), args.at("base_rev"))) !=
          json::UInt(json::String(revision, revision)))
    throw Failure("revision_conflict",
                  "The document changed since base_rev; inspect and retry");
  std::filesystem::path root;
  {
    std::lock_guard lock(mutex);
    root = registry.Root(project);
  }
  const bool attached_card =
      json::Text(args, "source") == "attached" && args.contains("card");
  const bool attached_project = json::Text(args, "source") == "attached" &&
                                !attached_card && args.contains("id");
  if (name == "project_copy" && (json::Text(args, "source") == "demo" ||
                                 attached_card || attached_project)) {
    H::ProjectOperateRequestT request;
    request.project_id = project;
    request.op_id = operation;
    if (attached_project) {
      H::ProjectImportT imported;
      imported.project_path = json::Text(args, "project_path");
      imported.path = json::Text(args, "folder");
      imported.id = json::Text(args, "id");
      imported.x = std::stod(json::Get(args, "x", "0"));
      imported.y = std::stod(json::Get(args, "y", "0"));
      request.body.Set(std::move(imported));
    } else {
      H::ProjectCopyT copy;
      copy.project_path = json::Text(args, "project_path");
      auto item = std::make_unique<H::ProjectCopyItemT>();
      if (attached_card)
        item->folder = json::Text(args, "folder");
      else
        item->demo = json::Text(args, "demo");
      item->from = json::Text(args, "card");
      item->id = json::Text(args, "id");
      item->name = json::Text(args, "name");
      item->x = std::stod(json::Get(args, "x", "0"));
      item->y = std::stod(json::Get(args, "y", "0"));
      if (attached_card ? item->folder.empty() : item->demo.empty())
        throw Failure("invalid_request", attached_card ? "folder is required"
                                                       : "demo is required");
      copy.items.push_back(std::move(item));
      request.body.Set(std::move(copy));
    }
    host::OperationContext context;
    context.origin = "agent";
    context.authorize_file = [&](const auto& path, const auto& base) {
      host::AgentExclude(root, json::Text(base.snapshot, "project_json")).Check(path);
    };
    context.authorize = [&](auto& plan, const auto& base) {
      host::AgentExclude(root, json::Text(base.snapshot, "project_json")).CheckSave(plan, base);
      {
        std::lock_guard lock(mutex);
        const auto resource = boundary && !boundary->authorization().empty() ? boundary->authorization() : project;
        for (const auto access : {Tsugumi::Service::Access::Read, Tsugumi::Service::Access::Write})
          if (!access_service.HasGrant(resource, conversation, access))
            throw Failure("permission_denied", "Project access was revoked before saving");
        if (attached_card || attached_project) {
          auto folder =
              db.prepare("SELECT id FROM folders WHERE project=? AND path=?");
          folder.bind(1, project).bind(2, json::Text(args, "folder"));
          if (!folder.step() ||
              !access_service.HasGrant(folder.text(0), conversation,
                                       Tsugumi::Service::Access::Read))
            throw Failure(
                "permission_denied",
                "Attached-folder Read grant was revoked before saving");
        }
      }
      if (args.contains("approved_permissions")) plan["approved_permissions"] = args.at("approved_permissions");
      if (boundary && boundary->scoped())
        boundary->CheckCurrentSave(plan, HelperReader(conversation, stop, json::Get(base.snapshot, "rev"), boundary->authorization()));
    };
    return OperateProject(project, request, context, stop).json;
  }
  const auto read_scope = HelperReader(
      conversation, stop, revision, boundary ? boundary->authorization() : "");
  auto check = [&](const std::string& path, bool writing) {
    policy.Check(path);
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
  const auto private_path = ".tsugumi/agent-files/" + conversation +
                            (boundary && !boundary->scratch_identity().empty()
                                 ? "/helpers/" + boundary->scratch_identity()
                                 : "");
  const auto scratch_path = private_path + "/scratch";
  std::filesystem::create_directories(root / scratch_path);
  std::map<std::string, std::string> hashes;
  std::map<std::string, std::string> source_info;
  for (const auto& raw : json::Array(json::Get(snapshot, "sources", "[]"))) {
    const auto info = json::Object(raw);
    hashes[json::Text(info, "path")] = json::Text(info, "hash");
    source_info[json::Text(info, "path")] = raw;
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
  std::vector<std::string> transfers, removals, summaries;
  auto stage = [&](const std::string& path, const std::string& from,
                   const std::string& source_root = "",
                   const std::string& expected_hash = "") {
    check_cancelled();
    if (!SourcePath(path))
      throw Failure(
          "permission_denied",
          "Files go into an existing Card's sources (definitions/<id>/<file>); "
          "copy a whole Card with card + id: " +
              path);
    const auto card = CardRoot(path);
    if (!hashes.contains(card + "/_card.json") &&
        !hashes.contains(card + "/_link.json"))
      throw Failure("not_found", "Destination Card does not exist: " + card);
    check(path, true);
    json::Members transfer{{"path", json::Quote(path)},
                           {"from", json::Quote(from)}};
    if (!source_root.empty()) transfer["source_root"] = json::Quote(source_root);
    if (!expected_hash.empty()) transfer["expected_hash"] = json::Quote(expected_hash);
    if (hashes.contains(path)) transfer["base_hash"] = json::Quote(hashes.at(path));
    transfers.push_back(json::Encode(transfer));
  };
  json::Members shell_result;
  if (name == "project_copy") {
    const auto source = json::Text(args, "source");
    const auto files = json::Array(json::Get(args, "files", "[]"));
    if (files.empty())
      throw Failure(
          "invalid_request",
          "Supply a non-empty explicit list of files with from and to paths");
    std::set<std::string> destinations;
    for (const auto& raw : files) {
      if (stop.stop_requested()) throw Cancelled();
      const auto file = json::Object(raw);
      const auto from = json::Text(file, "from"), to = json::Text(file, "to");
      if (!destinations.insert(to).second)
        throw Failure("invalid_request", "Duplicate destination: " + to);
      if (!SourcePath(to))
        throw Failure(
            "permission_denied",
            "Files go into an existing Card's sources "
            "(definitions/<id>/<file>); copy a whole Card with card + id: " +
                to);
      check(to, true);
      if (source == "attached") {
        const auto folder = json::Text(args, "folder");
        std::lock_guard lock(mutex);
        const auto attached = registry.AttachedRoot(project, folder);
        policy.Check(std::filesystem::path(folder) / from);
        stage(to, from, attached);
      } else if (source == "card") {
        check(from, false);
        if (!hashes.contains(from)) {
          if (fs::valid_relative_path(from)) {
            std::error_code error;
            const auto type =
                std::filesystem::symlink_status(root / from, error);
            if (!error && std::filesystem::is_directory(type))
              throw Failure("invalid_path",
                            from +
                                " is a folder; list its files or copy the "
                                "whole Card with card + id");
            if (!error && std::filesystem::exists(type) &&
                !std::filesystem::is_regular_file(type))
              throw Failure(
                  "invalid_path",
                  from +
                      " is not a regular file; choose a regular file to copy");
          }
          throw Failure("not_found", "Not a confirmed Card source: " + from);
        }
        stage(to, from, "", hashes.at(from));
      } else if (source == "scratch") {
        policy.Check(root / scratch_path / from);
        if (!fs::valid_relative_path(from)) throw Failure("invalid_path", "Invalid scratch path: " + from);
        stage(to, scratch_path + '/' + from);
      } else
        throw Failure("invalid_request",
                      "source must be attached, card or scratch");
    }
  } else {
    const auto work_path = private_path + "/commands/" + operation;
    const auto work = root / work_path;
    policy.Check(work);
    policy.Check(root / scratch_path);
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
      if (CardRoot(path).empty() || policy.Contains(path)) continue;
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
                       [&](std::string_view chunk) { check_cancelled(); digest.update(chunk); });
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
    std::vector<std::string> mounts, writable_json, protected_json, excluded_aliases;
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
        if (policy.Contains(folder)) continue;
        const auto tag = crypto::sha256_hex(folder).substr(0, 16);
        std::filesystem::create_directories(work / "attached" / tag);
        const auto real_folder = std::filesystem::weakly_canonical(folder);
        for (const auto& excluded : policy.Paths()) {
          const auto relative = excluded.lexically_relative(real_folder);
          if (!relative.empty() && *relative.begin() != "..")
            excluded_aliases.push_back(json::Quote("attached/" + tag + '/' + relative.generic_string()));
        }
        mounts.push_back(json::Encode(json::Members{
            {"path", json::Quote(registry.AttachedRoot(project, folder))},
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
    std::vector<std::string> excluded = excluded_aliases;
    for (const auto& path : policy.Paths()) {
      excluded.push_back(json::Quote(path.string()));
      const auto relative = path.lexically_relative(std::filesystem::weakly_canonical(root));
      // Only content copied into the view gets an alias. Internal state is
      // never staged; it holds the generated workspace itself.
      if (!relative.empty() && *relative.begin() != "..")
        excluded.push_back(json::Quote(relative.generic_string()));
    }
    for (const auto& raw : excluded) {
      const std::filesystem::path path(json::String(raw));
      if (!path.is_absolute() && !path.string().starts_with("attached/") && !std::filesystem::exists(work / path))
        std::filesystem::create_directories(work / path);
    }
    shell_args["excluded_paths"] = json::Encode(excluded);
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
      const auto listing = fs::list_beneath(work, area);
      if (!listing.links.empty() || !listing.special.empty())
        throw Failure(
            "invalid_path",
            "Shell sources cannot contain symbolic links or special files");
      for (const auto& rel : listing.files) {
        const auto path = std::string(area) + '/' + rel;
        seen.insert(path);
        crypto::Sha256 digest;
        fs::stream_beneath(work, path, UINT64_MAX, [&](std::string_view chunk) {
          check_cancelled();
          digest.update(chunk);
        });
        if (initial.contains(path) && initial.at(path) == digest.finish_hex())
          continue;
        if (!SourcePath(path, true))
          throw Failure("permission_denied",
                        "Shell changed a structural file: " + path);
        stage(path, work_path + '/' + path);
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
  const bool scratch_copy =
      name == "project_copy" && json::Text(args, "source") == "scratch";
  args = {{"base_rev", revision},
          {"op_id", json::Quote(operation)},
          {"origin", json::Quote("agent")},
          {"changes", json::Encode(removals)},
          {"transfers", json::Encode(transfers)},
          {"check_permissions", "true"}};
  if (stop.stop_requested()) throw Cancelled();
  auto saved = json::Object(
      SaveProject(project, args, stop));
  if (json::Text(saved, "reply") == "Failure") {
    if (scratch_copy) {
      auto failure = json::Object(json::Get(saved, "value", "{}"));
      auto diagnostic = json::Text(failure, "diagnostic");
      const auto prefix = scratch_path + '/';
      for (auto at = diagnostic.find(prefix); at != std::string::npos;
           at = diagnostic.find(prefix, at))
        diagnostic.erase(at, prefix.size());
      failure["diagnostic"] = json::Quote(diagnostic);
      saved["value"] = json::Encode(failure);
    }
    return json::Encode(saved);
  }
  const auto value = json::Object(json::Get(saved, "value", "{}"));
  const auto delta = json::Object(json::Get(value, "delta", "{}"));
  for (const auto& raw : json::Array(json::Get(delta, "sources_upsert", "[]"))) {
    const auto info = json::Object(raw);
    source_info[json::Text(info, "path")] = raw;
  }
  // The save returns only metadata. Unchanged files retain their inspected
  // metadata; changed files use the core's hash of the bytes it actually saved.
  for (const auto& raw : transfers) {
    const auto path = json::Text(json::Object(raw), "path");
    const auto info = json::Object(source_info.at(path));
    summaries.push_back(json::Encode(json::Members{
        {"path", json::Quote(path)}, {"size_bytes", json::Get(info, "size_bytes")},
        {"hash", json::Get(info, "hash")}}));
  }
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
