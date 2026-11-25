#pragma once

#include <stdexcept>
#include <string>
#include <utility>

#include "host/process_lock.hh"

namespace tsugumi::host {
struct Failure : std::runtime_error {
  Failure(std::string c, std::string message, std::string request = {})
      : std::runtime_error(std::move(message)),
        code(std::move(c)),
        request_id(std::move(request)) {}
  std::string code;
  std::string request_id;
};

struct ConversationLockFailure : Failure {
  explicit ConversationLockFailure(std::optional<LockHolder> owner)
      : Failure("busy", owner ? owner->Description() +
                                    " has this project's conversations open. "
                                    "Quit it to free them, then reload."
                              : "Another Agent host has this project's "
                                "conversations open. Quit the host to free "
                                "them, then reload."),
        holder(std::move(owner)) {}
  std::optional<LockHolder> holder;
};
}  // namespace tsugumi::host
