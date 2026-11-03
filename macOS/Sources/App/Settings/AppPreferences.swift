import AppKit
import Observation
import SwiftUI
import TsugumiRPC

/// Commands whose keys can be reassigned in Settings > Shortcuts.
enum ShortcutAction: String, CaseIterable, Identifiable, Codable {
  // The app and the project
  case newProject, openProject, home, save, duplicateProject, find, run, stop
  // The canvas, while it has the focus (single keys; never while typing)
  case openSettings, openArtifacts, openLog, toggleCollapse, duplicate, fitAll, actualSize, zoomIn, zoomOut, arrange, group
  case addDraft, generateDrafts
  // Panels and the rest
  case toggleChat, toggleLog, toggleTimeline, toggleTerminal, showDetails, addNote, formatSource
  var id: String { rawValue }

  var title: String {
    switch self {
    case .newProject: return String(localized: "新しいプロジェクト")
    case .openProject: return String(localized: "プロジェクトを開く")
    case .home: return String(localized: "Home へ戻る")
    case .save: return String(localized: "保存")
    case .duplicateProject: return String(localized: "プロジェクトを複製")
    case .find: return String(localized: "テキスト検索")
    case .run: return String(localized: "ワークフローを実行")
    case .stop: return String(localized: "実行を停止")
    case .openSettings: return String(localized: "Card 設定を開く")
    case .openArtifacts: return String(localized: "成果物を開く")
    case .openLog: return String(localized: "実行ログを開く")
    case .toggleCollapse: return String(localized: "折りたたみを切り替える")
    case .duplicate: return String(localized: "Card を複製")
    case .fitAll: return String(localized: "全体表示")
    case .actualSize: return String(localized: "100% で表示")
    case .zoomIn: return String(localized: "拡大")
    case .zoomOut: return String(localized: "縮小")
    case .arrange: return String(localized: "整列")
    case .group: return String(localized: "グループ操作")
    case .addDraft: return String(localized: "Draft Card を追加")
    case .generateDrafts: return String(localized: "Draft から Card を生成")
    case .toggleChat: return String(localized: "チャットの表示切替")
    case .toggleLog: return String(localized: "ログの表示切替")
    case .toggleTimeline: return String(localized: "タイムラインの表示切替")
    case .toggleTerminal: return String(localized: "ターミナルの表示切替")
    case .showDetails: return String(localized: "選択した Card の詳細")
    case .addNote: return String(localized: "付箋を追加")
    case .formatSource: return String(localized: "ソースコードを整形")
    }
  }

  /// Works only while the canvas has the focus (a key without ⌘ would
  /// otherwise be taken from a text field).
  var canvasOnly: Bool {
    switch self {
    case .openSettings, .openArtifacts, .openLog, .toggleCollapse, .duplicate, .fitAll, .actualSize, .zoomIn, .zoomOut,
         .arrange, .group, .addDraft, .generateDrafts: return true
    default: return false
    }
  }

  var defaultKey: KeyBinding {
    switch self {
    case .newProject: return KeyBinding(key: "n", command: true)
    case .openProject: return KeyBinding(key: "o", command: true)
    case .home: return KeyBinding(key: "h", command: true, shift: true)
    case .save: return KeyBinding(key: "s", command: true)
    case .duplicateProject: return KeyBinding(key: "s", command: true, shift: true)
    case .find: return KeyBinding(key: "f", command: true)
    case .run: return KeyBinding(key: "r", command: true)
    case .stop: return KeyBinding(key: ".", command: true)
    case .openSettings: return KeyBinding(key: "s")
    case .openArtifacts: return KeyBinding(key: "a")
    case .openLog: return KeyBinding(key: "l")
    case .toggleCollapse: return KeyBinding(key: "f")
    case .duplicate: return KeyBinding(key: "c")
    case .fitAll: return KeyBinding(key: "0")
    case .actualSize: return KeyBinding(key: "1")
    case .zoomIn: return KeyBinding(key: "=", shift: true)
    case .zoomOut: return KeyBinding(key: "-")
    case .arrange: return KeyBinding(key: "a", shift: true)
    case .group: return KeyBinding(key: "g")
    case .addDraft: return KeyBinding(key: "e")
    case .generateDrafts: return KeyBinding(key: "d", shift: true)
    case .toggleChat: return KeyBinding(key: "j", command: true, option: true)
    case .toggleLog: return KeyBinding(key: "l", command: true, option: true)
    case .toggleTimeline: return KeyBinding(key: "t", command: true, option: true)
    case .toggleTerminal: return KeyBinding(key: "`", control: true)
    case .showDetails: return KeyBinding(key: "i", command: true, option: true)
    case .addNote: return KeyBinding(key: "n", command: true, option: true)
    case .formatSource: return KeyBinding(key: "f", option: true, shift: true)
    }
  }
}

