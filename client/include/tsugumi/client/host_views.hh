#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include "tsugumi/client/client.hh"

namespace tsugumi::client {

struct ViewPointerInput {
  uint32_t kind = 0;
  float x = 0, y = 0;
  uint32_t buttons = 0;
  float wheel_x = 0, wheel_y = 0;
  uint32_t modifiers = 0;
};

struct ViewKeyInput {
  uint32_t kind = 0, code = 0, modifiers = 0;
};

struct ViewTextInput {
  std::string text;
};

struct ViewGamepadInput {
  uint32_t pad = 0, kind = 0, buttons = 0;
  float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
};

using ViewInput = std::variant<ViewPointerInput, ViewKeyInput, ViewTextInput,
                               ViewGamepadInput>;

// Use an authenticated Host connection. The resulting frames contain
// ViewWindows or ViewActionResult, or a Host Failure.
auto ListViews(Connection& host, std::string_view project,
               std::chrono::milliseconds timeout = {}) -> std::future<Result>;
auto SendViewInput(Connection& host, std::string_view project,
                   std::string_view session, const ViewInput& input,
                   std::chrono::milliseconds timeout = {})
    -> std::future<Result>;
auto SnapshotView(Connection& host, std::string_view project,
                  std::string_view session,
                  std::chrono::milliseconds timeout = {})
    -> std::future<Result>;

}  // namespace tsugumi::client
