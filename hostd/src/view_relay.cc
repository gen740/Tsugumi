#include "host/view_relay.hh"

#include <chrono>
#include <cmath>
#include <string_view>
#include <utility>

#include "host/failure.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/text.hh"

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;
constexpr auto kDeadline = std::chrono::seconds(10);
constexpr size_t kMaxPendingPerView = 16;
// Reserve frame overhead for the enclosing Host message and correlation IDs.
constexpr size_t kMaxPayloadBytes = TransportLimits{}.max_frame_bytes - 4096;
constexpr uint32_t kModifiers = 0x0f;

auto IsSession(std::string_view session) -> bool {
  return crypto::is_uuid(session) &&
         session.find_first_of("ABCDEF") == std::string_view::npos;
}

auto InvalidAction() -> void {
  throw Failure("invalid_request", "Invalid Card view input");
}

auto Validate(const H::ViewActionUnion& action) -> void {
  switch (action.type) {
    case H::ViewAction::ViewPointer: {
      const auto* pointer = action.AsViewPointer();
      if (!pointer || pointer->kind > 4 || pointer->buttons > 0x07 ||
          pointer->modifiers > kModifiers || !std::isfinite(pointer->x) ||
          !std::isfinite(pointer->y) || !std::isfinite(pointer->wheel_x) ||
          !std::isfinite(pointer->wheel_y))
        InvalidAction();
      return;
    }
    case H::ViewAction::ViewKey: {
      const auto* key = action.AsViewKey();
      if (!key || key->kind > 1 || key->code > 0xffff ||
          key->modifiers > kModifiers)
        InvalidAction();
      return;
    }
    case H::ViewAction::ViewText: {
      const auto* text = action.AsViewText();
      if (!text || text->text.size() > kMaxPayloadBytes ||
          !tsugumi::text::valid_utf8(text->text))
        InvalidAction();
      return;
    }
    case H::ViewAction::ViewGamepad: {
      const auto* pad = action.AsViewGamepad();
      const auto stick = [](float value) { return value >= -1 && value <= 1; };
      const auto trigger = [](float value) { return value >= 0 && value <= 1; };
      if (!pad || pad->kind > 2 || pad->buttons > 0x1ffff || !stick(pad->lx) ||
          !stick(pad->ly) || !stick(pad->rx) || !stick(pad->ry) ||
          !trigger(pad->lt) || !trigger(pad->rt))
        InvalidAction();
      return;
    }
    case H::ViewAction::ViewSnapshot:
      if (!action.AsViewSnapshot()) InvalidAction();
      return;
    default:
      InvalidAction();
  }
}
}  // namespace

auto ViewRelay::Report(ConnId owner, const H::ViewPresentationRequest& report)
    -> void {
  const auto& info = *report.window();
  const Address address{report.project_id()->str(), info.session()->str()};
  if (!IsSession(address.second))
    throw Failure("invalid_request", "Invalid Card view session");
  std::lock_guard lock(mutex_);
  const auto existing = windows_.find(address);
  if (existing != windows_.end() && existing->second.owner != owner)
    throw Failure("permission_denied", "Another connection presents this view");
  if (report.closed()) {
    if (existing != windows_.end()) Close(existing);
    return;
  }
  if (existing != windows_.end()) return;
  if ((info.type()->string_view() != "dawn" &&
       info.type()->string_view() != "html") ||
      info.mode() > 2 || info.flags() > 3)
    throw Failure("invalid_request", "Invalid Card view window");
  auto window = std::unique_ptr<H::ViewWindowT>(info.UnPack());
  windows_.emplace(address, Window{owner, std::move(*window)});
}

auto ViewRelay::Close(std::map<Address, Window>::iterator window) -> void {
  const auto& address = window->first;
  for (const auto& [_, request] : pending_) {
    if (request->address != address || request->done) continue;
    request->error = "The Card view closed before completing the request";
    request->done = true;
  }
  windows_.erase(window);
  changed_.notify_all();
}

