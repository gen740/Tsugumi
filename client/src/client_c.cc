#include <exception>
#include <memory>

#include "tsugumi/client/client.h"
#include "tsugumi/client/client.hh"

namespace C = tsugumi::client;

struct tsugumi_client {
  std::unique_ptr<C::Connection> connection;
};

namespace {
auto Complete(tsugumi_client_callback callback, void* context, C::Result result)
    -> void {
  if (callback)
    callback(context, result.bytes.data(), result.bytes.size(),
             static_cast<int>(result.error.code), result.error.message.c_str());
}
auto Failed(tsugumi_client_callback callback, void* context,
            const char* message) -> void {
  Complete(callback, context, {{}, {C::ErrorCode::InvalidArgument, message}});
}
}  // namespace

extern "C" {
auto tsugumi_client_create(const char* endpoint, int protocol)
    -> tsugumi_client* {
  try {
    if (!endpoint || (protocol != 0 && protocol != 1)) return nullptr;
    C::Options options;
    options.endpoint = endpoint;
    options.protocol =
        protocol == 1 ? C::Protocol::Agent : C::Protocol::Project;
    return new tsugumi_client{
        std::make_unique<C::Connection>(std::move(options))};
  } catch (...) {
    return nullptr;
  }
}
auto tsugumi_client_start(tsugumi_client* client,
                          tsugumi_client_callback callback, void* context)
    -> void {
  try {
    if (!client) {
      Failed(callback, context, "client is null");
      return;
    }
    client->connection->Start([callback, context](C::Error error) {
      Complete(callback, context, {{}, std::move(error)});
    });
  } catch (const std::exception& error) {
    Failed(callback, context, error.what());
  } catch (...) {
    Failed(callback, context, "client start failed");
  }
}
auto tsugumi_client_on_event(tsugumi_client* client,
                             tsugumi_client_callback callback, void* context)
    -> void {
  try {
    if (client)
      client->connection->OnEvent([callback, context](C::Bytes bytes) {
        Complete(callback, context, {std::move(bytes), {}});
      });
  } catch (...) {
    Failed(callback, context, "event callback registration failed");
  }
}
auto tsugumi_client_on_closed(tsugumi_client* client,
                              tsugumi_client_callback callback, void* context)
    -> void {
  try {
    if (client)
      client->connection->OnClosed([callback, context](C::Error error) {
        Complete(callback, context, {{}, std::move(error)});
      });
  } catch (...) {
    Failed(callback, context, "close callback registration failed");
  }
}
auto tsugumi_client_call(tsugumi_client* client, const uint8_t* bytes,
                         size_t size, uint32_t timeout_ms,
                         tsugumi_client_callback callback, void* context)
    -> void {
  try {
    if (!client || !bytes || !size) {
      Failed(callback, context, "client and request are required");
      return;
    }
    client->connection->CallFrame(
        C::Bytes(bytes, bytes + size),
        [callback, context](C::Result result) {
          Complete(callback, context, std::move(result));
        },
        std::chrono::milliseconds(timeout_ms));
  } catch (const std::exception& error) {
    Failed(callback, context, error.what());
  } catch (...) {
    Failed(callback, context, "client call failed");
  }
}
auto tsugumi_client_close(tsugumi_client* client) -> void {
  try {
    if (client) client->connection->Close();
  } catch (...) {
  }
}
auto tsugumi_client_destroy(tsugumi_client* client) -> void {
  try {
    delete client;
  } catch (...) {
  }
}
}  // extern "C"
