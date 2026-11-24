#include "tsugumi/client/host_views.hh"

#include <type_traits>
#include <utility>

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::client {
namespace {
namespace H = Tsugumi::Host;

auto Frame(flatbuffers::FlatBufferBuilder& builder, uint64_t id,
           H::RequestPayload type, flatbuffers::Offset<void> value) -> Bytes {
  const auto request = H::CreateRequest(builder, type, value);
  H::FinishMessageBuffer(
      builder,
      H::CreateMessage(builder, id, H::Payload::Request, request.Union()));
  return {builder.GetBufferPointer(),
          builder.GetBufferPointer() + builder.GetSize()};
}

auto ActionFrame(flatbuffers::FlatBufferBuilder& builder, uint64_t id,
                 std::string_view project, std::string_view session,
                 H::ViewAction type, flatbuffers::Offset<void> value) -> Bytes {
  const auto request =
      H::CreateViewActionRequest(builder, builder.CreateString(project),
                                 builder.CreateString(session), type, value);
  return Frame(builder, id, H::RequestPayload::ViewActionRequest,
               request.Union());
}
}  // namespace

auto ListViews(Connection& host, std::string_view project,
               std::chrono::milliseconds timeout) -> std::future<Result> {
  return host.Call(
      [project](uint64_t id) {
        flatbuffers::FlatBufferBuilder builder;
        const auto request =
            H::CreateViewWindowsRequest(builder, builder.CreateString(project));
        return Frame(builder, id, H::RequestPayload::ViewWindowsRequest,
                     request.Union());
      },
      timeout);
}

auto SendViewInput(Connection& host, std::string_view project,
                   std::string_view session, const ViewInput& input,
                   std::chrono::milliseconds timeout) -> std::future<Result> {
  return host.Call(
      [project, session, &input](uint64_t id) {
        flatbuffers::FlatBufferBuilder builder;
        const auto [type, value] = std::visit(
            [&](const auto& event) {
              using T = std::decay_t<decltype(event)>;
              if constexpr (std::is_same_v<T, ViewPointerInput>) {
                return std::pair{
                    H::ViewAction::ViewPointer,
                    H::CreateViewPointer(builder, event.kind, event.x, event.y,
                                         event.buttons, event.wheel_x,
                                         event.wheel_y, event.modifiers)
                        .Union()};
              } else if constexpr (std::is_same_v<T, ViewKeyInput>) {
                return std::pair{H::ViewAction::ViewKey,
                                 H::CreateViewKey(builder, event.kind,
                                                  event.code, event.modifiers)
                                     .Union()};
              } else if constexpr (std::is_same_v<T, ViewTextInput>) {
                return std::pair{
                    H::ViewAction::ViewText,
                    H::CreateViewText(builder, builder.CreateString(event.text))
                        .Union()};
              } else {
                return std::pair{
                    H::ViewAction::ViewGamepad,
                    H::CreateViewGamepad(builder, event.pad, event.kind,
                                         event.buttons, event.lx, event.ly,
                                         event.rx, event.ry, event.lt, event.rt)
                        .Union()};
              }
            },
            input);
        return ActionFrame(builder, id, project, session, type, value);
      },
      timeout);
}

auto SnapshotView(Connection& host, std::string_view project,
                  std::string_view session, std::chrono::milliseconds timeout)
    -> std::future<Result> {
  return host.Call(
      [project, session](uint64_t id) {
        flatbuffers::FlatBufferBuilder builder;
        const auto snapshot = H::CreateViewSnapshot(builder);
        return ActionFrame(builder, id, project, session,
                           H::ViewAction::ViewSnapshot, snapshot.Union());
      },
      timeout);
}

}  // namespace tsugumi::client
