// Typed Host and Agent calls on a client::Connection.
#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include "tsugumi/client/client.hh"
#include "tsugumi/protocol/agent_generated.h"
#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::cli {

/** A Failure reply: the service's code and message. */
struct RpcFailure : std::runtime_error {
  RpcFailure(std::string c, const std::string& message, std::string request)
      : std::runtime_error(message), code(std::move(c)), request_id(std::move(request)) {}
  std::string code;
  std::string request_id;
};

/** A reply value, viewed in the bytes it was read from. */
template <class T>
struct Reply {
  client::Bytes bytes;
  const T* view = nullptr;
};

namespace detail {
template <class Message, class Request, class Value, class Build, class Get>
auto Call(client::Connection& connection, Value value, Build finish, Get get,
          const std::function<void(uint64_t)>& assigned) -> client::Bytes {
  auto result = connection
                    .Call([&](uint64_t id) {
                      if (assigned) assigned(id);
                      Request request;
                      request.value.Set(std::move(value));
                      Message message;
                      message.request_id = id;
                      message.payload.Set(std::move(request));
                      flatbuffers::FlatBufferBuilder b;
                      finish(b, message);
                      return client::Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
                    })
                    .get();
  if (result.error) throw std::runtime_error("Tsugumi host connection: " + result.error.message);
  get(result.bytes);
  return std::move(result.bytes);
}
}  // namespace detail

template <class T, class Value>
auto HostCall(client::Connection& connection, Value value) -> Reply<T> {
  const std::function<void(uint64_t)> none;
  namespace H = Tsugumi::Host;
  Reply<T> out;
  out.bytes = detail::Call<H::MessageT, H::RequestT>(
      connection, std::move(value),
      [](auto& b, const H::MessageT& m) { H::FinishMessageBuffer(b, H::Message::Pack(b, &m)); },
      [](const client::Bytes& bytes) {
        flatbuffers::Verifier verifier(bytes.data(), bytes.size());
        if (!H::VerifyMessageBuffer(verifier)) throw std::runtime_error("invalid Host reply");
      },
      none);
  const auto* reply = H::GetMessage(out.bytes.data())->payload_as_Reply();
  if (!reply) throw std::runtime_error("invalid Host reply");
  if (const auto* failure = reply->value_as_Failure())
    throw RpcFailure(failure->code()->str(), failure->message()->str(),
                     failure->request_id() ? failure->request_id()->str() : "");
  out.view = reply->template value_as<T>();
  if (!out.view) throw std::runtime_error("unexpected Host reply");
  return out;
}

/** assigned receives the request's ID (ToolCallCancelRequest names it). */
template <class T, class Value>
auto AgentCall(client::Connection& connection, Value value,
               const std::function<void(uint64_t)>& assigned = {}) -> Reply<T> {
  namespace A = Tsugumi::Agent;
  Reply<T> out;
  out.bytes = detail::Call<A::MessageT, A::RequestT>(
      connection, std::move(value),
      [](auto& b, const A::MessageT& m) { A::FinishMessageBuffer(b, A::Message::Pack(b, &m)); },
      [](const client::Bytes& bytes) {
        flatbuffers::Verifier verifier(bytes.data(), bytes.size());
        if (!A::VerifyMessageBuffer(verifier)) throw std::runtime_error("invalid Agent reply");
      },
      assigned);
  const auto* reply = A::GetMessage(out.bytes.data())->payload_as_Reply();
  if (!reply) throw std::runtime_error("invalid Agent reply");
  if (const auto* failure = reply->value_as_Failure())
    throw RpcFailure(failure->code()->str(), failure->message()->str(),
                     failure->request_id() ? failure->request_id()->str() : "");
  out.view = reply->template value_as<T>();
  if (!out.view) throw std::runtime_error("unexpected Agent reply");
  return out;
}

}  // namespace tsugumi::cli
