// Typed Host and project calls on a client::Connection, answered on the main
// thread. The request is a generated object-API value; the reply is viewed
// in the bytes it was read from.
#pragma once

#include <flatbuffers/flatbuffers.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "main_thread.hh"
#include "tsugumi/client/client.hh"
#include "tsugumi/protocol/host_generated.h"
#include "tsugumi/protocol/tsugumi_generated.h"

namespace tsugumi::linux_ui {

template <class T>
struct Reply {
  client::Bytes bytes;
  const T* value = nullptr;  // null on failure
  std::string code;          // the service's failure code, if any
  std::string error;         // what went wrong, for people

  [[nodiscard]] auto ok() const -> bool { return value != nullptr; }
};

struct HostProtocol {
  using MessageT = Tsugumi::Host::MessageT;
  using RequestT = Tsugumi::Host::RequestT;

  static auto Finish(flatbuffers::FlatBufferBuilder& b, const MessageT& m)
      -> void {
    Tsugumi::Host::FinishMessageBuffer(b, Tsugumi::Host::Message::Pack(b, &m));
  }
  template <class T>
  static auto Read(Reply<T>& out) -> void {
    const auto* reply =
        Tsugumi::Host::GetMessage(out.bytes.data())->payload_as_Reply();
    if (!reply) {
      out.error = "invalid Host reply";
    } else if (const auto* failure = reply->value_as_Failure()) {
      out.code = failure->code()->str();
      out.error = failure->message()->str();
    } else if (!(out.value = reply->template value_as<T>())) {
      out.error = "unexpected Host reply";
    }
  }
};

struct ProjectProtocol {
  using MessageT = Tsugumi::RPC::MessageT;
  using RequestT = Tsugumi::RPC::RequestT;

  static auto Finish(flatbuffers::FlatBufferBuilder& b, const MessageT& m)
      -> void {
    Tsugumi::RPC::FinishMessageBuffer(b, Tsugumi::RPC::Message::Pack(b, &m));
  }
  template <class T>
  static auto Read(Reply<T>& out) -> void {
    const auto* reply =
        Tsugumi::RPC::GetMessage(out.bytes.data())->payload_as_Reply();
    if (!reply) {
      out.error = "invalid project reply";
    } else if (const auto* failure = reply->value_as_Failure()) {
      out.code = Tsugumi::Data::EnumNameFailureKind(failure->kind());
      out.error =
          failure->diagnostic() ? failure->diagnostic()->str() : out.code;
    } else if (!(out.value = reply->template value_as<T>())) {
      out.error = "unexpected project reply";
    }
  }
};

// Sends value and calls done on the main thread with the reply.
template <class Protocol, class T, class Value>
auto Call(client::Connection& connection, Value value,
          std::function<void(Reply<T>)> done) -> void {
  // client::Connection::BuildRequest is copyable; the value is not.
  auto shared = std::make_shared<Value>(std::move(value));
  connection.Call(
      [shared](uint64_t id) {
        typename Protocol::RequestT request;
        request.value.Set(std::move(*shared));
        typename Protocol::MessageT message;
        message.request_id = id;
        message.payload.Set(std::move(request));
        flatbuffers::FlatBufferBuilder b;
        Protocol::Finish(b, message);
        return client::Bytes(b.GetBufferPointer(),
                             b.GetBufferPointer() + b.GetSize());
      },
      [done = std::move(done)](client::Result result) {
        Post([done, result = std::move(result)]() mutable {
          Reply<T> reply;
          reply.bytes = std::move(result.bytes);
          if (result.error)
            reply.error = result.error.message;
          else
            Protocol::template Read<T>(reply);
          done(std::move(reply));
        });
      });
}

}  // namespace tsugumi::linux_ui
