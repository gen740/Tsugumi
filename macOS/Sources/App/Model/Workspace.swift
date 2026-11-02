import Foundation
import SwiftUI
import Observation

/// Window values for the scenes that show one project (and optionally a
/// Card or scope in it). Codable so windows restore across launches.
struct ProjectRef: Codable, Hashable {
  var path: String
  var url: URL { URL(fileURLWithPath: path) }
}

struct WorkflowRef: Codable, Hashable {
  var path: String
  var scope: [String]
}

enum CardEditorCategory: String, Codable, CaseIterable, Identifiable {
  case general, nix, source, ports, executions, artifacts, context
  var id: String { rawValue }

  var title: String {
    switch self {
    case .general: return String(localized: "一般")
    case .nix: return String(localized: "Nix 定義")
    case .source: return String(localized: "ソース")
    case .ports: return String(localized: "入出力")
    case .executions: return String(localized: "実行履歴")
    case .artifacts: return String(localized: "成果物")
    case .context: return String(localized: "追加コンテキスト")
    }
  }

  var symbol: String {
    switch self {
    case .general: return "slider.horizontal.3"
    case .nix: return "snowflake"
    case .source: return "doc.text"
    case .ports: return "arrow.left.arrow.right"
    case .executions: return "clock.arrow.circlepath"
    case .artifacts: return "shippingbox"
    case .context: return "text.bubble"
    }
  }
}

struct CardRef: Codable, Hashable {
  var path: String
  var card: CardKey
  var category: CardEditorCategory = .general
}

/// One tab of the workbench: a scope (the root workflow or a child workflow)
/// with its own selection and back history.
@MainActor
@Observable
final class WorkflowTab: Identifiable {
  let id = UUID()
  var scope: [String]
  var selection: Set<CardKey> = []
  var back: [[String]] = []
  var selectedConnection: Int?

  init(scope: [String], selection: Set<CardKey> = []) {
    self.scope = scope
    self.selection = selection
  }

  /// Switches the edited scope; the previous one goes on the back stack.
  func enter(_ s: [String]) {
    guard s != scope else { return }
    back.append(scope)
    scope = s
    selection = []
    selectedConnection = nil
  }

  func goBack() {
    guard let s = back.popLast() else { return }
    scope = s
    selection = []
    selectedConnection = nil
  }
}

enum BottomPanel: String, CaseIterable, Identifiable {
  case log, timeline
  var id: String { rawValue }
  var title: String { self == .log ? "ログ" : "タイムライン" }
}

/// State of one main window: which project is open, its tabs and panels.
@MainActor
@Observable
final class Workspace {
  var session: ProjectSession?
  var tabs: [WorkflowTab] = []
  var activeTab: UUID?
  var showChat = true
  var columns: NavigationSplitViewVisibility = .all
  var showBottom = false
  var bottomPanel: BottomPanel = .log
  /// Log filter chosen from elsewhere (an issue, a Card menu).
  var logCard: CardKey?

  var active: WorkflowTab? { tabs.first { $0.id == activeTab } ?? tabs.first }

  func open(_ s: ProjectSession) {
    session = s
    let tab = WorkflowTab(scope: s.initialScope, selection: s.initialSelection)
    tabs = [tab]
    activeTab = tab.id
  }

  func close() {
    session = nil
    tabs = []
    activeTab = nil
  }

  /// Opens a scope in a tab, reusing a tab that already shows it.
  func openTab(_ scope: [String]) {
    if let t = tabs.first(where: { $0.scope == scope }) {
      activeTab = t.id
      return
    }
    let t = WorkflowTab(scope: scope)
    tabs.append(t)
    activeTab = t.id
  }

  func closeTab(_ id: UUID) {
    guard tabs.count > 1, let i = tabs.firstIndex(where: { $0.id == id }) else { return }
    tabs.remove(at: i)
    if activeTab == id { activeTab = tabs[max(0, i - 1)].id }
  }

  func showLog(for card: CardKey?) {
    logCard = card
    bottomPanel = .log
    showBottom = true
  }
}
