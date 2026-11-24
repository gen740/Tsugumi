// Framing for the remote listener's transient socket to its job adapter.
#pragma once

#include <flatbuffers/flatbuffers.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/rpc/frame.hh"
#include "tsugumi/workerd/frame.hh"

namespace tsugumi::workerd {
namespace W = Tsugumi::Worker;

inline auto write_display(int fd, std::string_view view, std::string_view bytes)
    -> void {
  flatbuffers::FlatBufferBuilder b;
  const auto display = W::CreateWorkerDisplay(
      b, b.CreateString(view),
      b.CreateVector(reinterpret_cast<const uint8_t*>(bytes.data()),
                     bytes.size()));
  W::FinishWorkerMessageBuffer(
      b, W::CreateWorkerMessage(b, 0, W::WorkerPayload::WorkerDisplay,
                                display.Union()));
  rpc::frame::Write(
      fd, {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()},
      kMaxWorkerFrameBytes);
}

inline auto write_display_credit(int fd, std::string_view view, uint32_t bytes)
    -> void {
  flatbuffers::FlatBufferBuilder b;
  const auto credit =
      W::CreateWorkerDisplayCredit(b, b.CreateString(view), bytes);
  W::FinishWorkerMessageBuffer(
      b, W::CreateWorkerMessage(b, 0, W::WorkerPayload::WorkerDisplayCredit,
                                credit.Union()));
  rpc::frame::Write(
      fd, {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()},
      kMaxWorkerFrameBytes);
}

inline auto read_display(int fd) -> std::optional<std::string> {
  auto frame = rpc::frame::Read(fd, kMaxWorkerFrameBytes);
  if (!frame) return std::nullopt;
  flatbuffers::Verifier verifier(
      reinterpret_cast<const uint8_t*>(frame->data()), frame->size());
  if (!W::VerifyWorkerMessageBuffer(verifier))
    throw std::runtime_error("invalid remote display message");
  return frame;
}
}  // namespace tsugumi::workerd
