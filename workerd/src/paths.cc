#include <algorithm>
#include <stdexcept>

#include "tsugumi/workerd/invocation.hh"

namespace tsugumi {
namespace stdfs = std::filesystem;
auto remote::relative_path(std::string_view path) -> stdfs::path {
  if (path.empty() || path.size() > 4096 || path.front() == '/' ||
      path.back() == '/' || path.find("//") != std::string_view::npos ||
      path.find('\\') != std::string_view::npos ||
      std::ranges::any_of(path,
                          [](unsigned char c) { return c < 32 || c == 127; }))
    throw std::runtime_error("invalid remote file path");
  stdfs::path result(path);
  for (const auto& part : result)
    if (part == "." || part == "..")
      throw std::runtime_error("invalid remote file path");
  return result;
}

}  // namespace tsugumi
