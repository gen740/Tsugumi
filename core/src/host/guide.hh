#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {
// Called under Host's mutex: a guide belongs to the host, never a connection.
class Guide {
public:
  Guide(std::filesystem::path directory, std::filesystem::path demos);
  ~Guide();
  static auto Handles(Tsugumi::Host::RequestPayload type) -> bool;
  auto Handle(const Tsugumi::Host::Request &request, uint64_t id)
      -> std::string;

private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace tsugumi::host
