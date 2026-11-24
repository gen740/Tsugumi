#include "host/templates.hh"

#include "host/layout_file.hh"

#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string_view>

#include "host/failure.hh"
#include "tsugumi/document/document.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/process.hh"

namespace tsugumi::host {
namespace {
namespace f = std::filesystem;
namespace H = Tsugumi::Host;
namespace json = ::tsugumi::json;

struct Scratch {
  explicit Scratch(const f::path& parent) {
    auto pattern = (parent / ".tsugumi-template-XXXXXX").string();
    if (!::mkdtemp(pattern.data()))
      throw Failure("unavailable",
                    "Cannot create a template staging directory");
    path = pattern;
  }
  ~Scratch() {
    std::error_code ignored;
    f::remove_all(path, ignored);
  }
  f::path path;
};

auto Read(const f::path& root, const f::path& relative,
          const TemplatePathCheck& check = {}) -> TemplateFiles {
  if (!f::is_directory(f::symlink_status(root)))
    throw Failure("not_found", "Template folder not found");
  const auto listing = fs::list_beneath(root, relative, kTemplateEntries);
  if (!listing.links.empty() || !listing.special.empty())
    throw Failure("invalid_path",
                  "Templates may contain only files and folders");
  TemplateFiles files;
  size_t used = 0;
  for (const auto& path : listing.files) {
    if (f::path(path).filename() == ".DS_Store") continue;
    const auto source = relative / path;
    if (!Document::valid_version_path(path) ||
        path.find(".tsugumi-tmp-") != std::string::npos)
      throw Failure("invalid_path", "Invalid template file path: " + path);
    if (check) check((root / source).string());
    std::string bytes;
    used += fs::stream_beneath(root, source, kTemplateBytes - used,
                               [&](std::string_view chunk) { bytes += chunk; });
    files.emplace(path, std::move(bytes));
  }
  return files;
}

auto GitTemplate(const H::TemplateGitT& source, std::stop_token stop)
    -> TemplateFiles {
  const std::string_view url = source.url;
  const auto rest =
      url.starts_with("https://") ? url.substr(8) : std::string_view{};
  const auto slash = rest.find('/');
  if (slash == std::string_view::npos || slash == 0 ||
      rest.substr(0, slash).find_first_of("@%") != std::string_view::npos ||
      rest.find_first_of("?#\\") != std::string_view::npos ||
      std::ranges::any_of(rest,
                          [](unsigned char c) { return c <= 32 || c == 127; }))
    throw Failure("invalid_request",
                  "Use a public HTTPS Git URL without credentials");
  if (source.ref.empty() || source.ref.starts_with('-') ||
      source.ref.find_first_of(":*") != std::string::npos ||
      source.ref.find('\0') != std::string::npos)
    throw Failure("invalid_request", "Choose a Git branch, tag or commit");
  if (!Document::valid_version_path(source.subdirectory))
    throw Failure("invalid_path",
                  "Choose a repository-relative template folder");

  std::string executable = TSUGUMI_TEMPLATE_GIT;
  f::path bundled_git;
#ifdef __APPLE__
  if (executable == "@bundle/git") {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0)
      throw Failure("unavailable", "Cannot locate host executable");
    bundled_git =
        (f::canonical(f::path(path.c_str())).parent_path() / "../Resources/git")
            .lexically_normal();
    executable = (bundled_git / "git").string();
  }
#endif
  Scratch scratch(f::temp_directory_path());
  std::atomic<bool> cancel{stop.stop_requested()};
  std::stop_callback cancelled(stop, [&] { cancel.store(true); });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::minutes(2);
  auto git = [&](std::vector<std::string> args) {
    proc::Spec spec;
    spec.argv = {executable, "--literal-pathspecs",
                 "-c",       "core.hooksPath=/dev/null",
                 "-c",       "http.followRedirects=false",
                 "-c",       "credential.helper="};
    spec.argv.insert(spec.argv.end(), args.begin(), args.end());
    spec.cwd = scratch.path.string();
    spec.env = {"PATH=/usr/bin:/bin",    "HOME=" + spec.cwd,
                "GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null",
                "GIT_TERMINAL_PROMPT=0", "GIT_ALLOW_PROTOCOL=https"};
    if (!bundled_git.empty()) {
      spec.env.emplace_back("GIT_EXEC_PATH=" + bundled_git.string());
      spec.env.emplace_back("GIT_SSL_CAINFO=/etc/ssl/cert.pem");
    }
    if (const auto* cert = std::getenv("SSL_CERT_FILE"))
      spec.env.emplace_back(std::string("SSL_CERT_FILE=") + cert);
    std::string diagnostic;
    const auto result = proc::run(
        spec, &cancel, deadline,
        [&](auto chunk) {
          for (const auto& line : chunk.lines)
            if (diagnostic.size() < 2048) diagnostic += line + '\n';
        },
        2048);
    if (!proc::exited_ok(result.status) || result.cancelled || result.timed_out)
      throw Failure("template_import_failed",
                    result.cancelled   ? "Git import cancelled"
                    : result.timed_out ? "Git import timed out"
                                       : diagnostic);
  };
  git({"check-ref-format", "--allow-onelevel", source.ref});
  git({"init", "--quiet", "--template="});
  git({"fetch", "--quiet", "--depth=1", "--no-tags", "--", source.url,
       source.ref});
  git({"checkout", "--quiet", "FETCH_HEAD", "--", source.subdirectory});
  return Read(scratch.path, source.subdirectory);
}
}  // namespace

