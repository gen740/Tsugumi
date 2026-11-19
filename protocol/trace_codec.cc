#include "tsugumi/protocol/trace_codec.hh"

#include <flatbuffers/idl.h>
#include <flatbuffers/reflection.h>

#include <array>
#include <cstring>

#include "tsugumi/protocol/agent_generated.h"
#include "tsugumi/protocol/host_generated.h"
#include "tsugumi/protocol/local_worker_generated.h"
#include "tsugumi/protocol/remote_worker_generated.h"
#include "tsugumi/protocol/vm_host_generated.h"
#include "tsugumi/protocol/worker_generated.h"

#define SCHEMA(symbol)                 \
  extern const unsigned char symbol[]; \
  extern const unsigned long symbol##_size
SCHEMA(tsugumi_rpc_bfbs);
SCHEMA(tsugumi_host_bfbs);
SCHEMA(tsugumi_agent_bfbs);
SCHEMA(tsugumi_worker_bfbs);
SCHEMA(tsugumi_local_worker_bfbs);
SCHEMA(tsugumi_remote_worker_bfbs);
SCHEMA(tsugumi_vm_host_bfbs);
#undef SCHEMA

namespace tsugumi::trace {
namespace {
namespace A = Tsugumi::Agent;
namespace D = Tsugumi::Data;
namespace H = Tsugumi::Host;
namespace L = Tsugumi::LocalWorker;
namespace R = Tsugumi::RPC;
namespace V = Tsugumi::VMHost;
namespace W = Tsugumi::Worker;
namespace X = Tsugumi::RemoteWorker;

auto RequestId(uint64_t id) -> std::optional<uint64_t> {
  if (id) return id;
  return std::nullopt;
}

template <class Reply>
auto Failure(const Reply& reply) -> std::string_view {
  if (const auto* error = reply.value_as_Failure()) {
    if constexpr (requires { error->kind(); }) {
      return D::EnumNameFailureKind(error->kind());
    } else if constexpr (requires { error->code(); }) {
      return error->code()->string_view();
    } else {
      return "failure";
    }
  }
  return {};
}

template <class Message, class RequestName, class ReplyName>
auto Envelope(const Message* message, std::optional<uint64_t> id,
              RequestName request_name, ReplyName reply_name) -> Metadata {
  if (const auto* request = message->payload_as_Request()) {
    const std::string_view name = request_name(request->value_type());
    if (name != "HostTraceRequest" && name != "CoreTraceRequest")
      return {Direction::Request, name, id};
  }
  if (const auto* reply = message->payload_as_Reply()) {
    const std::string_view name = reply_name(reply->value_type());
    if (name != "HostTrace" && name != "TraceSnapshot")
      return {Direction::Reply, name, id, Failure(*reply)};
  }
  return {};
}

auto Describe(std::string_view bytes, bool outgoing) -> Metadata {
  if (bytes.size() < 8) return {};
  const auto* data = bytes.data();
  if (R::MessageBufferHasIdentifier(data)) {
    const auto* message = R::GetMessage(data);
    if (const auto* note = message->payload_as_Notification())
      return {Direction::Notification,
              R::EnumNameNotificationPayload(note->value_type())};
    return Envelope(message, message->request_id(), R::EnumNameRequestPayload,
                    R::EnumNameReplyPayload);
  }
  if (H::MessageBufferHasIdentifier(data)) {
    const auto* message = H::GetMessage(data);
    if (const auto* event = message->payload_as_Event())
      return {Direction::Event, H::EnumNameEventPayload(event->value_type())};
    return Envelope(message, RequestId(message->request_id()),
                    H::EnumNameRequestPayload, H::EnumNameReplyPayload);
  }
  if (A::MessageBufferHasIdentifier(data)) {
    const auto* message = A::GetMessage(data);
    return Envelope(message, RequestId(message->request_id()),
                    A::EnumNameRequestPayload, A::EnumNameReplyPayload);
  }
  if (V::MessageBufferHasIdentifier(data)) {
    const auto* message = V::GetMessage(data);
    if (const auto* event = message->payload_as_Event())
      return {Direction::Event, V::EnumNameEventPayload(event->value_type())};
    return Envelope(message, RequestId(message->request_id()),
                    V::EnumNameRequestPayload, V::EnumNameReplyPayload);
  }
  if (W::WorkerMessageBufferHasIdentifier(data)) {
    const auto* message = W::GetWorkerMessage(data);
    const auto tag = message->value_type();
    const auto direction =
        tag == W::WorkerPayload::WorkerCall      ? Direction::Request
        : tag == W::WorkerPayload::WorkerDisplay ? Direction::Event
        : tag == W::WorkerPayload::WorkerCancel  ? Direction::Notification
                                                 : Direction::Reply;
    const auto* error = message->value_as_WorkerFailure();
    return {direction, W::EnumNameWorkerPayload(tag), message->call_id(),
            error ? D::EnumNameFailureKind(error->failure()->kind()) : ""};
  }
  if (L::MessageBufferHasIdentifier(data)) {
    const auto* message = L::GetMessage(data);
    return {
        outgoing ? Direction::Request : Direction::Reply,
        L::EnumNameAction(message->action()),
        {},
        message->error() ? D::EnumNameFailureKind(message->error_kind()) : ""};
  }
  if (X::MessageBufferHasIdentifier(data)) {
    const auto* message = X::GetMessage(data);
    if (const auto* broker = message->payload_as_BrokerRequest())
      return {Direction::Request,
              X::EnumNameRequestBody(broker->request()->body_type())};
    if (const auto* request = message->payload_as_Request())
      return {Direction::Request, X::EnumNameRequestBody(request->body_type())};
    if (const auto* reply = message->payload_as_Reply()) {
      const auto* error = reply->body_as_Failure();
      return {Direction::Reply,
              X::EnumNameReplyBody(reply->body_type()),
              {},
              error ? X::EnumNameErrorCode(error->code()) : ""};
    }
  }
  return {};
}

struct Schema {
  const char* identifier;
  const unsigned char* bfbs;
  size_t size;
  bool (*verify)(flatbuffers::Verifier&);
};

const std::array kSchemas{
    Schema{"TSG1", tsugumi_rpc_bfbs, tsugumi_rpc_bfbs_size,
           R::VerifyMessageBuffer},
    Schema{"TSH1", tsugumi_host_bfbs, tsugumi_host_bfbs_size,
           H::VerifyMessageBuffer},
    Schema{"TSA1", tsugumi_agent_bfbs, tsugumi_agent_bfbs_size,
           A::VerifyMessageBuffer},
    Schema{"TSW1", tsugumi_worker_bfbs, tsugumi_worker_bfbs_size,
           W::VerifyWorkerMessageBuffer},
    Schema{"TSL1", tsugumi_local_worker_bfbs, tsugumi_local_worker_bfbs_size,
           L::VerifyMessageBuffer},
    Schema{"TSR1", tsugumi_remote_worker_bfbs, tsugumi_remote_worker_bfbs_size,
           X::VerifyMessageBuffer},
    Schema{"TSV1", tsugumi_vm_host_bfbs, tsugumi_vm_host_bfbs_size,
           V::VerifyMessageBuffer},
};

// Clear annotated strings/byte vectors in a transient buffer before passing
// it to FlatBuffers' text generator. Discovery uses the original verified
// graph, so an aliased field can't disappear from the traversal after a write.
struct SecretField {
  size_t offset;
  size_t size;
};

auto Secrets(const reflection::Schema& schema, const reflection::Object& object,
             const flatbuffers::Table& table, const uint8_t* base,
             std::vector<SecretField>& fields) -> bool {
  namespace F = flatbuffers;
  for (const auto* field : *object.fields()) {
    if (!table.CheckField(field->offset())) continue;
    const auto type = field->type()->base_type();
    const auto index = field->type()->index();
    if (field->attributes() &&
        field->attributes()->LookupByKey("trace_secret")) {
      if (type != reflection::String &&
          (type != reflection::Vector ||
           field->type()->element() != reflection::UByte))
        return false;
      const auto* value = table.GetPointer<const uint8_t*>(field->offset());
      fields.push_back({static_cast<size_t>(value - base),
                        sizeof(F::uoffset_t) +
                            F::ReadScalar<F::uoffset_t>(value) +
                            (type == reflection::String ? 1 : 0)});
    } else if (type == reflection::Obj &&
               !schema.objects()->Get(index)->is_struct()) {
      if (!Secrets(schema, *schema.objects()->Get(index),
                   *F::GetFieldT(table, *field), base, fields))
        return false;
    } else if (type == reflection::Union) {
      const auto* tag = object.fields()->LookupByKey(
          (field->name()->str() + F::UnionTypeFieldSuffix()).c_str());
      const auto* member = schema.enums()->Get(index)->values()->LookupByKey(
          F::GetFieldI<uint8_t>(table, *tag));
      if (!member || !member->union_type() ||
          member->union_type()->base_type() != reflection::Obj)
        return false;
      const auto* child = schema.objects()->Get(member->union_type()->index());
      if (!Secrets(schema, *child, *F::GetFieldT(table, *field), base, fields))
        return false;
    } else if (type == reflection::Vector &&
               field->type()->element() == reflection::Obj &&
               !schema.objects()->Get(index)->is_struct()) {
      const auto* list = F::GetFieldAnyV(table, *field);
      for (size_t i = 0; i < list->size(); ++i)
        if (!Secrets(schema, *schema.objects()->Get(index),
                     *F::GetAnyVectorElemPointer<const F::Table>(list, i), base,
                     fields))
          return false;
    } else if (type == reflection::Vector &&
               field->type()->element() == reflection::Union) {
      // Fail closed until the sanitizer supports vector unions.
      return false;
    }
  }
  return true;
}

auto Decode(std::string_view bytes) -> Content {
  namespace F = flatbuffers;
  for (const auto& spec : kSchemas) {
    if (!F::BufferHasIdentifier(bytes.data(), spec.identifier)) continue;
    const auto& schema = *reflection::GetSchema(spec.bfbs);
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    // The generated verifier handles sparse union tags in the project
    // protocol. Limit traversal before expanding the graph into JSON.
    auto verify = [&](const uint8_t* frame) {
      F::Verifier::Options limits;
      limits.max_depth = 32;
      limits.max_tables = 512;
      F::Verifier verifier(frame, bytes.size(), limits);
      return spec.verify(verifier);
    };
    if (!verify(data)) break;
    std::vector<SecretField> fields;
    if (!Secrets(schema, *schema.root_table(), *F::GetAnyRoot(data), data,
                 fields))
      return {.json = {}, .omission = "secret"};
    std::string sanitized;
    if (!fields.empty()) {
      sanitized.assign(bytes);
      for (const auto& field : fields) {
        // FlatBuffers stores a uint32 length before the bytes.
        std::memset(sanitized.data() + field.offset, 0, field.size);
      }
      data = reinterpret_cast<const uint8_t*>(sanitized.data());
      if (!verify(data)) return {.json = {}, .omission = "secret"};
    }
    F::IDLOptions options;
    options.strict_json = true;
    F::Parser parser(options);
    Content result;
    if (!parser.Deserialize(spec.bfbs, spec.size) ||
        F::GenerateText(parser, data, &result.json))
      break;
    if (!fields.empty()) result.omission = "redacted";
    return result;
  }
  return {.json = {}, .omission = "decode"};
}
}  // namespace

constinit const Codec kFlatBufferCodec{Describe, Decode};

auto ClientName(Tsugumi::Trace::Client client) -> const char* {
  using C = Tsugumi::Trace::Client;
  switch (client) {
    case C::MacOS:
      return "macOS app";
    case C::Web:
      return "Web client";
    case C::Host:
      return "hostd";
    default:
      return nullptr;
  }
}

auto ToWire(Snapshot snapshot) -> Tsugumi::Trace::SnapshotT {
  namespace T = Tsugumi::Trace;
  T::SnapshotT result;
  result.mode = static_cast<T::Mode>(snapshot.mode);
  for (auto& entry : snapshot.entries) {
    auto value = std::make_unique<T::EntryT>();
    value->sequence = entry.sequence;
    value->unix_micros = entry.unix_micros;
    value->from = entry.from;
    value->to = entry.to;
    value->direction = static_cast<T::Direction>(entry.direction);
    value->message_type = std::move(entry.type);
    value->request_id = entry.request_id;
    value->bytes = entry.bytes;
    value->elapsed_micros = entry.elapsed_micros;
    value->failure = std::move(entry.failure);
    value->content_json = std::move(entry.content.json);
    value->content_omission = entry.content.omission;
    result.entries.push_back(std::move(value));
  }
  return result;
}

}  // namespace tsugumi::trace
