#include "host/debug_trace.hh"

#include <utility>

#include "agent/wire.hh"
#include "host/failure.hh"
#include "tsugumi/protocol/tsugumi_generated.h"
#include "tsugumi/util/log.hh"

namespace tsugumi::host {
namespace {
namespace R = Tsugumi::RPC;
namespace T = Tsugumi::Trace;
using namespace std::chrono_literals;

// A dead project peer must not hold up control of the other recorders.
constexpr auto kPeerDeadline = 2s;

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
              kPeerDeadline)
          .get();
  if (response.error)
    throw Failure("unavailable", "Project trace connection lost");
  return std::move(response.bytes);
}
}  // namespace

auto DebugTrace::Feed::Notify(const std::string& source, bool catalog) -> void {
  std::lock_guard lock(mutex);
  if (!viewer || (catalog && !std::exchange(catalog_armed, false))) return;
  T::TraceChangedT changed;
  changed.source = source;
  changed.catalog = catalog;
  server->reply(viewer, agent::wire::HostEvent(std::move(changed)));
}

auto DebugTrace::Attach(TransportServer& server) -> void {
  feed_->server = &server;
}

auto DebugTrace::Subscribe(ConnId conn, bool catalog) -> void {
  std::lock_guard lock(feed_->mutex);
  if (!feed_->viewer) feed_->viewer = conn;
  if (feed_->viewer != conn)
    throw Failure("busy",
                  "Only the subscribed viewer can watch RPC trace changes");
  if (catalog) feed_->catalog_armed = true;
}

auto DebugTrace::CatalogChanged() -> void { feed_->Notify({}, true); }

auto DebugTrace::Disconnect(ConnId conn) -> void {
  {
    std::lock_guard lock(feed_->mutex);
    if (!conn || feed_->viewer != conn) return;
    feed_->viewer = 0;
    feed_->catalog_armed = false;
  }
  trace::CancelWatch();
}

auto DebugTrace::Stop() -> void {
  Disconnect(feed_->viewer);
  peers_.clear();
}

auto DebugTrace::Query(Peer& peer, const std::string& project,
                       std::optional<trace::Mode> mode, uint64_t after)
    -> std::unique_ptr<T::SnapshotT> {
  try {
    if (!peer.connection) {
      client::Options options;
      options.endpoint = peer.target.endpoint;
      peer.connection = std::make_unique<client::Connection>(options);
      auto started = peer.connection->Start();
      if (started.wait_for(kPeerDeadline) != std::future_status::ready ||
          started.get())
        throw Failure("unavailable", "Cannot connect to project trace");
      auto hello =
          Call(*peer.connection, R::RequestPayload::SessionHelloRequest,
               [&](auto& builder) {
                 return R::CreateSessionHelloRequest(
                     builder, builder.CreateString(project),
                     peer.target.token.empty()
                         ? 0
                         : builder.CreateString(peer.target.token),
                     T::Client::Trace);
               });
      const auto* reply = R::GetMessage(hello.data())->payload_as_Reply();
      if (!reply || !reply->value_as_HelloResult())
        throw Failure("unavailable", "Project trace authentication failed");
      const std::weak_ptr<Feed> feed = feed_;
      peer.connection->OnEvent([feed, project](client::Bytes bytes) {
        const auto* note =
            R::GetMessage(bytes.data())->payload_as_Notification();
        if (note && note->value_as_TraceChanged())
          if (auto viewer = feed.lock()) viewer->Notify(project);
      });
      peer.generation = ++next_generation_;
      peer.connection->OnClosed(
          [this, project, generation = peer.generation](client::Error) {
            lane_.submit([this, project, generation] {
              PeerClosed(project, generation);
            });
          });
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
                   : flatbuffers::nullopt,
              after);
        });
    const auto* reply = R::GetMessage(response.data())->payload_as_Reply();
    const auto* snapshot = reply ? reply->value_as_TraceSnapshot() : nullptr;
    if (!snapshot)
      throw Failure("unavailable", "Project trace request was rejected");
    peer.mode = static_cast<trace::Mode>(snapshot->mode());
    auto result = std::unique_ptr<T::SnapshotT>(snapshot->UnPack());
    result->generation = peer.generation;
    return result;
  } catch (const Failure& failure) {
    log::error("RPC trace for " + project + ": " + failure.what());
    return nullptr;
  }
}

auto DebugTrace::PeerClosed(const std::string& project, uint64_t generation)
    -> void {
  auto found = peers_.find(project);
  if (found == peers_.end()) return;
  auto& peer = found->second;
  if (!peer.connection || peer.generation != generation) return;
  peer.connection.reset();
  const auto mode = trace::CurrentMode();
  if (mode != trace::Mode::Off && !Query(peer, project, mode))
    peer.connection.reset();
  CatalogChanged();
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
    if ((peer.connection ? peer.mode != mode : mode != trace::Mode::Off) &&
        !Query(peer, id, mode))
      peer.connection.reset();
}

auto DebugTrace::Handle(ConnId conn, const T::HostTraceRequest& request,
                        const std::function<Targets()>& targets)
    -> T::HostTraceT {
  const auto* source = request.source();
  Subscribe(conn, !source);
  if (auto mode = request.mode()) {
    if (*mode > static_cast<uint8_t>(T::Mode::MAX))
      throw Failure("invalid_request", "Unknown RPC trace mode");
    trace::SetMode(static_cast<trace::Mode>(*mode));
  }
  T::HostTraceT result;
  result.snapshot = std::make_unique<T::SnapshotT>();
  if (source && !request.mode()) {
    if (source->size() == 0) {
      *result.snapshot = trace::ToWire(
          trace::Read([feed = feed_] { feed->Notify({}); }, request.after()));
    } else {
      auto peer = peers_.find(source->str());
      if (peer == peers_.end() || !peer->second.connection)
        throw Failure("unavailable", "Project trace is unavailable");
      const auto after =
          request.generation() == peer->second.generation ? request.after() : 0;
      result.snapshot = Query(peer->second, peer->first, std::nullopt, after);
      if (!result.snapshot) {
        peer->second.connection->Close();
        throw Failure("unavailable", "Project trace is unavailable");
      }
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
