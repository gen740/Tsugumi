#include "tsugumi/workerd/scratch_policy.hh"

#include <unistd.h>

#include <cstdlib>
namespace tsugumi {
auto run_scratch_root() -> std::filesystem::path {
  if (const auto* root = std::getenv("TSUGUMI_RUN_SCRATCH_DIR"); root && *root)
    return root;
#ifdef __APPLE__
  const std::filesystem::path temporary = "/private/var/tmp";
#else
  const std::filesystem::path temporary = "/var/tmp";
#endif
  return temporary / ("tsugumi-scratch-" + std::to_string(::geteuid()));
}

}  // namespace tsugumi
