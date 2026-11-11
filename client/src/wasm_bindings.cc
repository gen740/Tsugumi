#include <emscripten/bind.h>

#include "tsugumi/client/client.hh"

namespace tsugumi::client {
namespace {
using emscripten::val;

auto ErrorValue(const Error& error) -> val {
  if (!error) return val::null();
  auto result = val::object();
  result.set("code", static_cast<int>(error.code));
  result.set("message", error.message);
  return result;
}
auto CopyBytes(const Bytes& bytes) -> val {
  // JavaScript owns the copy after the C++ callback returns.
  return val::global("Uint8Array")
      .new_(val(emscripten::typed_memory_view(bytes.size(), bytes.data())));
}

class BrowserClient {
 public:
  BrowserClient(std::string endpoint, bool agent)
      : connection_(MakeOptions(std::move(endpoint), agent)) {}
  auto Start(val callback) -> void {
    connection_.Start([callback](Error error) { callback(ErrorValue(error)); });
  }
  auto Call(val bytes, unsigned timeout_ms, val callback) -> void {
    connection_.CallFrame(
        emscripten::convertJSArrayToNumberVector<uint8_t>(bytes),
        [callback](Result result) {
          callback(ErrorValue(result.error), CopyBytes(result.bytes));
        },
        std::chrono::milliseconds(timeout_ms));
  }
  auto OnEvent(val callback) -> void {
    connection_.OnEvent(
        [callback](Bytes bytes) { callback(CopyBytes(bytes)); });
  }
  auto OnClosed(val callback) -> void {
    connection_.OnClosed(
        [callback](Error error) { callback(ErrorValue(error)); });
  }
  auto Close() -> void { connection_.Close(); }

 private:
  static auto MakeOptions(std::string endpoint, bool agent) -> Options {
    Options options;
    options.endpoint = std::move(endpoint);
    options.protocol = agent ? Protocol::Agent : Protocol::Project;
    return options;
  }
  Connection connection_;
};
}  // namespace

EMSCRIPTEN_BINDINGS(tsugumi_client) {
  emscripten::class_<BrowserClient>("RpcClient")
      .constructor<std::string, bool>()
      .function("start", &BrowserClient::Start)
      .function("call", &BrowserClient::Call)
      .function("onEvent", &BrowserClient::OnEvent)
      .function("onClosed", &BrowserClient::OnClosed)
      .function("close", &BrowserClient::Close);
}
}  // namespace tsugumi::client