struct KeyBinding: Codable, Hashable {
  var key: String
  var command = false
  var option = false
  var shift = false
  var control = false

  var modifiers: EventModifiers {
    var m: EventModifiers = []
    if command { m.insert(.command) }
    if option { m.insert(.option) }
    if shift { m.insert(.shift) }
    if control { m.insert(.control) }
    return m
  }

  var keyEquivalent: KeyEquivalent {
    switch key {
    case "\r": return .return
    case "\u{1b}": return .escape
    default: return KeyEquivalent(key.first ?? " ")
    }
  }

  var label: String {
    let k: String
    switch key {
    case "\r": k = "↩"
    case "\u{1b}": k = "⎋"
    default: k = key.uppercased()
    }
    return (control ? "⌃" : "") + (option ? "⌥" : "") + (shift ? "⇧" : "") + (command ? "⌘" : "") + k
  }
}

struct SkillFolder: Codable, Hashable, Identifiable {
  var id: String { path }
  var path: String
  var enabled = true
}

/// Application-wide settings (UserDefaults). Project settings live in the
/// project documents instead.
@MainActor
@Observable
final class AppPreferences {
  static let shared = AppPreferences()
  @ObservationIgnored private let d = UserDefaults.standard

  enum Language: String, CaseIterable, Identifiable {
    case system, ja, en
    var id: String { rawValue }
    var label: String { self == .system ? "システムに従う" : (self == .ja ? "日本語" : "English") }
  }

  enum Theme: String, CaseIterable, Identifiable {
    case system, light, dark
    var id: String { rawValue }
    var label: String { self == .system ? "システムに従う" : (self == .light ? "ライト" : "ダーク") }
  }

  /// Whether the Tsugumi bird (Home, loading) moves.
  enum Motion: String, CaseIterable, Identifiable {
    case system, on, off
    var id: String { rawValue }
    var label: String {
      switch self {
      case .system: return String(localized: "システムに従う（視差効果を減らす）")
      case .on: return String(localized: "動かす")
      case .off: return String(localized: "動かさない")
      }
    }
  }

  enum SendKey: String, CaseIterable, Identifiable {
    case returnKey, commandReturn
    var id: String { rawValue }
    var label: String { self == .returnKey ? "Return で送信（⇧Return で改行）" : "⌘Return で送信（Return で改行）" }
  }

  var language: Language { didSet { d.set(language.rawValue, forKey: "app.language"); applyLanguage() } }
  var maxConcurrency: Int { didSet { d.set(maxConcurrency, forKey: "run.maxConcurrency") } }
  var sendKey: SendKey { didSet { d.set(sendKey.rawValue, forKey: "chat.sendKey") } }
  var theme: Theme { didSet { d.set(theme.rawValue, forKey: "app.theme"); applyTheme() } }
  var showGrid: Bool { didSet { d.set(showGrid, forKey: "canvas.grid") } }
  var gridSize: Double { didSet { d.set(gridSize, forKey: "canvas.gridSize") } }
  var animateLines: Bool { didSet { d.set(animateLines, forKey: "canvas.animateLines") } }
  var indentWidth: Int { didSet { d.set(indentWidth, forKey: "editor.indent") } }
  var showIndentGuides: Bool { didSet { d.set(showIndentGuides, forKey: "editor.guides") } }
  var editorFontSize: Double { didSet { d.set(editorFontSize, forKey: "editor.fontSize") } }
  var editorTheme: EditorTheme { didSet { d.set(editorTheme.rawValue, forKey: "editor.theme") } }
  var flowDirection: FlowDirection { didSet { d.set(flowDirection.rawValue, forKey: "canvas.flow") } }
  var tsugumiMotion: Motion { didSet { d.set(tsugumiMotion.rawValue, forKey: "app.tsugumiMotion") } }

  /// The bird stays still: as set here, or by default as macOS "Reduce motion" (視差効果を減らす).
  func tsugumiStill(reduceMotion: Bool) -> Bool {
    switch tsugumiMotion {
    case .system: return reduceMotion
    case .on: return false
    case .off: return true
    }
  }
  var shortcuts: [ShortcutAction: KeyBinding] { didSet { saveShortcuts() } }
  var skillFolders: [SkillFolder] { didSet { saveSkills() } }

