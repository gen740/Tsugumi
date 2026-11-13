import AppKit
import SwiftUI
import WebKit
import TsugumiRPC

/// Immutable ES modules fetched through RPC, shared by interactive views and previews.
@MainActor enum ModuleSources {
  private final class Connection {
    weak var client: RPCClient?
    init(_ client: RPCClient) { self.client = client }
  }
  private static var connections: [Connection] = []
  private static var cache: [String: Task<String, Error>] = [:]
  static func register(_ client: RPCClient) {
    connections.removeAll { $0.client == nil }
    connections.append(Connection(client))
  }
  static func source(_ identity: String) async throws -> String {
    if let task = cache[identity] { return try await task.value }
    let task = Task { @MainActor () throws -> String in
      var last: Error = NSError(domain: "Module", code: 1)
      for connection in connections.reversed() {
        guard let client = connection.client else { continue }
        do {
          var data = Data(), offset: UInt64 = 0
          while true {
            let request = Tsugumi_RPC_ModuleReadRequestT()
            request.module = identity; request.offset = offset; request.limitBytes = 1 << 20
            let page: Tsugumi_RPC_FileContentT = try await client.call(.modulereadrequest, request)
            guard page.offset == offset, data.count + page.bytes.count <= 32 << 20 else { throw last }
            data.append(contentsOf: page.bytes)
            if page.eof == true { break }
            guard let next = page.nextOffset, next > offset else { throw last }
            offset = next
          }
          guard let code = String(data: data, encoding: .utf8) else { throw last }
          return code
        } catch { last = error }
      }
      throw last
    }
    if cache.count >= 8, let first = cache.keys.first { cache[first] = nil }
    cache[identity] = task
    do { return try await task.value } catch { cache[identity] = nil; throw error }
  }
}

/// Card code may not navigate to a network or local-file URL, including in subframes.
@MainActor final class ModuleNavigation: NSObject, WKNavigationDelegate {
  static let shared = ModuleNavigation()
  private var hostLoads = Set<ObjectIdentifier>()
  func prepare(_ web: WKWebView) { hostLoads.insert(ObjectIdentifier(web)); web.navigationDelegate = self }
  func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
               decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
    let initial = action.targetFrame?.isMainFrame != false && action.request.url?.absoluteString == "https://localhost/"
      && hostLoads.remove(ObjectIdentifier(webView)) != nil
    decisionHandler(initial || action.request.url?.scheme == "about" ? .allow : .cancel)
  }
}

struct ModuleViewer: View {
  let name: String
  let json: String
  var blobs: [String: Data] = [:]
  let interactive: Bool
  var zoom: CGFloat = 1
  @State private var error = ""
  var body: some View {
    if let dir = ModuleViewerHost.directory {
      ZStack(alignment: .bottomLeading) {
        ModuleViewerHost(directory: dir, name: name, json: json, blobs: blobs, interactive: interactive, zoom: zoom, error: $error)
          .allowsHitTesting(interactive).clipShape(RoundedRectangle(cornerRadius: 4))
        if !error.isEmpty { Text(error).font(.caption).foregroundStyle(.red).padding(4).textSelection(.enabled) }
      }
    } else {
      Text("このビルドにはグラフの表示部品（modules）がありません").font(.caption).foregroundStyle(.secondary)
    }
  }
}

