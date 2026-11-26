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

/// A direct reference, clipped at the edges of its source and target Cards.
struct NixReferenceSegment {
  let start: CGPoint
  let end: CGPoint

  init?(_ source: CGRect, _ target: CGRect) {
    let a = source.center, b = target.center
    let dx = b.x - a.x, dy = b.y - a.y
    guard dx != 0 || dy != 0 else { return nil }

    func edge(_ rect: CGRect) -> CGFloat {
      min(dx == 0 ? .infinity : rect.width / (2 * abs(dx)),
          dy == 0 ? .infinity : rect.height / (2 * abs(dy)))
    }

    let targetEdge = 1 - edge(target)
    let endAt = targetEdge > 0 ? targetEdge : 1
    let sourceEdge = edge(source)
    let startAt = sourceEdge < endAt ? sourceEdge : 0
    start = CGPoint(x: a.x + dx * startAt, y: a.y + dy * startAt)
    end = CGPoint(x: a.x + dx * endAt, y: a.y + dy * endAt)
  }

  var path: Path {
    Path { line in
      line.move(to: start)
      line.addLine(to: end)
    }
  }

  func arrow(applying transform: CGAffineTransform) -> Path {
    let tail = start.applying(transform), tip = end.applying(transform)
    let dx = tip.x - tail.x, dy = tip.y - tail.y
    let length = hypot(dx, dy)
    guard length > 0 else { return Path() }
    let x = dx / length, y = dy / length
    let back = min(10, length)
    let base = CGPoint(x: tip.x - x * back, y: tip.y - y * back)
    return Path { arrow in
      arrow.move(to: tip)
      arrow.addLine(to: CGPoint(x: base.x - y * 4, y: base.y + x * 4))
      arrow.addLine(to: CGPoint(x: base.x + y * 4, y: base.y - x * 4))
      arrow.closeSubpath()
    }
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
