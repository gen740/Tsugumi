import AppKit

/// The displayed language is fixed for this process; host settings may change
/// locally or arrive in a new host snapshot. Returning to it cancels the offer.
struct LanguageRestart {
  let running: AppPreferences.Language
  private(set) var selected: AppPreferences.Language
  var pending: Bool { selected != running }

  init(running: AppPreferences.Language) {
    self.running = running
    selected = running
  }

  @discardableResult mutating func select(_ next: AppPreferences.Language) -> Bool {
    guard selected != next else { return false }
    selected = next
    return true
  }
}

/// Resolve this notice in the selected language, while the rest of the UI
/// continues to use the bundle localization chosen at launch.
struct LanguageRestartText {
  let bundle: Bundle
  init(language: AppPreferences.Language, resources: Bundle = .main) {
    bundle = resources.path(forResource: language.rawValue, ofType: "lproj").flatMap(Bundle.init(path:)) ?? resources
  }
  var title: String { String(localized: "表示言語を変えるには再起動が必要です", bundle: bundle) }
  var message: String {
    String(localized: "表示言語は再起動後に変わります。これからプロジェクトに書く記録には、すでに新しい言語が使われます。再起動すると実行中の処理は停止します。", bundle: bundle)
  }
  var now: String { String(localized: "今すぐ再起動", bundle: bundle) }
  var later: String { String(localized: "あとで", bundle: bundle) }
}

@MainActor enum LanguageRestartNotice {
  private static var shown: NSAlert?

  static func restart() {
    Task {
      // The host owns the setting. Quitting while its asynchronous write
      // is in flight can bring the old value back at the next launch.
      await CorePreferences.shared.finishEditing()
      if AppPreferences.shared.languageRestart.pending { Updater.shared.restart() }
    }
  }

  static func update(_ state: LanguageRestart) {
    if let shown, let window = shown.window.sheetParent {
      window.endSheet(shown.window, returnCode: .alertSecondButtonReturn)
    }
    shown = nil
    guard state.pending, let window = NSApp?.keyWindow ?? NSApp?.mainWindow else { return }
    let text = LanguageRestartText(language: state.selected)
    let alert = NSAlert()
    alert.messageText = text.title
    alert.informativeText = text.message
    alert.addButton(withTitle: text.now)
    alert.addButton(withTitle: text.later)
    shown = alert
    alert.beginSheetModal(for: window) { response in
      if shown === alert { shown = nil }
      if response == .alertFirstButtonReturn { restart() }
    }
  }
}
