import AppKit
import SwiftUI
import TsugumiAgentClient

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

/// Refreshes the open menu from agentd. Catalog persistence and empty-refresh
/// handling are owned by the service; only UI scheduling lives here.
@MainActor
final class ModelListRefresher {
  typealias Fetch = @Sendable (String) async throws -> [(id: String, name: String)]
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
  /// Connections being listed: opening the menu again does not ask them twice.
  private(set) var running: Set<String> = []

  init(fetch: @escaping Fetch) {
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
          }
          self.running.remove(id)
          finished(id, outcome)
        }
      }
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
  /// The effort chosen ("": the model's default), and Fast mode turned on or off.
  var onEffort: ((String) -> Void)?
  var onFast: ((Bool) -> Void)?
  weak var anchor: NSView?
  let menu = NSMenu()
  private var connections: [ChatPanel.Connection] = []
  private var provider = ""
  private var model = ""
  private var names: [String: String] = [:]
  /// What goes into the requests of the model in use.
  private var effort: String?
  private var fast = false

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

  func setChoices(effort: String?, fast: Bool) {
    self.effort = effort
    self.fast = fast
    rebuild()
  }

  func setCurrent(provider: String, model: String) {
    self.provider = provider
    self.model = model
    rebuild()
  }

  /// The items, also while the menu is open.
  private func rebuild() {
    var items = connections.isEmpty ? [] : choiceItems()
    for c in connections {
      if !items.isEmpty { items.append(.separator()) }
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
      $0.title == $1.title && $0.state == $1.state && $0.isEnabled == $1.isEnabled && ($0.representedObject as? [String]) == ($1.representedObject as? [String])
    }
    if !same { menu.items = items }
  }

  /// The effort and Fast mode of the model in use: above the models, whose
  /// lists are long and change. What cannot be chosen says why.
  private func choiceItems() -> [NSMenuItem] {
    let options = ModelOptions(connection: provider, model: model)
    var items = [NSMenuItem.sectionHeader(title: "Effort")]
    if options.efforts.isEmpty {
      items.append(unavailable(String(localized: "このモデルでは選べません（公式の文書で対応を確かめたモデルだけ選べます）")))
    } else {
      let title = options.defaultEffort.map { String(localized: "既定（\($0)）") } ?? String(localized: "既定")
      items.append(effortItem(title, level: ""))
      items += options.efforts.map { effortItem($0, level: $0) }
    }
    items.append(.separator())
    if let price = options.fastPrice {
      let times = String(format: "%g", price)
      let item = NSMenuItem(title: String(localized: "Fast mode（料金は通常の \(times) 倍）"), action: #selector(toggledFast), keyEquivalent: "")
      item.target = self
      item.state = fast ? .on : .off
      items.append(item)
    } else {
      items.append(unavailable(provider == "chatgpt" ? String(localized: "Fast mode: ChatGPT プランでは選べません（指定が拒否されることがあるため）")
        : provider == "openai" ? String(localized: "Fast mode: このモデルでは選べません（公式の料金表で確かめたモデルだけ選べます）")
        : String(localized: "Fast mode: Claude Opus 5.5 でだけ選べます")))
    }
    return items
  }

  private func unavailable(_ title: String) -> NSMenuItem {
    let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
    item.isEnabled = false
    return item
  }

  private func effortItem(_ title: String, level: String) -> NSMenuItem {
    let item = NSMenuItem(title: title, action: #selector(pickedEffort(_:)), keyEquivalent: "")
    item.target = self
    item.representedObject = ["effort", level]
    item.state = (effort ?? "") == level ? .on : .off
    return item
  }

  @objc private func pickedEffort(_ item: NSMenuItem) {
    guard let pick = item.representedObject as? [String], pick.count == 2 else { return }
    onEffort?(pick[1])
  }

  @objc private func toggledFast() { onFast?(!fast) }

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