  init() {
    language = Language(rawValue: d.string(forKey: "app.language") ?? "") ?? .system
    maxConcurrency = d.object(forKey: "run.maxConcurrency") as? Int ?? 4
    sendKey = SendKey(rawValue: d.string(forKey: "chat.sendKey") ?? "") ?? .returnKey
    theme = Theme(rawValue: d.string(forKey: "app.theme") ?? "") ?? .system
    showGrid = d.object(forKey: "canvas.grid") as? Bool ?? true
    gridSize = d.object(forKey: "canvas.gridSize") as? Double ?? 20
    animateLines = d.object(forKey: "canvas.animateLines") as? Bool ?? true
    indentWidth = d.object(forKey: "editor.indent") as? Int ?? 4
    showIndentGuides = d.object(forKey: "editor.guides") as? Bool ?? true
    editorFontSize = d.object(forKey: "editor.fontSize") as? Double ?? 12
    editorTheme = EditorTheme(rawValue: d.string(forKey: "editor.theme") ?? "") ?? .standard
    flowDirection = FlowDirection(rawValue: d.string(forKey: "canvas.flow") ?? "") ?? .topToBottom
    tsugumiMotion = Motion(rawValue: d.string(forKey: "app.tsugumiMotion") ?? "") ?? .system
    if let data = d.data(forKey: "app.shortcuts"), let s = try? JSONDecoder().decode([String: KeyBinding].self, from: data) {
      shortcuts = Dictionary(uniqueKeysWithValues: s.compactMap { k, v in ShortcutAction(rawValue: k).map { ($0, v) } })
    } else {
      shortcuts = [:]
    }
    if let data = d.data(forKey: "agent.skillFolders"), let s = try? JSONDecoder().decode([SkillFolder].self, from: data) {
      skillFolders = s
    } else {
      skillFolders = []
    }
  }

  func shortcut(_ a: ShortcutAction) -> KeyBinding { shortcuts[a] ?? a.defaultKey }

  /// The action already using this key, if any (to warn about duplicates).
  func conflict(_ binding: KeyBinding, except a: ShortcutAction) -> ShortcutAction? {
    ShortcutAction.allCases.first { $0 != a && shortcut($0) == binding }
  }

  private func saveShortcuts() {
    let s = Dictionary(uniqueKeysWithValues: shortcuts.map { ($0.key.rawValue, $0.value) })
    d.set(try? JSONEncoder().encode(s), forKey: "app.shortcuts")
  }

  private func saveSkills() {
    d.set(try? JSONEncoder().encode(skillFolders), forKey: "agent.skillFolders")
  }

  func applyTheme() {
    switch theme {
    case .system: NSApp.appearance = nil
    case .light: NSApp.appearance = NSAppearance(named: .aqua)
    case .dark: NSApp.appearance = NSAppearance(named: .darkAqua)
    }
  }

  /// The language takes effect at the next launch (standard macOS behavior).
  private func applyLanguage() {
    if language == .system { d.removeObject(forKey: "AppleLanguages") } else { d.set([language.rawValue], forKey: "AppleLanguages") }
  }
}

extension View {
  /// Keyboard shortcut taken from the user's Shortcuts settings.
  func shortcut(_ a: ShortcutAction) -> some View {
    let b = AppPreferences.shared.shortcut(a)
    return keyboardShortcut(b.keyEquivalent, modifiers: b.modifiers)
  }
}

/// Colors of the code editor (syntax highlighting).
enum EditorTheme: String, CaseIterable, Identifiable {
  case standard, contrast, muted
  var id: String { rawValue }
  var label: String {
    switch self {
    case .standard: return String(localized: "標準")
    case .contrast: return String(localized: "高コントラスト")
    case .muted: return String(localized: "控えめ")
    }
  }

  func color(_ token: SyntaxToken) -> NSColor {
    switch (self, token) {
    case (_, .plain): return .textColor
    case (.standard, .comment): return .systemGreen
    case (.standard, .string): return .systemRed
    case (.standard, .number): return .systemPurple
    case (.standard, .keyword): return .systemPink
    case (.standard, .function): return .systemBlue
    case (.contrast, .comment): return .systemGray
    case (.contrast, .string): return .systemOrange
    case (.contrast, .number): return .systemYellow
    case (.contrast, .keyword): return .systemTeal
    case (.contrast, .function): return .systemIndigo
    case (.muted, .comment): return .tertiaryLabelColor
    case (.muted, .string): return .systemBrown
    case (.muted, .number): return .secondaryLabelColor
    case (.muted, .keyword): return .labelColor
    case (.muted, .function): return .secondaryLabelColor
    case (.standard, .type): return .systemTeal
    case (.standard, .constant): return .systemOrange
    case (.standard, .property): return .systemCyan
    case (.contrast, .type): return .systemGreen
    case (.contrast, .constant): return .systemPink
    case (.contrast, .property): return .systemBlue
    case (.muted, .type): return .labelColor
    case (.muted, .constant): return .systemBrown
    case (.muted, .property): return .secondaryLabelColor
    }
  }
}
