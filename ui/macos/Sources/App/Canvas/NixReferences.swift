import SwiftUI

struct CardNixReference: Identifiable {
  let source: CardKey
  let target: CardKey
  let name: String
  var id: String { [source, target, name].joined(separator: "\u{0}") }
  var expression: String {
    let quoted = String(data: try! JSONEncoder().encode(name), encoding: .utf8)!
    let attr = name.range(of: #"^[A-Za-z_][A-Za-z0-9_'-]*$"#, options: .regularExpression) == nil ? quoted : name
    return "inputs.\(attr)"
  }
}

@MainActor func nixReferences(_ session: ProjectSession) -> [CardNixReference] {
  session.resolution.values.flatMap(\.nixReferences).sorted {
    $0.source == $1.source ? $0.expression < $1.expression : $0.source < $1.source
  }
}

/// Nested references meet the visible container.
func visibleNixCard(_ card: CardKey, scope: [String], visible: Set<CardKey>) -> CardKey? {
  let parts = path(card)
  guard parts.count > scope.count, Array(parts.prefix(scope.count)) == scope else { return nil }
  let projected = key(Array(parts.prefix(scope.count + 1)))
  return visible.contains(projected) ? projected : nil
}

/// A link one scope draws: both ends projected to Cards visible there.
struct NixLink {
  let reference: CardNixReference
  let source: CardKey
  let target: CardKey
}

func visibleNixLinks(_ references: [CardNixReference], scope: [String], visible: Set<CardKey>) -> [NixLink] {
  references.compactMap { ref in
    guard let source = visibleNixCard(ref.source, scope: scope, visible: visible),
          let target = visibleNixCard(ref.target, scope: scope, visible: visible), source != target else { return nil }
    return NixLink(reference: ref, source: source, target: target)
  }
}

/// A hovered Card shows all of its links; otherwise the link picked on the canvas.
func activeNixLinks(_ links: [NixLink], hovered: CardKey?, selected: String?) -> [NixLink] {
  links.filter {
    if let hovered { return $0.source == hovered || $0.target == hovered }
    return $0.reference.id == selected
  }
}

func nixCurve(_ a: CGRect, _ b: CGRect) -> Path {
  let right = b.midX >= a.midX
  let start = CGPoint(x: right ? a.maxX : a.minX, y: a.midY)
  let end = CGPoint(x: right ? b.minX : b.maxX, y: b.midY)
  let bend = max(45, abs(end.x - start.x) / 2) * (right ? 1.0 : -1.0)
  return Path { p in
    p.move(to: start)
    p.addCurve(to: end, control1: CGPoint(x: start.x + bend, y: start.y), control2: CGPoint(x: end.x - bend, y: end.y))
  }
}

struct NixReferenceControls: View {
  @Bindable var tab: WorkflowTab

  var body: some View {
    Toggle(isOn: $tab.showNixReferences) { Image(systemName: "snowflake") }
      .toggleStyle(.button)
      .buttonStyle(.borderless)
      .help("配線から決まる Nix の Card 入力を表示します。")
      .accessibilityLabel("Nix 参照")
  }
}
