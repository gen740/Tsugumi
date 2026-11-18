#pragma once

#include <utility>

#include "tsugumi/workerd/module_source.hh"
namespace tsugumi {

class ModuleIdentity {
 public:
  static auto parse(std::string_view path) -> std::optional<ModuleIdentity>;
  auto path() const -> const std::filesystem::path& { return path_; }

 private:
  explicit ModuleIdentity(std::filesystem::path path)
      : path_(std::move(path)) {}
  std::filesystem::path path_;
};

auto cached_module_source(const std::filesystem::path& cache,
                          const ModuleIdentity& identity)
    -> std::optional<std::string>;
auto save_module_source(const std::filesystem::path& cache,
                        const ModuleIdentity& identity, std::string_view source)
    -> void;
}  // namespace tsugumi
