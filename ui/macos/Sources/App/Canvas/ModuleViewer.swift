import AppKit
import SwiftUI
import WebKit
import TsugumiRPC

@MainActor enum ModuleSources {
  private static var cache: [String: Task<String, Error>] = [:]
  static func source(_ identity: String) async throws -> String {
    if identity.hasPrefix("tsugumi:") {
      let name = String(identity.dropFirst("tsugumi:".count))
      guard ["text", "markdown", "scalar", "table", "image", "points", "line-chart", "plotly", "vega-lite"].contains(name),
            let directory = ModuleViewerHost.directory else { throw CocoaError(.fileNoSuchFile) }
      if let task = cache[identity] { return try await task.value }
      let file = directory.appendingPathComponent(name + ".js")
      let task = Task.detached { try String(contentsOf: file, encoding: .utf8) }
      cache[identity] = task
      do { return try await task.value } catch { cache[identity] = nil; throw error }
    }

    throw CocoaError(.fileNoSuchFile)
  }
}

struct DisplayView: View {
  let display: DisplaySnapshot?
  var emptyNote: String?

  var body: some View {
    if let module = display?.module {
      ModuleViewer(module: module).id(module.name)
    } else if display != nil {
      ContentUnavailableView("この結果は表示できません", systemImage: "chart.xyaxis.line")
    } else {
      ContentUnavailableView("まだ結果がありません", systemImage: "chart.xyaxis.line",
                             description: Text(emptyNote ?? String(localized: "実行すると表示されます。")))
    }
  }
}

private struct ModuleViewer: View {
  let module: DisplayModule
  @Environment(\.colorScheme) private var colorScheme
  @State private var error = ""

  var body: some View {
    if let directory = ModuleViewerHost.directory {
      VStack(alignment: .leading, spacing: 4) {
        ModuleViewerHost(directory: directory, module: module, colorScheme: colorScheme == .dark ? "dark" : "light", error: $error)
        if !error.isEmpty { Text(error).font(.caption).foregroundStyle(.red).textSelection(.enabled) }
      }
    } else {
      Text("このビルドにはグラフの表示部品（modules）がありません").font(.caption).foregroundStyle(.secondary)
    }
  }
}

private struct ModuleViewerHost: NSViewRepresentable {
  static let directory: URL? = {
    if let url = Bundle.main.resourceURL?.appendingPathComponent("web/modules"),
       FileManager.default.fileExists(atPath: url.appendingPathComponent("viewer.html").path) { return url }
    #if DEBUG
    var repo = URL(fileURLWithPath: #filePath)
    for _ in 0..<6 { repo.deleteLastPathComponent() }
    let dev = repo.appendingPathComponent("ui/web/dist/public/modules")
    if FileManager.default.fileExists(atPath: dev.appendingPathComponent("viewer.html").path) { return dev }
    #endif
    return nil
  }()

  let directory: URL
  let module: DisplayModule
  let colorScheme: String
  @Binding var error: String

  func makeCoordinator() -> Coordinator { Coordinator(error: $error) }

  func makeNSView(context: Context) -> WKWebView {
    let coordinator = context.coordinator
    let config = WKWebViewConfiguration()
    config.websiteDataStore = .nonPersistent()
    config.userContentController.add(coordinator, name: "tsugumi")
    let web = WKWebView(frame: .zero, configuration: config)
    web.navigationDelegate = coordinator
    coordinator.web = web
    Coordinator.withBlockedNetwork { [weak web, weak coordinator] rules in
      guard let web, let coordinator, coordinator.web != nil else { return }
      guard let rules, let page = try? String(contentsOf: directory.appendingPathComponent("viewer.html"), encoding: .utf8) else {
        coordinator.error.wrappedValue = "Cannot initialise isolated module viewer"
        return
      }
      config.userContentController.add(rules)
      web.loadHTMLString(page, baseURL: URL(string: "https://localhost/"))
    }
    return web
  }

  func updateNSView(_ web: WKWebView, context: Context) {
    context.coordinator.error = $error
    context.coordinator.show(module, colorScheme: colorScheme)
  }

  static func dismantleNSView(_ web: WKWebView, coordinator: Coordinator) {
    coordinator.web = nil
    coordinator.loading?.cancel()
    web.stopLoading()
    web.evaluateJavaScript("document.querySelector('iframe')?.remove()", completionHandler: nil)
    web.configuration.userContentController.removeScriptMessageHandler(forName: "tsugumi")
  }

  @MainActor final class Coordinator: NSObject, WKScriptMessageHandler, WKNavigationDelegate {
    weak var web: WKWebView?
    var error: Binding<String>
    var loading: Task<Void, Never>?
    private var initial = true
    private var ready = false
    private var source: String?
    private var latest: (DisplayModule, String)?
    private var sent: (DisplayModule, String)?
    private var sending = false

    init(error: Binding<String>) { self.error = error }

    private static var rules: WKContentRuleList??
    private static var waiting: [(WKContentRuleList?) -> Void] = []
    static func withBlockedNetwork(_ then: @escaping (WKContentRuleList?) -> Void) {
      if let rules { return then(rules) }
      waiting.append(then)
      guard waiting.count == 1 else { return }
      let list = #"[{"trigger":{"url-filter":"^(https?|wss?|ftp|file):"},"action":{"type":"block"}}]"#
      WKContentRuleListStore.default().compileContentRuleList(forIdentifier: "tsugumi-module-isolation", encodedContentRuleList: list) { compiled, _ in
        Task { @MainActor in
          rules = .some(compiled)
          let all = waiting; waiting = []
          for f in all { f(compiled) }
        }
      }
    }

    func show(_ module: DisplayModule, colorScheme: String) {
      if let current = latest ?? sent, current.0 == module && current.1 == colorScheme { return }
      latest = (module, colorScheme)
      if source != nil { flush(); return }
      guard loading == nil else { return }
      loading = Task {
        defer { loading = nil }
        do {
          let code = try await ModuleSources.source(module.name)
          guard !Task.isCancelled else { return }
          source = code
          flush()
        } catch { if !Task.isCancelled { self.error.wrappedValue = error.localizedDescription } }
      }
    }

    private func flush() {
      guard ready, !sending, let update = latest, let source, let web else { return }
      latest = nil
      var message: [String: Any] = ["name": update.0.name, "colorScheme": update.1]
      if sent == nil { message["code"] = source }
      if sent?.0 != update.0 {
        message["config"] = update.0.config
        message["blobs"] = update.0.blobs.mapValues { $0.base64EncodedString() }
      }
      sent = update
      sending = true
      Task {
        defer { sending = false; flush() }
        do {
          _ = try await web.callAsyncJavaScript("tsugumiRender(message)", arguments: ["message": message], contentWorld: .page)
        } catch {
          if self.web != nil { self.error.wrappedValue = error.localizedDescription }
        }
      }
    }

    func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
      guard message.frameInfo.isMainFrame, let body = message.body as? [String: Any] else { return }
      switch body["tsugumi"] as? String {
      case "ready": ready = true; flush()
      case "state":
        error.wrappedValue = body["state"] as? String == "error" ? String((body["message"] as? String ?? "").prefix(4096)) : ""
      default: break
      }
    }

    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
      let hostLoad = initial && action.targetFrame?.isMainFrame != false && action.request.url?.absoluteString == "https://localhost/"
      if hostLoad { initial = false }
      decisionHandler(hostLoad || action.request.url?.scheme == "about" ? .allow : .cancel)
    }
  }
}