auto BuiltinCard(H::BuiltinCard kind, const std::string& id) -> TemplateFiles {
  if (!valid_name(id)) throw Failure("invalid_request", "Invalid Card id");
  const auto card = [&](std::string_view definition) -> TemplateFiles {
    return {{"_card.json", std::string(definition)},
            {"README.md", CardReadme(id)}};
  };
  switch (kind) {
    case H::BuiltinCard::Draft:
      return card(R"({"type":"draft","symbol":"pencil.and.outline"})");
    case H::BuiltinCard::Trigger:
      return card(R"({"type":"trigger","symbol":"play.circle"})");
    case H::BuiltinCard::Merge:
      return card(
          R"({"type":"merge","symbol":"arrow.triangle.merge","inputs":[{"name":"a"},{"name":"b"}],"maxRuns":1,"recordIO":"project"})");
    case H::BuiltinCard::Collect:
      return card(
          R"({"type":"collect","symbol":"line.3.horizontal.decrease","maxRuns":1,"recordIO":"project"})");
    case H::BuiltinCard::InputValue:
    case H::BuiltinCard::BooleanInput: {
      const bool boolean = kind == H::BuiltinCard::BooleanInput;
      const std::string type = boolean ? "bool" : "double";
      const std::string value = boolean ? "false" : "0.0";
      const std::string table = boolean ? "Boolean" : "Number";
      auto files = card(R"({
  "type":"input",
  "symbol":"slider.horizontal.3",
  "inputs":[],
  "maxRuns":1,
  "reuseWorker":false,
  "timeoutSeconds":0,
  "buildTimeoutSeconds":0,
  "recordLogs":"project",
  "recordIO":"project",
  "sandbox":{"filesystem":true,"pid":true,"ipc":true,"identity":true,
             "limits":{"cpu":0,"memoryMiB":0}},
  "grants":{"network":false,"gui":false},
  "workerCapabilities":[]
})");
      auto definition = json::Object(files.at("_card.json"));
#if defined(__aarch64__) || defined(__arm64__)
      definition["system"] = json::Quote("aarch64-linux");
#else
      definition["system"] = json::Quote("x86_64-linux");
#endif
      files["_card.json"] = json::Encode(definition);
      files["params.nix"] = "{ value = " + value + "; }\n";
      files["_outputs.fbs"] = "namespace input;\ntable " + table +
                              " { value:" + type +
                              "; }\ntable Outputs { value:" + table +
                              " (required); }\nroot_type Outputs;\n";
      files["default.nix"] = R"nix({ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/input> { inherit pkgs; };
in {
  run = sdk.input {
    outputs = ./_outputs.fbs;
    values.value.value = params.value;
  };
}
)nix";
      return files;
    }
  }
  throw Failure("invalid_request", "Unknown built-in Card kind");
}

