import SwiftUI

struct CardNixReference: Identifiable {
  let source: CardKey
  let target: CardKey
  let name: String
  let local: Bool
  let valid: Bool
  var id: String { [source, local ? "local" : "upstream", name].joined(separator: "\u{0}") }
  var expression: String {
    let quoted = String(data: try! JSONEncoder().encode(name), encoding: .utf8)!
    return "inputs.\(local ? "local" : "upstream").\(quoted)"
  }
}

@MainActor func nixReferences(_ session: ProjectSession) -> [CardNixReference] {
  session.resolution.values.flatMap(\.nixReferences).sorted {
    $0.source == $1.source ? $0.expression < $1.expression : $0.source < $1.source
  }
}

/// Nested references meet the visible container. The list preserves the exact target.
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
  @Environment(\.openWindow) private var openWindow
  let session: ProjectSession
  @Bindable var tab: WorkflowTab

  var body: some View {
    HStack(spacing: 8) {
      Toggle(isOn: $tab.showNixReferences) { Label("Nix 参照（記述どおり）", systemImage: "snowflake") }
        .toggleStyle(.button)
        .help("保存済みの default.nix に書かれた Card 参照をそのまま表示します。別名や計算で決まる参照は含みません。")
      Button { tab.showNixList.toggle() } label: { Label("一覧", systemImage: "list.bullet") }
        .help("Nix 参照一覧")
        .popover(isPresented: $tab.showNixList) { referenceList }
    }
    .buttonStyle(.borderless).font(.callout)
    .padding(.horizontal, 12).padding(.vertical, 7)
    .background(.regularMaterial, in: Capsule())
  }

  private var referenceList: some View {
    ScrollViewReader { proxy in
      ScrollView {
        VStack(alignment: .leading, spacing: 10) {
          Text("Nix 参照一覧").font(.headline)
          Text("参照元 → 参照先。保存済みの default.nix にある静的な Card 参照を表示します。")
            .font(.caption).foregroundStyle(.secondary)
          let refs = nixReferences(session)
          if refs.isEmpty { Text("Card 間の Nix 参照はありません") }
          ForEach(refs) { ref in
            VStack(alignment: .leading, spacing: 6) {
              HStack {
                Button(ref.source) { jump(ref.source) }
                Text("→")
                Button(ref.target) { jump(ref.target) }.disabled(session.resolution[ref.target] == nil)
              }
              Text(ref.expression).font(.caption.monospaced()).textSelection(.enabled)
              if !ref.valid { Text("参照を解決できません。Nix 定義と診断を確認してください。").foregroundStyle(.secondary) }
              Button("Nix 定義を開く") {
                tab.showNixList = false
                openWindow(id: "card", value: CardRef(path: session.identity.url.path, card: ref.source, category: .nix))
              }
            }
            .padding(8).frame(maxWidth: .infinity, alignment: .leading)
            .background(tab.selectedNixReference == ref.id ? Color.purple.opacity(0.12) : .clear, in: RoundedRectangle(cornerRadius: 6))
            .id(ref.id)
          }
        }.padding(12)
      }
      .frame(width: 460, height: 420)
      .onAppear { if let id = tab.selectedNixReference { proxy.scrollTo(id) } }
      .onChange(of: tab.selectedNixReference) { _, id in if let id { proxy.scrollTo(id) } }
    }
  }

  private func jump(_ card: CardKey) {
    tab.enter(Array(path(card).dropLast()))
    tab.selection = [card]
    tab.revealNixCard = card
    tab.showNixList = false
  }
}
