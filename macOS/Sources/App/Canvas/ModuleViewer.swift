import AppKit
import SwiftUI
import WebKit

/// A Plotly figure or a Vega-Lite chart (a module display), drawn by the
/// module viewer (`ui/modules`, with the bundled plotly.js / Vega) in a web
/// view that reaches nothing but its own files: a Card's plot is untrusted.
/// Interactive (zoom, hover, legend) in the expanded view; on the canvas a
/// picture of it is shown instead (ModuleThumbnail).
struct ModuleViewer: View {
  static let modules: Set<String> = ["plotly", "vega-lite"]

  let name: String
  let json: String
  let interactive: Bool
  /// The canvas's zoom: the plot is laid out at the display's own size and scaled with it.
  var zoom: CGFloat = 1
  @State private var error = ""

  var body: some View {
    if let dir = ModuleViewerHost.directory {
      ZStack(alignment: .bottomLeading) {
        ModuleViewerHost(directory: dir, name: name, json: json, interactive: interactive, zoom: zoom, error: $error)
          .allowsHitTesting(interactive)
          .clipShape(RoundedRectangle(cornerRadius: 4))
        if !error.isEmpty {
          Text(error).font(.caption).foregroundStyle(.red).padding(4).textSelection(.enabled)
        }
      }
      .accessibilityLabel(name == "plotly" ? String(localized: "Plotly のグラフ") : String(localized: "Vega-Lite のグラフ"))
    } else {
      Text("このビルドにはグラフの表示部品（modules）がありません").font(.caption).foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
  }
}

struct ModuleViewerHost: NSViewRepresentable {
  /// The viewer and its libraries: Contents/Resources/modules (build-app.sh),
  /// or the web build's copy in a development checkout.
  static let directory: URL? = {
    if let url = Bundle.main.url(forResource: "modules", withExtension: nil) { return url }
    #if DEBUG
    var repo = URL(fileURLWithPath: #filePath)
    for _ in 0..<5 { repo.deleteLastPathComponent() }
    let dev = repo.appendingPathComponent("web/dist/public/modules")
    if FileManager.default.fileExists(atPath: dev.appendingPathComponent("viewer.html").path) { return dev }
    #endif
    return nil
  }()

  let directory: URL
  let name: String
  let json: String
  let interactive: Bool
  let zoom: CGFloat
  @Binding var error: String

  func makeCoordinator() -> Coordinator { Coordinator(error: $error) }

  func makeNSView(context: Context) -> WKWebView {
    let config = WKWebViewConfiguration()
    config.websiteDataStore = .nonPersistent()
    config.userContentController.add(context.coordinator, name: "tsugumi")
    let web = WKWebView(frame: .zero, configuration: config)
    web.underPageBackgroundColor = .white
    context.coordinator.web = web
    let page = directory.appendingPathComponent("viewer.html")
    let dir = directory
    Coordinator.withBlockedNetwork { rules in
      if let rules { config.userContentController.add(rules) }
      web.loadFileURL(page, allowingReadAccessTo: dir)
    }
    return web
  }

  func updateNSView(_ web: WKWebView, context: Context) {
    if abs(web.pageZoom - zoom) > 0.001 { web.pageZoom = max(zoom, 0.05) }
    context.coordinator.error = $error
    context.coordinator.show(name: name, json: json, interactive: interactive)
  }

  static func dismantleNSView(_ web: WKWebView, coordinator: Coordinator) {
    web.configuration.userContentController.removeScriptMessageHandler(forName: "tsugumi")
  }

  @MainActor
  final class Coordinator: NSObject, WKScriptMessageHandler {
    weak var web: WKWebView?
    var error: Binding<String>
    private var ready = false
    private var pending: String?
    private var shown: String?

    init(error: Binding<String>) { self.error = error }

    /// Every load but the viewer's own files is blocked (compiled once).
    private static var rules: WKContentRuleList??
    private static var waiting: [(WKContentRuleList?) -> Void] = []
    static func withBlockedNetwork(_ then: @escaping (WKContentRuleList?) -> Void) {
      if let rules { return then(rules) }
      waiting.append(then)
      guard waiting.count == 1 else { return }
      let list = #"[{"trigger":{"url-filter":".*"},"action":{"type":"block"}},"# +
        #"{"trigger":{"url-filter":"^file:"},"action":{"type":"ignore-previous-rules"}}]"#
      WKContentRuleListStore.default().compileContentRuleList(forIdentifier: "tsugumi-module-viewer", encodedContentRuleList: list) { compiled, _ in
        Task { @MainActor in
          rules = .some(compiled)
          let all = waiting
          waiting = []
          for f in all { f(compiled) }
        }
      }
    }

    func show(name: String, json: String, interactive: Bool) {
      let message: [String: Any] = ["name": name, "json": json, "interactive": interactive]
      guard let data = try? JSONSerialization.data(withJSONObject: message),
            let text = String(data: data, encoding: .utf8), text != shown else { return }
      pending = text
      flush()
    }

    private func flush() {
      guard ready, let text = pending, let web else { return }
      pending = nil
      shown = text
      web.evaluateJavaScript("tsugumiRender(\(text))", completionHandler: nil)
    }

    func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
      guard let body = message.body as? [String: Any] else { return }
      if body["tsugumi"] as? String == "ready" {
        ready = true
        flush()
      } else if body["tsugumi"] as? String == "state" {
        error.wrappedValue = body["state"] as? String == "error" ? (body["message"] as? String ?? "") : ""
      }
    }
  }
}
