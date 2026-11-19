import SwiftUI

/// A small (i) button beside a setting or a section's title: its explanation
/// (one or more paragraphs) opens in a popover when clicked, so the panes
/// show settings and state only.
struct InfoButton: View {
  private let paragraphs: [Text]
  @State private var shown = false

  init(_ key: LocalizedStringKey) { paragraphs = [Text(key)] }
  init(_ paragraphs: Text...) { self.paragraphs = paragraphs }
  init(paragraphs: [Text]) { self.paragraphs = paragraphs }

  var body: some View {
    Button { shown.toggle() } label: { Image(systemName: "info.circle") }
      .buttonStyle(.borderless)
      .foregroundStyle(.secondary)
      .accessibilityLabel("説明")
      .popover(isPresented: $shown, arrowEdge: .bottom) {
        VStack(alignment: .leading, spacing: 8) {
          ForEach(paragraphs.indices, id: \.self) { paragraphs[$0] }
        }
        .font(.callout).textSelection(.enabled)
        .fixedSize(horizontal: false, vertical: true)
        .frame(width: 320, alignment: .leading)
        .padding(12)
      }
  }
}

/// A section's title with its explanation behind an (i) button.
struct InfoHeader: View {
  let title: LocalizedStringKey
  let info: [Text]
  init(_ title: LocalizedStringKey, info: LocalizedStringKey) { self.title = title; self.info = [Text(info)] }
  init(_ title: LocalizedStringKey, info: Text...) { self.title = title; self.info = info }

  var body: some View {
    HStack(spacing: 4) {
      Text(title)
      InfoButton(paragraphs: info)
    }
  }
}
