import AppKit
import Observation
import SwiftUI
import TsugumiRPC

/// Commands whose keys can be reassigned in Settings > Shortcuts.
enum ShortcutAction: String, CaseIterable, Identifiable, Codable {
  case save, run, stop, toggleChat, toggleLog, fitAll, arrange, addNote, duplicate, openSettings
  var id: String { rawValue }

  var title: String {
    switch self {
    case .save: return String(localized: "保存")
    case .run: return String(localized: "実行")
    case .stop: return String(localized: "停止")
    case .toggleChat: return String(localized: "チャットの表示切替")
    case .toggleLog: return String(localized: "ログの表示切替")
    case .fitAll: return String(localized: "全体表示")
    case .arrange: return String(localized: "整列")
    case .addNote: return String(localized: "付箋を追加")
    case .duplicate: return String(localized: "複製")
    case .openSettings: return String(localized: "Card 設定を開く")
    }
  }

  var defaultKey: KeyBinding {
    switch self {
    case .save: return KeyBinding(key: "s", command: true)
    case .run: return KeyBinding(key: "r", command: true)
    case .stop: return KeyBinding(key: ".", command: true)
    case .toggleChat: return KeyBinding(key: "j", command: true, option: true)
    case .toggleLog: return KeyBinding(key: "l", command: true, option: true)
    case .fitAll: return KeyBinding(key: "0", command: true, option: true)
    case .arrange: return KeyBinding(key: "a", command: true, option: true, shift: true)
    case .addNote: return KeyBinding(key: "n", command: true, option: true)
    case .duplicate: return KeyBinding(key: "d", command: true)
    case .openSettings: return KeyBinding(key: "i", command: true)
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

  var keyEquivalent: KeyEquivalent { KeyEquivalent(key.first ?? " ") }

  var label: String {
    (control ? "⌃" : "") + (option ? "⌥" : "") + (shift ? "⇧" : "") + (command ? "⌘" : "") + key.uppercased()
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
    }
  }
}
