import AppKit
import SwiftUI

/// Runs on the main thread soon, also while an NSMenu is open: menu tracking
/// serves run loop blocks and timers in the common modes, but neither the main
/// queue nor the main actor until it ends (checked on macOS 26).
nonisolated func onMainWhileMenuTracks(_ body: @escaping @MainActor @Sendable () -> Void) {
  final class Once: @unchecked Sendable {
    private let lock = NSLock()
    private var body: (@MainActor @Sendable () -> Void)?
    init(_ body: @escaping @MainActor @Sendable () -> Void) { self.body = body }
    func run() {
      lock.lock()
      let b = body
      body = nil
      lock.unlock()
      if let b { MainActor.assumeIsolated { b() } }
    }
  }
  let once = Once(body)
  RunLoop.main.perform(inModes: [.common]) { once.run() }
  CFRunLoopWakeUp(CFRunLoopGetMain())
  // Where the main thread runs no run loop (tests).
  DispatchQueue.main.async { once.run() }
}

/// Lists the models of connections again and keeps what comes back where
/// Settings > API keeps it. One connection's failure or slowness touches
/// nothing else, and a failed or empty answer leaves its saved list as it is.
@MainActor
final class ModelListRefresher {
  typealias Fetch = @Sendable (String) async throws -> [(id: String, label: String)]
  enum Outcome: Equatable, CustomStringConvertible {
    case updated(Int), empty, failed
    var description: String {
      switch self {
      case .updated(let n): return "\(n) models"
      case .empty: return "empty, kept the saved list"
      case .failed: return "failed, kept the saved list"
      }
    }
  }

  private let fetch: Fetch
  private let defaults: UserDefaults
  /// Connections being listed: opening the menu again does not ask them twice.
  private(set) var running: Set<String> = []

  init(defaults: UserDefaults = .standard, fetch: @escaping Fetch) {
    self.defaults = defaults
    self.fetch = fetch
  }

  /// `finished` is called for each connection, as soon as its answer comes,
  /// also while the menu is open.
  func refresh(_ ids: [String], finished: @escaping @MainActor @Sendable (String, Outcome) -> Void) {
    for id in ids where !running.contains(id) {
      running.insert(id)
      let fetch = self.fetch
      Task.detached {
        let list = try? await fetch(id)
        onMainWhileMenuTracks {
          var outcome = Outcome.failed
          if let list {
            outcome = list.isEmpty ? .empty : .updated(list.count)
            if !list.isEmpty { self.store(id, list) }
          }
          self.running.remove(id)
          finished(id, outcome)
        }
      }
    }
  }

  private func store(_ id: String, _ list: [(id: String, label: String)]) {
    defaults.set(list.map(\.id), forKey: AgentSettings.modelsKey(id))
    if id == "chatgpt" {
      defaults.set(Dictionary(list.map { ($0.id, $0.label) }, uniquingKeysWith: { a, _ in a }), forKey: "agent.modelNames")
    }
  }
}

/// The chat's model menu. An NSMenu rather than SwiftUI's Menu: its delegate
/// says when it opens, each time, whatever opened it, and its items can
/// change while it is open.
@MainActor
final class ModelMenu: NSObject, NSMenuDelegate {
  var onOpen: (() -> Void)?
  var onChoose: ((String, String) -> Void)?
  weak var anchor: NSView?
  let menu = NSMenu()
  private var connections: [ChatPanel.Connection] = []
  private var provider = ""
  private var model = ""
  private var names: [String: String] = [:]

  override init() {
    super.init()
    menu.delegate = self
    menu.autoenablesItems = false
    rebuild()
  }

  func show() {
    guard let anchor else { return }
    // After the button's action returns: popUp stays until the menu closes.
    DispatchQueue.main.async { [menu] in
      menu.popUp(positioning: nil, at: NSPoint(x: 0, y: anchor.isFlipped ? anchor.bounds.maxY + 2 : -2), in: anchor)
    }
  }

  func menuWillOpen(_ menu: NSMenu) { onOpen?() }

  func setContents(connections: [ChatPanel.Connection], provider: String, model: String, names: [String: String]) {
    self.connections = connections
    self.provider = provider
    self.model = model
    self.names = names
    rebuild()
  }

  func setCurrent(provider: String, model: String) {
    self.provider = provider
    self.model = model
    rebuild()
  }

  /// The items, also while the menu is open.
  private func rebuild() {
    var items: [NSMenuItem] = []
    for c in connections {
      items.append(NSMenuItem.sectionHeader(title: c.name))
      for m in c.models {
        let item = NSMenuItem(title: names[m] ?? m, action: #selector(picked(_:)), keyEquivalent: "")
        item.target = self
        item.representedObject = [c.id, m]
        item.state = c.id == provider && m == model ? .on : .off
        items.append(item)
      }
    }
    if items.isEmpty {
      let none = NSMenuItem(title: String(localized: "設定の「API」で接続を設定してください"), action: nil, keyEquivalent: "")
      none.isEnabled = false
      items.append(none)
    }
    let same = menu.items.count == items.count && zip(menu.items, items).allSatisfy {
      $0.title == $1.title && $0.state == $1.state && ($0.representedObject as? [String]) == ($1.representedObject as? [String])
    }
    if !same { menu.items = items }
  }

  @objc private func picked(_ item: NSMenuItem) {
    guard let pick = item.representedObject as? [String], pick.count == 2 else { return }
    onChoose?(pick[0], pick[1])
  }
}

/// Where the model menu opens: the view behind its button.
struct ModelMenuAnchor: NSViewRepresentable {
  let menu: ModelMenu
  func makeNSView(context: Context) -> NSView {
    let v = NSView()
    menu.anchor = v
    return v
  }
  func updateNSView(_ v: NSView, context: Context) { menu.anchor = v }
}
