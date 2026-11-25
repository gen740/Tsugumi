// Wire conversion and framing for transient process view traffic.
#pragma once

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/rpc/frame.hh"
#include "tsugumi/workerd/frame.hh"
#include "tsugumi/workerd/view.hh"

namespace tsugumi::workerd {
namespace W = Tsugumi::Worker;

inline auto view_uuid(const W::ViewUuid* id) -> ViewUuid {
  ViewUuid result{};
  if (id) std::ranges::copy(*id->bytes(), result.begin());
  return result;
}

inline auto view_uuid(const ViewUuid& id) -> W::ViewUuid {
  return W::ViewUuid(flatbuffers::span<const uint8_t, 16>(id));
}

inline auto view_bytes(const flatbuffers::Vector<uint8_t>* data)
    -> std::string {
  return data ? std::string(data->begin(), data->end()) : std::string();
}

inline auto view_event(const W::WorkerViewEvent& event) -> WorkerViewEvent {
  return {static_cast<ViewEventKind>(event.kind()), view_uuid(event.route()),
          view_uuid(event.session()), view_bytes(event.bytes())};
}

inline auto view_request(const W::WorkerViewRequest& request)
    -> WorkerViewRequest {
  return {request.request_id(),
          static_cast<ViewControlKind>(request.kind()),
          request.view() ? request.view()->str() : "",
          request.mode(),
          request.flags(),
          request.width(),
          request.height(),
          view_uuid(request.session()),
          view_uuid(request.route())};
}

inline auto view_reply(const W::WorkerViewReply& reply) -> WorkerViewReply {
  return {reply.request_id(), reply.status(), view_uuid(reply.session()),
          view_uuid(reply.route())};
}

inline auto valid_view_event(const WorkerViewEvent& event) -> bool {
  return event.route != ViewUuid{} && event.session != ViewUuid{} &&
         ((event.kind == ViewEventKind::message &&
           event.bytes.size() <= TSUGUMI_MESSAGE_MAX) ||
          (event.kind == ViewEventKind::close && event.bytes.empty()));
}

inline auto write_display_frame(int fd, const flatbuffers::FlatBufferBuilder& b)
    -> void {
  rpc::frame::Write(
      fd, {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()},
      kMaxWorkerFrameBytes);
}

inline auto finish_display_frame(flatbuffers::FlatBufferBuilder& b,
                                 W::WorkerPayload type,
                                 flatbuffers::Offset<void> payload) -> void {
  W::FinishWorkerMessageBuffer(b, W::CreateWorkerMessage(b, 0, type, payload));
}

inline auto display_bytes(flatbuffers::FlatBufferBuilder& b,
                          std::string_view data)
    -> flatbuffers::Offset<flatbuffers::Vector<uint8_t>> {
  return b.CreateVector(reinterpret_cast<const uint8_t*>(data.data()),
                        data.size());
}

inline auto build_display(flatbuffers::FlatBufferBuilder& b,
                          const ViewUuid& session, std::string_view bytes)
    -> void {
  const auto id = view_uuid(session);
  finish_display_frame(
      b, W::WorkerPayload::WorkerDisplay,
      W::CreateWorkerDisplay(b, &id, display_bytes(b, bytes)).Union());
}

inline auto build_display_credit(flatbuffers::FlatBufferBuilder& b,
                                 const ViewUuid& address, uint32_t bytes)
    -> void {
  const auto id = view_uuid(address);
  finish_display_frame(b, W::WorkerPayload::WorkerDisplayCredit,
                       W::CreateWorkerDisplayCredit(b, &id, bytes).Union());
}

inline auto build_view_event(flatbuffers::FlatBufferBuilder& b,
                             const WorkerViewEvent& event) -> void {
  const auto route = view_uuid(event.route);
  const auto session = view_uuid(event.session);
  finish_display_frame(
      b, W::WorkerPayload::WorkerViewEvent,
      W::CreateWorkerViewEvent(b, static_cast<W::ViewEventKind>(event.kind),
                               &route, &session, display_bytes(b, event.bytes))
          .Union());
}

inline auto build_view_request(flatbuffers::FlatBufferBuilder& b,
                               const WorkerViewRequest& request) -> void {
  const auto session = view_uuid(request.session);
  const auto route = view_uuid(request.route);
  const auto name = request.view.empty() ? 0 : b.CreateString(request.view);
  finish_display_frame(
      b, W::WorkerPayload::WorkerViewRequest,
      W::CreateWorkerViewRequest(
          b, request.request_id, static_cast<W::ViewControlKind>(request.kind),
          name, request.mode, request.width, request.height,
          request.session == ViewUuid{} ? nullptr : &session,
          request.route == ViewUuid{} ? nullptr : &route, request.flags)
          .Union());
}

inline auto build_view_reply(flatbuffers::FlatBufferBuilder& b,
                             const WorkerViewReply& reply) -> void {
  const auto session = view_uuid(reply.session);
  const auto route = view_uuid(reply.route);
  finish_display_frame(
      b, W::WorkerPayload::WorkerViewReply,
      W::CreateWorkerViewReply(b, reply.request_id, reply.status,
                               reply.session == ViewUuid{} ? nullptr : &session,
                               reply.route == ViewUuid{} ? nullptr : &route)
          .Union());
}

inline auto write_display_credit(int fd, const ViewUuid& address,
                                 uint32_t bytes) -> void {
  flatbuffers::FlatBufferBuilder b;
  build_display_credit(b, address, bytes);
  write_display_frame(fd, b);
}

inline auto read_display(int fd) -> std::optional<std::string> {
  auto frame = rpc::frame::Read(fd, kMaxWorkerFrameBytes);
  if (!frame) return std::nullopt;
  flatbuffers::Verifier verifier(
      reinterpret_cast<const uint8_t*>(frame->data()), frame->size());
  if (!W::VerifyWorkerMessageBuffer(verifier) ||
      W::GetWorkerMessage(frame->data())->call_id() != 0)
    throw std::runtime_error("invalid remote view relay frame");
  return frame;
}
}  // namespace tsugumi::workerd
