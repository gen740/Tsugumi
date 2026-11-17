#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace tsugumi::host {
struct Failure : std::runtime_error {
  Failure(std::string c, std::string message, std::string request = {})
      : std::runtime_error(std::move(message)),
        code(std::move(c)),
        request_id(std::move(request)) {}
  std::string code;
  std::string request_id;
};
}  // namespace tsugumi::host
