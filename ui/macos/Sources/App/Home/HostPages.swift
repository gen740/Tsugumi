import Darwin
import Foundation
import TsugumiAgentClient
import TsugumiLauncher

/// Each main window is a page of the host (HostPageRequest), as a Web page
/// is: hostd gives it the projects the person or an Agent shows (ShowRequest)
/// when it is on Home, tells the window that shows one already to come
/// forward, and names a Card to reveal (PageShow). The windows share the app's
/// one Host connection; each names its page with its Workspace id.
@MainActor
final class HostPages {
  static let shared = HostPages()

  /// What hostd was told each window shows ("": Home).
  private var reported: [UUID: String] = [:]
  private var observing = false
  /// Reports go out in the order they were made.
  private var last: Task<Void, Never>?

  /// The window shows another project, or Home.
  func report(_ w: Workspace) {
    let project = w.session.map { Self.hostPath($0.identity.url) } ?? ""
    guard reported[w.id] != project else { return }
    reported[w.id] = project
    observe()
    send(w.id, project: project, closed: false)
  }

  /// The window closed: hostd forgets its page (and tells the pages when no
  /// other shows its project, PageClosed).
  func closed(_ w: Workspace) {
    guard reported.removeValue(forKey: w.id) != nil else { return }
    send(w.id, project: "", closed: true)
  }

  private func send(_ page: UUID, project: String, closed: Bool) {
    let previous = last
    last = Task {
      await previous?.value
      // A lost connection is reported again once it is back (observe).
      try? await CoreHostClient.shared.reportPage(page.uuidString, project: project.isEmpty ? nil : project, closed: closed)
    }
  }

  private func observe() {
    guard !observing else { return }
    observing = true
    Task {
      _ = await CoreHostClient.shared.observeHost { event in
        let type = event["type"] as? String
        let page = event["page"] as? String ?? "", project = event["project"] as? String ?? ""
        let card = event["card"] as? String ?? ""
        Task { @MainActor in HostPages.shared.event(type, page: page, project: project, card: card) }
      }
    }
  }

  private func event(_ type: String?, page: String, project: String, card: String) {
    switch type {
    case "Connected":
      // A new host (or connection) knows no page yet.
      for (page, project) in reported { send(page, project: project, closed: false) }
    case "PageShow":
      guard let w = AppModel.shared.mainWindows.first(where: { $0.id.uuidString == page }) else { return }
      show(project, card: card, in: w)
    default: break
    }
  }

  /// Opens the project in the window when it is on Home (hostd chose it), and
  /// brings the window forward with the Card in view.
  private func show(_ project: String, card: String, in w: Workspace) {
    let url = URL(fileURLWithPath: project)
    let id = try? ProjectIdentity(url: url).id
    if w.session == nil {
      AppModel.shared.show(url, in: w)
      report(w)
    } else if w.session?.identity.id != id {
      // The window took another project meanwhile: hostd learns it from the report.
      return
    }
    if !card.isEmpty { w.revealCard = card }
    w.revealPendingCard()
    w.bringForward()
  }

  /// The path as hostd names a project (realpath: /private/var, not /var).
  static func hostPath(_ url: URL) -> String {
    guard let real = realpath(url.path, nil) else { return url.path }
    defer { free(real) }
    return String(cString: real)
  }
}

extension Workspace {
  /// Shows the Card asked for once the project has it (it may still be loading).
  func revealPendingCard() {
    guard let card = revealCard, let s = session, s.instance(card) != nil else { return }
    revealCard = nil
    let path = card.split(separator: "/").map(String.init)
    openTab(Array(path.dropLast()))
    guard let tab = active else { return }
    tab.selection = [card]
    DispatchQueue.main.async { NotificationCenter.default.post(name: .canvasReveal, object: tab.id, userInfo: ["card": card]) }
  }
}
