import Foundation
import SwiftUI
import Observation
import TsugumiRPC

/// Window values for the scenes that show one project (and optionally a
/// Card or scope in it). Codable so windows restore across launches.
struct ProjectRef: Codable, Hashable {
  var path: String
  /// Definition to show first (Card definitions window).
  var definition: String? = nil
  var url: URL { URL(fileURLWithPath: path) }
}

struct WorkflowRef: Codable, Hashable {
  var path: String
  var scope: [String]
}

enum CardEditorCategory: String, Codable, CaseIterable, Identifiable {
  case general, comments, nix, source, ports, executions, artifacts, context
  var id: String { rawValue }

  var title: String {
    switch self {
    case .general: return String(localized: "一般")
    case .comments: return String(localized: "コメント")
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
    case .comments: return "bubble.left"
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
  var selection: Set<CardKey> = [] {
    // Choosing Cards anew lets go of the notes and frames: those come from a marquee.
    didSet {
      selectedNotes = []
      selectedFrames = []
    }
  }
  /// Sticky notes (ids) and Cards' output frames caught by a marquee, set after `selection`.
  var selectedNotes: Set<String> = []
  var selectedFrames: Set<CardKey> = []
  var back: [[String]] = []
  var selectedConnection: Int?

  init(scope: [String], selection: Set<CardKey> = []) {
    self.scope = scope
    self.selection = selection
  }

  var hasSelection: Bool { !selection.isEmpty || !selectedNotes.isEmpty || !selectedFrames.isEmpty || selectedConnection != nil }

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
  case log, timeline, terminal
  var id: String { rawValue }
  var title: String {
    switch self {
    case .log: return String(localized: "ログ")
    case .timeline: return String(localized: "タイムライン")
    case .terminal: return String(localized: "ターミナル")
    }
  }
  var symbol: String {
    switch self {
    case .log: return "text.alignleft"
    case .timeline: return "chart.bar.xaxis"
    case .terminal: return "terminal"
    }
  }
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
  /// Log filter chosen from elsewhere (an issue, a Card menu, a stage icon).
  var logCard: CardKey?
  var logInvocation: UInt64?
  var logStream: Tsugumi_Data_LogStream = .journal
  /// Stage whose log section to open (a click on a stage icon).
  var logStage: Stage?

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

  /// Opens the log of a Card. A stage opens the stream that stage writes to
  /// (Nix evaluation and build: stderr; execution: stdout; others: journal).
  /// The log / timeline icons: a click opens that panel, or closes it when
  /// it is already the one shown.
  func togglePanel(_ p: BottomPanel) {
    if showBottom && bottomPanel == p { showBottom = false } else { bottomPanel = p; showBottom = true }
  }

  func showLog(for card: CardKey?, stage: Stage? = nil, invocation: UInt64? = nil) {
    logCard = card
    logInvocation = invocation
    logStage = stage
    if card == nil { logStream = .journal }
    bottomPanel = .log
    showBottom = true
  }
}
