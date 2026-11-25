#include "game.hh"

// The skill tree's rules: checks and applies one purchase (when one arrives),
// then prices every node for the page.
namespace {

const auto& P = Card::params;

constexpr size_t kBranchLength = 4;

auto bloom_index() -> size_t { return P.names.size() - 1; }

auto parent(size_t node) -> int {
  if (node == bloom_index() || node % kBranchLength == 0) return -1;
  return static_cast<int>(node) - 1;
}

auto owned(const glow::GameStateT& state, size_t node) -> bool {
  return node < state.nodes.size() && state.nodes[node] != 0;
}

// Why the node cannot be bought now, or empty when it can.
auto blocked(const glow::GameStateT& state, size_t node) -> std::string {
  if (node >= P.names.size()) return P.unknown_note;
  if (owned(state, node)) return P.owned_note;
  if (node == bloom_index() && state.replants < P.bloom_after)
    return game::replace(P.bloom_note, "{}", std::to_string(P.bloom_after));
  if (const int up = parent(node); up >= 0 && !owned(state, static_cast<size_t>(up)))
    return game::replace(P.parent_note, "{}", P.names.at(static_cast<size_t>(up)));
  if (state.seeds < P.costs.at(node)) return P.seeds_note;
  return {};
}

// Boosts follow from the nodes owned, so they are worked out again each time.
auto boosts_of(const glow::GameStateT& state) -> std::unique_ptr<glow::BoostsT> {
  auto boosts = std::make_unique<glow::BoostsT>();
  for (size_t node = 0; node + 1 < P.names.size(); ++node) {
    if (!owned(state, node)) continue;
    const double factor = P.factors.at(node);
    switch (node / kBranchLength) {
      case 0: boosts->click *= factor; break;
      case 1: boosts->roots *= factor; break;
      default: boosts->seeds *= factor; break;
    }
  }
  return boosts;
}

template <class Request>
auto buy(glow::GameStateT& state, const Request* request, glow::SkillOfferT& offer) -> void {
  const size_t node = request->node();
  if (auto reason = blocked(state, node); !reason.empty()) {
    std::fprintf(stderr, "rejected node %zu: %s\n", node, reason.c_str());
    offer.note = std::move(reason);
    return;
  }
  state.seeds -= P.costs.at(node);
  state.nodes.at(node) = 1;
  state.boosts = boosts_of(state);
  state.bloom = owned(state, bloom_index());
  offer.bought = static_cast<int16_t>(node);
  offer.note = game::replace(P.bought_note, "{}", P.names.at(node));
  std::fprintf(stderr, "bought node %zu (%s)\n", node, P.names.at(node).c_str());
}

}  // namespace

auto run(const Card::Inputs& inputs) -> Card::Output {
  Card::Output output;
  auto& offer = output.offer.emplace();
  // The request is a Turn when the screen opens, or a Purchase from the page.
  std::visit(
      [&](const auto* request) {
        if constexpr (requires { request->node(); }) {
          offer.turn = std::make_unique<glow::TurnT>(game::own_turn(request->turn()));
          offer.turn->state->nodes.resize(P.names.size(), 0);
          buy(*offer.turn->state, request, offer);
        } else {
          offer.turn = std::make_unique<glow::TurnT>(game::own_turn(request));
          offer.turn->state->nodes.resize(P.names.size(), 0);
        }
      },
      inputs.request);

  const auto& state = *offer.turn->state;
  for (size_t node = 0; node < P.names.size(); ++node) {
    auto& item = *offer.nodes.emplace_back(std::make_unique<glow::NodeOfferT>());
    item.name = P.names[node];
    item.effect = P.effects.at(node);
    item.cost = P.costs.at(node);
    item.parent = static_cast<int16_t>(parent(node));
    const auto reason = blocked(state, node);
    if (owned(state, node)) item.state = 3;
    else if (reason.empty()) item.state = 1;
    else if (reason == P.seeds_note) item.state = 2;
    else item.state = 0;
    item.note = reason;
  }
  return output;
}