struct ModuleViewerHost: NSViewRepresentable {
  static let directory: URL? = {
    if let url = Bundle.main.url(forResource: "modules", withExtension: nil) { return url }
    #if DEBUG
    var repo = URL(fileURLWithPath: #filePath)
    for _ in 0..<5 { repo.deleteLastPathComponent() }
    let dev = repo.appendingPathComponent("ui/modules")
    if FileManager.default.fileExists(atPath: dev.appendingPathComponent("viewer.html").path) { return dev }
    #endif
    return nil
  }()

  /// No file URL or store read access is granted to WebKit.
  static func page(_ directory: URL) throws -> String {
    let script = try String(contentsOf: directory.appendingPathComponent("viewer.js"), encoding: .utf8)
    let page = try String(contentsOf: directory.appendingPathComponent("viewer.html"), encoding: .utf8)
    return page.replacingOccurrences(of: "<script src=\"viewer.js\"></script>", with: "<script>\(script)</script>")
  }

  let directory: URL
  let name: String
  let json: String
  let blobs: [String: Data]
  let interactive: Bool
  let zoom: CGFloat
  @Binding var error: String

  func makeCoordinator() -> Coordinator { Coordinator(error: $error) }
  func makeNSView(context: Context) -> WKWebView {
    let config = WKWebViewConfiguration()
    config.websiteDataStore = .nonPersistent()
    config.userContentController.add(context.coordinator, name: "tsugumi")
    let web = WKWebView(frame: .zero, configuration: config)
    ModuleNavigation.shared.prepare(web)
    web.underPageBackgroundColor = .white
    context.coordinator.web = web
    Coordinator.withBlockedNetwork { rules in
      guard let rules, let page = try? Self.page(directory) else {
        context.coordinator.error.wrappedValue = "Cannot initialise isolated module viewer"
        return
      }
      config.userContentController.add(rules)
      web.loadHTMLString(page, baseURL: URL(string: "https://localhost/"))
    }
    return web
  }
  func updateNSView(_ web: WKWebView, context: Context) {
    if abs(web.pageZoom - zoom) > 0.001 { web.pageZoom = max(zoom, 0.05) }
    context.coordinator.error = $error
    context.coordinator.show(name: name, json: json, blobs: blobs, interactive: interactive)
  }
  static func dismantleNSView(_ web: WKWebView, coordinator: Coordinator) {
    coordinator.loading?.cancel()
    web.evaluateJavaScript("document.querySelector('iframe')?.remove()", completionHandler: nil)
    web.configuration.userContentController.removeScriptMessageHandler(forName: "tsugumi")
  }

  @MainActor final class Coordinator: NSObject, WKScriptMessageHandler {
    weak var web: WKWebView?
    var error: Binding<String>
    var loading: Task<Void, Never>?
    private var ready = false
    private var pending: [String: Any]?
    private var shown: (String, String, [String: Data], Bool)?
    init(error: Binding<String>) { self.error = error }

    private static var rules: WKContentRuleList??
    private static var waiting: [(WKContentRuleList?) -> Void] = []
    static func withBlockedNetwork(_ then: @escaping (WKContentRuleList?) -> Void) {
      if let rules { return then(rules) }
      waiting.append(then)
      guard waiting.count == 1 else { return }
      // Block subresources as well as navigations; failure to compile is fatal.
      let list = #"[{"trigger":{"url-filter":"^(https?|wss?|ftp|file):"},"action":{"type":"block"}}]"#
      WKContentRuleListStore.default().compileContentRuleList(forIdentifier: "tsugumi-module-isolation", encodedContentRuleList: list) { compiled, _ in
        Task { @MainActor in
          rules = .some(compiled)
          let all = waiting; waiting = []
          for f in all { f(compiled) }
        }
      }
    }
    func show(name: String, json: String, blobs: [String: Data], interactive: Bool) {
      if let shown, shown.0 == name, shown.1 == json, shown.2 == blobs, shown.3 == interactive { return }
      shown = (name, json, blobs, interactive)
      loading?.cancel()
      loading = Task {
        do {
          let code = try await ModuleSources.source(name)
          guard !Task.isCancelled else { return }
          pending = ["name": name, "json": json, "blobs": blobs.mapValues { $0.base64EncodedString() },
                     "code": code, "interactive": interactive, "visible": true]
          flush()
        } catch { if !Task.isCancelled { self.error.wrappedValue = error.localizedDescription } }
      }
    }
    private func flush() {
      guard ready, let message = pending, let web else { return }
      pending = nil
      Task {
        _ = try? await web.callAsyncJavaScript("return await tsugumiRender(message)", arguments: ["message": message], contentWorld: .page)
      }
    }
    func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
      guard message.frameInfo.isMainFrame, let body = message.body as? [String: Any] else { return }
      if body["tsugumi"] as? String == "ready" { ready = true; flush() }
      else if body["tsugumi"] as? String == "state" {
        error.wrappedValue = body["state"] as? String == "error" ? String((body["message"] as? String ?? "").prefix(4096)) : ""
      }
    }
  }
}
