// Transient process view relay types.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace tsugumi {

using ViewUuid = std::array<uint8_t, 16>;
enum class ViewEventKind { message, close };
enum class ViewControlKind { open, register_route, unregister_route };

struct WorkerDisplayData {
  ViewUuid session;
  std::string bytes;
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
  uint32_t mode = 0, width = 0, height = 0;
  ViewUuid session = {}, route = {};
};

struct WorkerViewReply {
  uint64_t request_id;
  bool ok;
  ViewUuid session = {}, route = {};
};

}  // namespace tsugumi
