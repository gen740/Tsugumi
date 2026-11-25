#include "packed_builds.hh"

#include <algorithm>
#include <stdexcept>
#include <string_view>

#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi {
namespace {

namespace stdfs = std::filesystem;

constexpr std::string_view kPackedSystem = "aarch64-darwin";

auto invalid(std::string_view diagnostic) -> std::runtime_error {
  return std::runtime_error("invalid packed build manifest: " +
                            std::string(diagnostic));
}

auto string_array(const std::string& raw) -> std::vector<std::string> {
  schema::v::Array<schema::v::String> strings;
  if (auto error = schema::decode(raw, strings)) throw invalid(error->message);
  return std::move(strings.value());
}

auto bundled_path(const stdfs::path& root, const std::string& relative)
    -> stdfs::path {
  const stdfs::path path(relative);
  if (path.empty() || path.is_absolute() ||
      std::ranges::any_of(path,
                          [](const auto& element) {
                            return element.empty() || element == "." ||
                                   element == "..";
                          }) ||
      relative.find('\0') != std::string::npos)
    throw invalid("invalid bundle-relative path " + relative);
  std::error_code error;
  const auto resolved = stdfs::canonical(root / path, error);
  if (error || !resolved.string().starts_with(root.string() + "/"))
    throw invalid("missing or out-of-bundle path " + relative);
  return root / path;
}

auto card_root(const stdfs::path& root, const std::string& relative)
    -> stdfs::path {
  const stdfs::path path(relative);
  auto component = path.begin();
  if (component == path.end() || *component != "cards")
    throw invalid("package is not under cards/ " + relative);
  ++component;
  if (component == path.end())
    throw invalid("package has no Card directory " + relative);
  return root / "cards" / *component;
}

auto read_card(const json::Members& entry, const stdfs::path& manifest,
               const stdfs::path& resources, const Node& node)
    -> CardBuildPlan {
  schema::CardPlan product;
  if (auto error = schema::decode_card_plan(json::Get(entry, "plan"), product))
    throw invalid(error->message);
  CardBuildPlan plan;
  plan.common.derivation.output = manifest.string();
  plan.common.derivation.system = kPackedSystem;
  plan.common.environment.manifest = manifest.string();
  plan.common.environment.system = kPackedSystem;
  plan.common.key = path_key(node.path);
  plan.common.cached = true;
  plan.execution_list = json::Bool(json::Get(entry, "executionList"));
  plan.implementation_labels =
      string_array(json::Get(entry, "implementationLabels"));
  plan.implementations = std::move(product.runs->value());
  plan.common.views = std::move(product.views->value());
  enforce_implementation_limit(plan.implementations.size(),
                               kMaxExecutionBatchSize);
  check_implementation_labels(plan.implementation_labels,
                              plan.implementations.size());
  if (json::Bool(json::Get(entry, "reuseWorker")) != node.settings.reuse_worker)
    throw invalid("worker reuse does not match Card " + path_key(node.path));
  std::set<std::string> closure;
  for (auto& implementation : plan.implementations) {
    const auto package = bundled_path(resources, implementation);
    if (!stdfs::is_regular_file(
            bundled_path(resources, implementation + "/lib/main.so")))
      throw invalid("missing lib/main.so in " + implementation);
    closure.insert(card_root(resources, implementation).string());
    implementation = package.string();
  }
  for (auto& view : plan.common.views) {
    const auto blob = bundled_path(resources, *view.blob);
    if (!stdfs::is_regular_file(blob))
      throw invalid("view blob is not a regular file " + *view.blob);
    *view.blob = blob.string();
  }
  plan.common.environment.paths.assign(closure.begin(), closure.end());
  return plan;
}

}  // namespace

auto PackedBuilds::read(const stdfs::path& manifest,
                        const Resolution& resolution) -> PackedBuilds {
  const auto path = stdfs::canonical(manifest);
  const auto source = fs::read_file(path);
  if (!source) throw invalid("cannot read " + path.string());
  const auto fields = json::Object(*source);
  if (json::UInt(json::Get(fields, "version")) != 1 ||
      json::Text(fields, "system") != kPackedSystem)
    throw invalid("unrecognized version or system");
  PackedBuilds result;
  result.resources = path.parent_path();
  const auto mode = json::Text(fields, "mode");
  if (mode != "default" && mode != "fast") throw invalid("unrecognized mode");
  result.fast = mode == "fast";
  result.debug = json::Bool(json::Get(fields, "debug"));
  for (auto& name : string_array(json::Get(fields, "hostPaths"))) {
    if (name.empty() || name.starts_with('.') ||
        name.find('/') != std::string::npos ||
        name.find('\0') != std::string::npos)
      throw invalid("invalid host path name");
    result.host_paths.insert(std::move(name));
  }
  for (const auto& raw : json::Array(json::Get(fields, "cards"))) {
    const auto entry = json::Object(raw);
    const auto card = json::Text(entry, "card");
    const auto found = resolution.index.find(card);
    if (found == resolution.index.end()) throw invalid("unknown Card " + card);
    const auto& node = resolution.nodes.at(found->second);
    if (node.kind != CardType::Code || node.is_virtual ||
        !result.cards
             .emplace(card, read_card(entry, path, result.resources, node))
             .second)
      throw invalid("unexpected Card " + card);
  }
  for (const auto& node : resolution.nodes)
    if (node.kind == CardType::Code && !node.is_virtual &&
        !result.cards.contains(path_key(node.path)))
      throw invalid("missing Code Card " + path_key(node.path));
  return result;
}

auto PackedBuilds::libraries() const -> std::vector<std::string> {
  std::vector<std::string> result;
  for (const auto& [_, plan] : cards)
    for (const auto& package : plan.implementations)
      result.push_back(package + "/lib/main.so");
  return result;
}

auto PackedBuilds::build(const BuildRequest& request) const -> CardBuildPlan {
  if (request.cancel.stop_requested())
    throw Failed(FailureKind::cancelled, "the invocation was cancelled");
  const auto card = path_key(request.node->path);
  const auto found = cards.find(card);
  if (found == cards.end())
    throw Failed(FailureKind::unsupported, "missing packed build for " + card);
  const auto& plan = found->second;
  try {
    enforce_implementation_limit(plan.implementations.size(),
                                 request.max_implementations);
  } catch (const NixImplementationLimit& error) {
    throw Failed(FailureKind::execution_limit, error.what(), {{"card", card}});
  }
  if (request.on_plan) request.on_plan(plan.implementations.size());
  return plan;
}

}  // namespace tsugumi