auto ViewRelay::Disconnect(ConnId owner) -> void {
  std::lock_guard lock(mutex_);
  for (auto window = windows_.begin(); window != windows_.end();) {
    auto current = window++;
    if (current->second.owner == owner) Close(current);
  }
}

auto ViewRelay::List(const std::string& project) -> H::ViewWindowsT {
  std::lock_guard lock(mutex_);
  H::ViewWindowsT result;
  for (auto it = windows_.lower_bound({project, ""});
       it != windows_.end() && it->first.first == project; ++it)
    result.windows.push_back(std::make_unique<H::ViewWindowT>(it->second.info));
  return result;
}

auto ViewRelay::Execute(const H::ViewActionRequestT& request,
                        const Deliver& send, std::stop_token stop)
    -> H::ViewActionResultT {
  Validate(request.action);
  if (!IsSession(request.session))
    throw Failure("invalid_request", "Invalid Card view session");
  const Address address{request.project_id, request.session};
  const bool snapshot = request.action.type == H::ViewAction::ViewSnapshot;
  std::unique_lock lock(mutex_);
  if (stop.stop_requested())
    throw Failure("cancelled", "The Card view request was cancelled");
  const auto window = windows_.find(address);
  if (window == windows_.end())
    throw Failure("not_found", "The Card view is no longer open");
  if (!snapshot && window->second.info.type != "dawn")
    throw Failure("invalid_request", "Only Dawn views receive synthetic input");
  if (window->second.pending >= kMaxPendingPerView)
    throw Failure("busy", "The Card view request queue is full");

  H::ViewActionDispatchT delivery;
  delivery.request = crypto::uuid4();
  delivery.project_id = request.project_id;
  delivery.session = request.session;
  delivery.action = request.action;
  auto pending = std::make_shared<Pending>(
      Pending{address, window->second.owner, snapshot, false, {}, {}});
  const auto id = delivery.request;
  pending_.emplace(id, pending);
  ++window->second.pending;
  auto finish = [&] {
    pending_.erase(id);
    const auto current = windows_.find(address);
    if (!pending->done && current != windows_.end()) --current->second.pending;
  };
  try {
    send(pending->owner, std::move(delivery));
    const auto until = std::chrono::steady_clock::now() + kDeadline;
    changed_.wait_until(lock, stop, until, [&] { return pending->done; });
  } catch (...) {
    finish();
    throw;
  }
  finish();
  if (!pending->done) {
    if (stop.stop_requested())
      throw Failure("cancelled", "The Card view request was cancelled");
    throw Failure("timeout", "The Card view did not complete the request");
  }
  if (!pending->error.empty())
    throw Failure("view_error", std::move(pending->error));
  return std::move(pending->result);
}

auto ViewRelay::Complete(ConnId owner,
                         const H::ViewActionCompleteRequest& reply) -> void {
  std::lock_guard lock(mutex_);
  const auto it = pending_.find(reply.request()->str());
  // A request can end while the renderer is completing it.
  if (it == pending_.end() || it->second->done) return;
  auto& pending = *it->second;
  if (pending.owner != owner ||
      pending.address.second != reply.session()->string_view())
    throw Failure("permission_denied",
                  "This connection does not own the view request");
  if (reply.error() && reply.error()->size()) {
    pending.error = reply.error()->str().substr(0, 1024);
  } else if (pending.snapshot) {
    const auto* png = reply.png();
    constexpr std::string_view kSignature("\x89PNG\r\n\x1a\n", 8);
    if (!png || png->size() < kSignature.size() ||
        png->size() > kMaxPayloadBytes ||
        std::string_view(reinterpret_cast<const char*>(png->data()),
                         kSignature.size()) != kSignature) {
      pending.error =
          "The renderer did not return a valid PNG within the size limit";
    } else {
      pending.result.png.assign(png->begin(), png->end());
    }
  }
  --windows_.at(pending.address).pending;
  pending.done = true;
  changed_.notify_all();
}

}  // namespace tsugumi::host
