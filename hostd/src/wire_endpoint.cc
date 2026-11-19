#include "host/wire_endpoint.hh"

#include "tsugumi/protocol/agent_generated.h"
#include "tsugumi/protocol/host_generated.h"
namespace tsugumi {
namespace {
template <class Read, class Verify, class Wrap>
auto Endpoint(ServerOptions& options, const char* subprotocol, Read read,
              Verify verify, Wrap wrap) -> void {
  options.subprotocol = subprotocol;
  options.codec.inspect = [read, verify](std::string_view bytes,
                                         uint32_t) -> std::optional<FrameInfo> {
    flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(bytes.data()),
                            bytes.size());
    if (!verify(v)) return std::nullopt;
    const auto* m = read(bytes.data());
    const auto* r = m->payload_as_Request();
    using Tag = decltype(r->value_type());
    if (!r || !r->value() || !m->request_id() || r->value_type() == Tag::NONE ||
        r->value_type() > Tag::MAX)
      return std::nullopt;
    return FrameInfo{m->request_id(), r->value_type() == Tag::HelloRequest};
  };
  options.codec.completed = [read](std::string_view bytes) {
    return read(bytes.data())->request_id();
  };
  options.codec.failure = [wrap](uint64_t id, AdmissionFailure reason) {
    const auto* kind = reason == AdmissionFailure::AlreadyHello
                           ? "invalid_request"
                           : "unauthenticated";
    const auto* message = reason == AdmissionFailure::AlreadyHello
                              ? "Hello was already sent"
                              : "Hello must succeed first";
    flatbuffers::FlatBufferBuilder b;
    auto f = Tsugumi::Service::CreateFailure(b, b.CreateString(kind),
                                             b.CreateString(message));
    wrap(b, id, f);
    return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()),
                       b.GetSize());
  };
}
}  // namespace

auto AgentEndpoint(ServerOptions& options) -> void {
  namespace R = Tsugumi::Agent;
  Endpoint(
      options, "tsugumi.agent.v1", R::GetMessage,
      [](flatbuffers::Verifier& v) { return R::VerifyMessageBuffer(v); },
      [](auto& b, auto id, auto f) {
        auto reply = R::CreateReply(b, R::ReplyPayload::Failure, f.Union());
        R::FinishMessageBuffer(
            b, R::CreateMessage(b, id, R::Payload::Reply, reply.Union()));
      });
}
auto HostEndpoint(ServerOptions& options) -> void {
  namespace R = Tsugumi::Host;
  Endpoint(
      options, "tsugumi.host.v1", R::GetMessage,
      [](flatbuffers::Verifier& v) { return R::VerifyMessageBuffer(v); },
      [](auto& b, auto id, auto f) {
        auto reply = R::CreateReply(b, R::ReplyPayload::Failure, f.Union());
        R::FinishMessageBuffer(
            b, R::CreateMessage(b, id, R::Payload::Reply, reply.Union()));
      });
}
}  // namespace tsugumi
