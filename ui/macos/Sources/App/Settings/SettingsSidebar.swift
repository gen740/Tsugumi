import SwiftUI

/// Category list of a settings window (application, Workflow and Card
/// settings). Each row's tag has exactly the type of the selection, so a
/// click or the arrow keys change the selection and the content together.
struct SettingsSidebar<Item: Hashable>: View {
  let items: [Item]
  @Binding var selection: Item
  let title: (Item) -> String
  let symbol: (Item) -> String
  var width: CGFloat = 170

  var body: some View {
    List(selection: Binding<Item?>(get: { selection }, set: { if let v = $0 { selection = v } })) {
      ForEach(items, id: \.self) { item in
        Label(title(item), systemImage: symbol(item))
          .tag(Optional(item))
          .accessibilityIdentifier("settings.category.\(title(item))")
      }
    }
    .listStyle(.sidebar)
    .scrollContentBackground(.hidden)
    .frame(width: width)
  }
}
