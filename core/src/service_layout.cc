#include <cmath>
#include <set>

#include "layout.hh"
#include "service.hh"

namespace tsugumi {
namespace R = Tsugumi::RPC;
namespace {
// Reject non-finite/unbounded geometry before sorting or converting to integer
// grid constraints. The transport already validates all required
// vectors/tables.
auto measured(double v) -> double {
  if (!std::isfinite(v) || std::abs(v) > 1e7)
    throw Failed(FailureKind::invalid_request,
                 "layout geometry is out of range");
  return v;
}
auto size(double v) -> double {
  if (measured(v) < 0)
    throw Failed(FailureKind::invalid_request,
                 "layout size must be nonnegative");
  return v;
}
auto scene_of(const R::LayoutScene& in) -> layout::Scene {
  layout::Scene s;
  const auto flow = in.flow()->str();
  if (flow != "topToBottom" && flow != "leftToRight")
    throw Failed(FailureKind::invalid_request, "unknown layout flow");
  s.top_to_bottom = flow == "topToBottom";
  s.grid = in.grid() == 0 ? 20 : measured(in.grid());
  if (s.grid < 1)
    throw Failed(FailureKind::invalid_request,
                 "layout grid must be at least 1");
  s.snap = in.snap();
  auto ports = [](const auto* ps) {
    std::vector<layout::Port> out;
    std::set<std::string> names;
    for (const auto* p : *ps) {
      if (!names.insert(p->name()->str()).second)
        throw Failed(FailureKind::invalid_request, "duplicate layout port");
      out.push_back({p->name()->str(), measured(p->x()), measured(p->y())});
    }
    return out;
  };
  std::set<std::string> ids;
  for (const auto* c : *in.cards()) {
    const auto id = c->id()->str();
    if (id.empty() || id.find('\0') != std::string::npos ||
        !ids.insert(id).second)
      throw Failed(FailureKind::invalid_request,
                   "invalid or duplicate layout Card id");
    s.cards.push_back({id, measured(c->x()), measured(c->y()), size(c->width()),
                       size(c->height()), ports(c->inputs()),
                       ports(c->outputs()), c->reorderable()});
  }
  for (const auto* w : *in.wires())
    s.wires.push_back({w->from()->str(), w->from_port()->str(), w->to()->str(),
                       w->to_port()->str()});
  auto boxes = [](const auto* bs) {
    std::vector<layout::Box> out;
    for (const auto* b : *bs)
      out.push_back({b->id()->str(), measured(b->x()), measured(b->y()),
                     size(b->width()), size(b->height())});
    return out;
  };
  s.frames = boxes(in.frames());
  s.notes = boxes(in.notes());
  auto bar = [](const R::LayoutBar* b) {
    layout::Bar out;
    if (!b) return out;
    for (const auto* e : *b->ends())
      out.ends.push_back(
          {e->name()->str(), e->card()->str(), e->port()->str()});
    if (b->x().has_value() != b->y().has_value())
      throw Failed(FailureKind::invalid_request,
                   "layout bar needs both coordinates");
    if (b->x()) out.at = layout::Point{measured(*b->x()), measured(*b->y())};
    return out;
  };
  s.inputs = bar(in.inputs());
  s.outputs = bar(in.outputs());
  return s;
}
}  // namespace

auto Service::layout_proposal(RequestIn& req) -> void {
  const auto* a = req.request()->value_as_LayoutArrangeRequest();
  const auto* u = req.request()->value_as_LayoutUntangleRequest();
  const auto scene = scene_of(*(a ? a->scene() : u->scene()));
  std::vector<std::string> around;
  if (u && u->around())
    for (const auto* id : *u->around()) around.push_back(id->str());
  const auto proposal =
      a ? layout::arrange(scene) : layout::untangle(scene, around);
  flatbuffers::FlatBufferBuilder b;
  auto places = [&](const auto& ps) {
    std::vector<flatbuffers::Offset<R::LayoutPlace>> out;
    for (const auto& p : ps)
      out.push_back(R::CreateLayoutPlace(b, b.CreateString(p.id), p.x, p.y));
    return b.CreateVector(out);
  };
  const auto cards = places(proposal.cards), frames = places(proposal.frames),
             notes = places(proposal.notes);
  std::vector<flatbuffers::Offset<R::LayoutPortOrder>> orders;
  for (const auto& o : proposal.port_orders) {
    const auto id = b.CreateString(o.id);
    const auto ins = b.CreateVectorOfStrings(o.inputs),
               outs = b.CreateVectorOfStrings(o.outputs);
    orders.push_back(R::CreateLayoutPortOrder(b, id, ins, outs));
  }
  const auto res =
      R::CreateLayoutProposal(b, cards, frames, notes, b.CreateVector(orders));
  reply(req, wire::reply(b, req.request_id, R::ReplyPayload::LayoutProposal,
                         res.Union()));
}
}  // namespace tsugumi
