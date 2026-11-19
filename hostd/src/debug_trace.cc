#include "host/debug_trace.hh"

#include "host/failure.hh"
#include "tsugumi/protocol/tsugumi_generated.h"

namespace tsugumi::host {
namespace {
namespace R = Tsugumi::RPC;
namespace T = Tsugumi::Trace;
using namespace std::chrono_literals;

template <class Build>
auto Call(client::Connection& connection, R::RequestPayload type, Build build)
    -> client::Bytes {
  auto response =
      connection
          .Call(
              [&](uint64_t id) {
                flatbuffers::FlatBufferBuilder builder;
                const auto value = build(builder);
                const auto request =
                    R::CreateRequest(builder, type, value.Union());
                R::FinishMessageBuffer(
                    builder, R::CreateMessage(builder, id, R::Payload::Request,
                                              request.Union()));
                return client::Bytes(
                    builder.GetBufferPointer(),
                    builder.GetBufferPointer() + builder.GetSize());
              },
              2s)
          .get();
  if (response.error)
    throw Failure("unavailable", "Project trace connection lost");
  return std::move(response.bytes);
}
}  // namespace

auto DebugTrace::Query(Peer& peer, const std::string& project,
                       std::optional<trace::Mode> mode)
    -> std::unique_ptr<T::SnapshotT> {
  try {
    if (!peer.connection) {
      client::Options options;
      options.endpoint = peer.target.endpoint;
      peer.connection = std::make_unique<client::Connection>(options);
      auto started = peer.connection->Start();
      if (started.wait_for(2s) != std::future_status::ready || started.get())
        throw Failure("unavailable", "Cannot connect to project trace");
      auto hello =
          Call(*peer.connection, R::RequestPayload::SessionHelloRequest,
               [&](auto& builder) {
                 return R::CreateSessionHelloRequest(
                     builder, builder.CreateString(project),
                     peer.target.token.empty()
                         ? 0
                         : builder.CreateString(peer.target.token),
                     T::Client::Host);
               });
      const auto* reply = R::GetMessage(hello.data())->payload_as_Reply();
      if (!reply || !reply->value_as_HelloResult())
        throw Failure("unavailable", "Project trace authentication failed");
    }
    auto response = Call(
        *peer.connection, R::RequestPayload::CoreTraceRequest,
        [&](auto& builder) {
          return T::CreateCoreTraceRequest(
              builder,
              peer.target.capability.empty()
                  ? 0
                  : builder.CreateString(peer.target.capability),
              mode ? flatbuffers::Optional<uint8_t>(static_cast<uint8_t>(*mode))
                   : flatbuffers::nullopt);
        });
    const auto* reply = R::GetMessage(response.data())->payload_as_Reply();
    const auto* snapshot = reply ? reply->value_as_TraceSnapshot() : nullptr;
    if (!snapshot)
      throw Failure("unavailable", "Project trace request was rejected");
    peer.mode = static_cast<trace::Mode>(snapshot->mode());
    return std::unique_ptr<T::SnapshotT>(snapshot->UnPack());
  } catch (const Failure&) {
    peer.connection.reset();
    return nullptr;
  }
}

auto DebugTrace::Reconcile(Targets targets) -> void {
  for (auto it = peers_.begin(); it != peers_.end();) {
    if (auto target = targets.find(it->first);
        target == targets.end() || target->second != it->second.target) {
      it = peers_.erase(it);
    } else {
      targets.erase(target);
      ++it;
    }
  }
  const auto mode = trace::CurrentMode();
  if (mode != trace::Mode::Off) {
    for (auto& [id, target] : targets)
      peers_.emplace(id, Peer{.target = std::move(target), .connection = {}});
  }
  for (auto& [id, peer] : peers_)
    if (peer.connection ? peer.mode != mode : mode != trace::Mode::Off)
      Query(peer, id, mode);
}

auto DebugTrace::Handle(const T::HostTraceRequest& request,
                        const std::function<Targets()>& targets)
    -> T::HostTraceT {
  if (auto mode = request.mode()) {
    if (*mode > static_cast<uint8_t>(T::Mode::MAX))
      throw Failure("invalid_request", "Unknown RPC trace mode");
    trace::SetMode(static_cast<trace::Mode>(*mode));
  }
  T::HostTraceT result;
  result.snapshot = std::make_unique<T::SnapshotT>();
  if (const auto* source = request.source(); source && !request.mode()) {
    if (source->size() == 0) {
      *result.snapshot = trace::ToWire(trace::Read());
    } else {
      auto peer = peers_.find(source->str());
      if (peer == peers_.end() || !peer->second.connection)
        throw Failure("unavailable", "Project trace is unavailable");
      result.snapshot = Query(peer->second, peer->first, std::nullopt);
      if (!result.snapshot)
        throw Failure("unavailable", "Project trace is unavailable");
    }
    return result;
  }
  Reconcile(targets());
  result.snapshot->mode = static_cast<T::Mode>(trace::CurrentMode());
  for (const auto& [id, peer] : peers_) {
    auto recorder = std::make_unique<T::RecorderT>();
    recorder->id = id;
    recorder->available = peer.connection != nullptr;
    result.recorders.push_back(std::move(recorder));
  }
  return result;
}
}  // namespace tsugumi::host
