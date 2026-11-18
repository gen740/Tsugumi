#include "host/wire_endpoint.hh"

#include "tsugumi/protocol/agent_generated.h"
#include "tsugumi/protocol/host_generated.h"
namespace tsugumi {
auto AgentEndpoint(ServerOptions& options) -> void {
  namespace R = Tsugumi::Agent;
  options.subprotocol = "tsugumi.agent.v1";
  options.codec.inspect = [](std::string_view bytes,
                             uint32_t) -> std::optional<FrameInfo> {
    flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(bytes.data()),
                            bytes.size());
    if (!R::VerifyMessageBuffer(v)) return std::nullopt;
    const auto* m = R::GetMessage(bytes.data());
    const auto* r = m->payload_as_Request();
    if (!r || !r->value() || !m->request_id() ||
        r->value_type() == R::RequestPayload::NONE ||
        r->value_type() > R::RequestPayload::MAX)
      return std::nullopt;
    return FrameInfo{m->request_id(),
                     r->value_type() == R::RequestPayload::HelloRequest};
  };
  options.codec.completed = [](std::string_view bytes) {
    return R::GetMessage(bytes.data())->request_id();
  };
  options.codec.failure = [](uint64_t id, AdmissionFailure reason) {
    const auto* kind =
        reason == AdmissionFailure::AlreadyHello      ? "invalid_request"
        : reason == AdmissionFailure::Unauthenticated ? "unauthenticated"
                                                      : "busy";
    const auto* message = reason == AdmissionFailure::AlreadyHello
                              ? "Hello was already sent"
                          : reason == AdmissionFailure::Unauthenticated
                              ? "Hello must succeed first"
                              : "too many requests in flight";
    flatbuffers::FlatBufferBuilder b;
    auto f = Tsugumi::Service::CreateFailure(b, b.CreateString(kind),
                                             b.CreateString(message));
    auto reply = R::CreateReply(b, R::ReplyPayload::Failure, f.Union());
    R::FinishMessageBuffer(
        b, R::CreateMessage(b, id, R::Payload::Reply, reply.Union()));
    return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()),
                       b.GetSize());
  };
}
auto HostEndpoint(ServerOptions& options) -> void {
  namespace R = Tsugumi::Host;
  options.subprotocol = "tsugumi.host.v1";
  options.codec.inspect = [](std::string_view bytes,
                             uint32_t) -> std::optional<FrameInfo> {
    flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(bytes.data()),
                            bytes.size());
    if (!R::VerifyMessageBuffer(v)) return std::nullopt;
    const auto* m = R::GetMessage(bytes.data());
    const auto* r = m->payload_as_Request();
    if (!r || !r->value() || !m->request_id() ||
        r->value_type() == R::RequestPayload::NONE ||
        r->value_type() > R::RequestPayload::MAX)
      return std::nullopt;
    return FrameInfo{m->request_id(),
                     r->value_type() == R::RequestPayload::HelloRequest};
  };
  options.codec.completed = [](std::string_view bytes) {
    return R::GetMessage(bytes.data())->request_id();
  };
  options.codec.failure = [](uint64_t id, AdmissionFailure reason) {
    const auto* kind =
        reason == AdmissionFailure::AlreadyHello      ? "invalid_request"
        : reason == AdmissionFailure::Unauthenticated ? "unauthenticated"
                                                      : "busy";
    const auto* message = reason == AdmissionFailure::AlreadyHello
                              ? "Hello was already sent"
                          : reason == AdmissionFailure::Unauthenticated
                              ? "Hello must succeed first"
                              : "too many requests in flight";
    flatbuffers::FlatBufferBuilder b;
    auto f = Tsugumi::Service::CreateFailure(b, b.CreateString(kind),
                                             b.CreateString(message));
    auto reply = R::CreateReply(b, R::ReplyPayload::Failure, f.Union());
    R::FinishMessageBuffer(
        b, R::CreateMessage(b, id, R::Payload::Reply, reply.Union()));
    return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()),
                       b.GetSize());
  };
}
}  // namespace tsugumi
