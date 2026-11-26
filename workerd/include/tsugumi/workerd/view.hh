// Transient process view relay types.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace tsugumi {

using ViewUuid = std::array<uint8_t, 16>;
enum class ViewEventKind { message, close };
enum class ViewControlKind { open, register_route, unregister_route, close };

struct WorkerDisplayData {
  ViewUuid session;
  std::string bytes;
  uint64_t view_request_id = 0;
  // Relay-local worker identity. This is never carried on the wire.
  std::weak_ptr<const void> source = {};
};

struct WorkerViewEvent {
  ViewEventKind kind;
  ViewUuid route;
  ViewUuid session;
  std::string bytes;
};

struct WorkerViewRequest {
  uint64_t request_id;
  ViewControlKind kind;
  std::string view;
  uint32_t mode = 0, flags = 0, width = 0, height = 0;
  ViewUuid session = {}, route = {};
};

struct WorkerViewReply {
  uint64_t request_id;
  int status = -1;
  ViewUuid session = {}, route = {};
};

}  // namespace tsugumi
