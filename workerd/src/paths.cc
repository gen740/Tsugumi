#include <algorithm>
#include <cctype>
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

auto remote::store_path(std::string_view path) -> bool {
  constexpr std::string_view prefix = "/nix/store/";
  if (!path.starts_with(prefix)) return false;
  path.remove_prefix(prefix.size());
  return path.size() > 33 && path.size() < 256 && path[32] == '-' &&
         std::ranges::all_of(
             path.substr(0, 32),
             [](char c) {
               return std::string_view("0123456789abcdfghijklmnpqrsvwxyz")
                          .find(c) != std::string_view::npos;
             }) &&
         std::ranges::all_of(
             path.substr(33),
             [](unsigned char c) {
               return std::isalnum(c) != 0 || c == '_' || c == '-' ||
                      c == '+' || c == '.' || c == '=' || c == '?';
             });
}

}  // namespace tsugumi
