#pragma once

#include <string>
#include <string_view>

#include "tsugumi/protocol/agent_generated.h"
#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::agent::wire {

// Storage/model JSON is decoded here into typed service replies. JSON never
// serves as the service envelope or as an untyped operation result.
auto ProviderJson(const Tsugumi::Service::ProviderInput& provider)
    -> std::string;
auto SelectionJson(const Tsugumi::Service::ProviderSelection& provider)
    -> std::string;
auto Conversation(std::string_view stored) -> Tsugumi::Agent::ConversationT;
auto Event(std::string_view stored) -> Tsugumi::Agent::EventT;
auto Usage(std::string_view stored) -> Tsugumi::Agent::UsageT;

template <class T> auto AgentReply(uint64_t id, T value) -> std::string {
  Tsugumi::Agent::ReplyT reply;
  reply.value.Set(std::move(value));
  Tsugumi::Agent::MessageT message;
  message.request_id = id;
  message.payload.Set(std::move(reply));
  flatbuffers::FlatBufferBuilder b;
  Tsugumi::Agent::FinishMessageBuffer(
      b, Tsugumi::Agent::Message::Pack(b, &message));
  return {reinterpret_cast<const char *>(b.GetBufferPointer()), b.GetSize()};
}

template <class T> auto HostReply(uint64_t id, T value) -> std::string {
  Tsugumi::Host::ReplyT reply;
  reply.value.Set(std::move(value));
  Tsugumi::Host::MessageT message;
  message.request_id = id;
  message.payload.Set(std::move(reply));
  flatbuffers::FlatBufferBuilder b;
  Tsugumi::Host::FinishMessageBuffer(b,
                                     Tsugumi::Host::Message::Pack(b, &message));
  return {reinterpret_cast<const char *>(b.GetBufferPointer()), b.GetSize()};
}
template <class T> auto HostEvent(T value) -> std::string {
  Tsugumi::Host::EventT event;
  event.value.Set(std::move(value));
  Tsugumi::Host::MessageT message;
  message.payload.Set(std::move(event));
  flatbuffers::FlatBufferBuilder b;
  Tsugumi::Host::FinishMessageBuffer(b, Tsugumi::Host::Message::Pack(b, &message));
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}
} // namespace tsugumi::agent::wire