auto TemplateName(const std::string& name) -> bool {
  return !name.empty() && name.size() <= 128 && name.front() != '.' &&
         name.back() != ' ' && name.back() != '.' && name.front() != ' ' &&
         fs::valid_relative_path(name) &&
         std::ranges::none_of(name, [](unsigned char c) {
           return c < 32 || c == 127 ||
                  std::string_view("/<>:\"|?*").find(c) !=
                      std::string_view::npos;
         });
}

auto CheckTemplate(TemplateFiles files) -> DocumentTemplate {
  std::set<std::string> cards;
  size_t used = 0;
  for (const auto& [path, bytes] : files) {
    if (!Document::valid_version_path(path) ||
        path.find(".tsugumi-tmp-") != std::string::npos)
      throw Failure("invalid_path", "Invalid template file path: " + path);
    if (bytes.size() > kTemplateBytes - used || files.size() > kTemplateEntries)
      throw Failure("resource_limit", "Template exceeds the import limit");
    used += bytes.size();
    const auto split = path.find('/');
    if (split != std::string::npos) cards.insert(path.substr(0, split));
  }
  if (cards.empty())
    throw Failure(
        "invalid_document",
        "Choose a template folder containing Card definition directories");
  for (const auto& id : cards) {
    if (!valid_name(id) || !files.contains(id + "/_card.json"))
      throw Failure("invalid_document",
                    "Invalid Card definition in template: " + id);
    const auto path = id + "/_card.json";
    const auto& bytes = files.at(path);
    parse_card_json(id, bytes);
    const auto readme = files.find(id + "/README.md");
    if (readme == files.end())
      throw Failure("invalid_document", id + "/README.md is required");
    check_readme(readme->first, readme->second);
    if (json::Text(json::Object(bytes), "type") == "subgraph")
      throw Failure(
          "invalid_document",
          path + ": a definition template does not contain child projects");
  }
  // Inspect the layout while keeping the stored bytes for both template stores.
  if (files.contains("layout.bin")) {
    json::Members layout;
    try {
      layout = json::Object(LayoutView(files.at("layout.bin")));
    } catch (const std::exception&) {
      throw Failure("invalid_document", "Template layout.bin is not a layout");
    }
    if (layout.contains("displays") || layout.contains("frames"))
      throw Failure("invalid_document", "Template layout must be frame-free");
    for (const auto& [id, raw] :
         json::Object(json::Get(layout, "cards", "{}"))) {
      const auto point = json::Object(raw);
      if (!cards.contains(id))
        throw Failure("invalid_document",
                      "Template layout refers to an unknown Card: " + id);
      for (const auto* axis : {"x", "y"}) {
        if (!point.contains(axis)) continue;
        const auto& field = point.at(axis);
        size_t end = 0;
        try {
          const auto coordinate = std::stod(field, &end);
          if (end == field.size() && std::isfinite(coordinate)) continue;
        } catch (const std::logic_error&) {
          // Invalid or out-of-range JSON coordinate.
        }
        throw Failure("invalid_document", "Invalid template Card position");
      }
    }
  }
  if (files.contains("tsugumi.json")) {
    const auto& bytes = files.at("tsugumi.json");
    parse_project_json(bytes);
    const auto project = json::Object(bytes);
    const auto canvas = json::Object(json::Get(project, "canvas", "{}"));
    std::set<std::string> placed;
    for (const auto& raw : json::Array(canvas.at("cards")))
      if (!placed.insert(json::Text(json::Object(raw), "id")).second)
        throw Failure("invalid_document", "Duplicate Card in template canvas");
    if (placed != cards)
      throw Failure("invalid_document",
                    "Template canvas and definition folders must match");
    for (const auto& raw : json::Array(canvas.at("connections"))) {
      const auto wire = json::Object(raw);
      for (const auto* end : {"from", "to"}) {
        const auto endpoint = json::Object(json::Get(wire, end, "{}"));
        if (!cards.contains(json::Text(endpoint, "card")))
          throw Failure("invalid_document",
                        "Template connection leaves the template");
      }
    }
  }
  return {std::move(files), {cards.begin(), cards.end()}};
}

