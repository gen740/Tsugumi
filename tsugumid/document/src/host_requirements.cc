#include "tsugumi/document/host_requirements.hh"

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi {
auto host_requirements(const Resolution& resolution, uint64_t rev)
    -> Tsugumi::RPC::ProjectHostRequirementsT {
  Tsugumi::RPC::ProjectHostRequirementsT result;
  result.rev = rev;
  for (const auto& node : resolution.nodes) {
    if (node.kind != CardType::Code) continue;
    for (const auto& folder : node.settings.grants.host_paths) {
      auto value = std::make_unique<Tsugumi::RPC::HostFolderRequirementT>();
      value->card = std::make_unique<Tsugumi::Data::CardPathT>();
      value->card->segments = node.path;
      value->name = folder.name;
      value->scope = node.project_dir;
      value->write = folder.write;
      result.folders.push_back(std::move(value));
    }
  }
  return result;
}
auto load_host_requirements(const std::filesystem::path& root)
    -> Tsugumi::RPC::ProjectHostRequirementsT {
  const auto doc = load_from_disk(root);
  const auto resolution =
      resolve(*doc, [&](const std::string& rel) -> std::optional<std::string> {
        const auto source = doc->sources.find(rel);
        if (source == doc->sources.end()) return std::nullopt;
        auto bytes = fs::read_file(root / rel);
        if (!bytes || crypto::sha256_hex(*bytes) != source->second.hash)
          return std::nullopt;
        return bytes;
      });
  return host_requirements(*resolution);
}
}  // namespace tsugumi
