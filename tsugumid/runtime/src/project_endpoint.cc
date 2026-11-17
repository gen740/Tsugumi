#include "server.hh"
#include "tsugumi/workerd/value_wire.hh"
namespace tsugumi {
auto ProjectEndpoint(ServerOptions& options) -> void {
  namespace R = Tsugumi::RPC;
  options.subprotocol = "tsugumi.rpc.v1";
  options.codec.inspect = [](std::string_view bytes, uint32_t limit) -> std::optional<FrameInfo> {
    flatbuffers::Verifier::Options vo; vo.max_size = limit;
    flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), vo);
    if (!R::VerifyMessageBuffer(v)) return std::nullopt;
    const auto* m = R::GetMessage(bytes.data());
    const auto* r = m->payload_as_Request();
    if (!r || !m->request_id() || !*m->request_id() || r->value_type() == R::RequestPayload::NONE ||
        r->value_type() > R::RequestPayload::MAX) return std::nullopt;
    return FrameInfo{*m->request_id(), r->value_type() == R::RequestPayload::SessionHelloRequest};
  };
  options.codec.completed = [](std::string_view bytes) { return R::GetMessage(bytes.data())->request_id().value_or(0); };
  options.codec.failure = [](uint64_t id, AdmissionFailure reason) {
    using K = Tsugumi::Data::FailureKind;
    const auto kind = reason == AdmissionFailure::AlreadyHello ? K::invalid_request :
                      reason == AdmissionFailure::Unauthenticated ? K::unauthenticated : K::busy;
    const auto* message = reason == AdmissionFailure::AlreadyHello ? "Hello was already sent on this connection" :
                          reason == AdmissionFailure::Unauthenticated ? "SessionHello must succeed first" :
                          "too many requests in flight on this connection";
    flatbuffers::FlatBufferBuilder b;
    auto f = wire::failure(b, FailureData::make(kind, message));
    auto reply = R::CreateReply(b, R::ReplyPayload::Failure, f.Union());
    R::FinishMessageBuffer(b, R::CreateMessage(b, id, R::Payload::Reply, reply.Union()));
    return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize());
  };
}
}  // namespace tsugumi