auto ReadTemplateSource(const H::TemplateAddT& request, std::stop_token stop,
                        const TemplatePathCheck& check) -> DocumentTemplate {
  if (!TemplateName(request.name))
    throw Failure("invalid_request", "Choose a valid template name");
  if (const auto* folder = request.source.AsTemplateFolder()) {
    const f::path path(folder->path);
    if (!path.is_absolute() || folder->path.find('\0') != std::string::npos)
      throw Failure("invalid_path", "Choose an absolute host folder path");
    // Canonicalize the parent (including system aliases such as /tmp on macOS).
    const auto normalized = path.lexically_normal();
    if (check) check(normalized.string());
    const auto parent = f::canonical(normalized.parent_path());
    if (check) check((parent / normalized.filename()).string());
    return CheckTemplate(Read(parent, normalized.filename(), check));
  }
  if (const auto* git = request.source.AsTemplateGit())
    return CheckTemplate(GitTemplate(*git, stop));
  throw Failure("invalid_request", "Choose a template source");
}

auto ReadHostTemplate(const f::path& store, const std::string& name,
                      const TemplatePathCheck& check) -> DocumentTemplate {
  if (!TemplateName(name))
    throw Failure("invalid_request", "Choose a valid template name");
  if (check) check((store / name).string());
  if (!f::is_directory(f::symlink_status(store / name)))
    throw Failure("not_found", "Host template not found");
  return CheckTemplate(Read(store, name, check));
}

auto HostTemplateNames(const f::path& store, const TemplatePathCheck& check)
    -> std::vector<std::string> {
  std::vector<std::string> names;
  if (!f::is_directory(f::symlink_status(store))) return names;
  for (const auto& entry : f::directory_iterator(store)) {
    const auto name = entry.path().filename().string();
    if (!TemplateName(name) || !f::is_directory(entry.symlink_status()))
      continue;
    try {
      if (check) check(entry.path().string());
      const auto listing = fs::list_beneath(store, name, kTemplateEntries);
      if (!listing.links.empty() || !listing.special.empty()) continue;
      if (check)
        for (const auto& path : listing.files)
          check((entry.path() / path).string());
      if (std::ranges::any_of(listing.files, [](const auto& path) {
            const auto slash = path.find('/');
            return path.ends_with("/_card.json") && slash == path.rfind('/') &&
                   valid_name(path.substr(0, slash));
          }))
        names.push_back(name);
    } catch (const fs::Error&) {
      // A folder may disappear between enumeration and opening.
    } catch (const Failure& error) {
      if (error.code != "not_found") throw;
    }
  }
  std::ranges::sort(names);
  return names;
}

auto AddHostTemplate(const f::path& store, const std::string& name,
                     const DocumentTemplate& contents) -> void {
  f::create_directories(store);
  if (!f::is_directory(f::symlink_status(store)))
    throw Failure("invalid_path",
                  "The host template store must be a directory");
  const auto destination = store / name;
  if (f::exists(f::symlink_status(destination)))
    throw Failure("value_conflict", "A Host template already has this name");
  Scratch staging(store);
  for (const auto& [path, bytes] : contents.files)
    fs::write_atomic(staging.path / path, bytes);
  f::rename(staging.path, destination);
  fs::fsync_dir(store);
}
}  // namespace tsugumi::host
