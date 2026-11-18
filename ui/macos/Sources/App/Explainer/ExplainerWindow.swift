import AppKit

/// The published documentation is shared by all clients.
enum Explainer {
  static let published = URL(string: "https://gen740.github.io/Tsugumi/")!
  static func open() { NSWorkspace.shared.open(published) }
}
