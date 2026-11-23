import AppKit
import Observation
import SwiftUI
import WebKit

/// The views a Card declares, in its view window: each one hosted by its type.
struct CardViewsPanel: View {
  let session: ProjectSession
  let cardKey: CardKey
  @State private var selected: String?

  private struct Slot: Identifiable {
    let index: Int
    let view: CardViewDeclaration
    var id: String { "\(index)\u{0}\(view.name)\u{0}\(view.type)\u{0}\(view.blob)" }
  }

  var body: some View {
    let relay = session.cardViews
    VStack(alignment: .leading, spacing: 8) {
      ForEach(relay.undeclared(cardKey), id: \.self) { name in
        Label(String(localized: "宣言されていない表示「\(name)」にデータが届きました"), systemImage: "exclamationmark.triangle")
          .font(.caption).foregroundStyle(.red).textSelection(.enabled)
      }
      ForEach(relay.ambiguous(cardKey), id: \.self) { name in
        Label(String(localized: "ビュー名「\(name)」が重複しているため、この名前に送られたデータはどのビューにも届きません"), systemImage: "exclamationmark.triangle")
          .font(.caption).foregroundStyle(.red).textSelection(.enabled)
      }
      if let failure = relay.failures[cardKey] {
        Label(String(localized: "ビューの宣言を評価できませんでした: \(failure)"), systemImage: "exclamationmark.triangle")
          .font(.caption).foregroundStyle(.red).textSelection(.enabled)
          .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
      } else if let views = relay.declarations[cardKey] {
        if views.isEmpty {
          ContentUnavailableView("この Card は表示を宣言していません", systemImage: "rectangle.dashed")
        } else {
          let slots = views.enumerated().map { Slot(index: $0.offset, view: $0.element) }
          let shown = slots.first { $0.id == selected }?.id ?? slots[0].id
          if slots.count > 1 {
            Picker("表示の切り替え", selection: Binding(get: { shown }, set: { selected = $0 })) {
              ForEach(slots) { Text(verbatim: $0.view.name).tag($0.id) }
            }
            .pickerStyle(.segmented).labelsHidden()
          }
          // Every declared view stays mounted, so a hidden one keeps receiving its data.
          ZStack {
            ForEach(slots) { slot in
              CardViewHost(session: session, cardKey: cardKey, index: slot.index, view: slot.view)
                .opacity(slot.id == shown ? 1 : 0)
                .allowsHitTesting(slot.id == shown)
                .accessibilityHidden(slot.id != shown)
            }
          }
        }
      } else {
        ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
      }
    }
  }
}

/// One declared view, routed by its type. The UI only knows `html` and `dawn`.
struct CardViewHost: View {
  let session: ProjectSession
  let cardKey: CardKey
  /// The declaration's place in the views list, which selects its file.
  let index: Int
  let view: CardViewDeclaration

  var body: some View {
    switch view.type {
    case "html":
      HTMLCardView(session: session, cardKey: cardKey, index: index, view: view)
    case "dawn":
      // TODO(open): dawn host
      ContentUnavailableView("Dawn の表示はまだ使えません", systemImage: "cube.transparent",
                             description: Text(verbatim: view.name))
    default:
      ContentUnavailableView(String(localized: "表示の種類「\(view.type)」には対応していません"),
                             systemImage: "exclamationmark.triangle", description: Text(verbatim: view.name))
    }
  }
}

/// Loads the declared HTML file and keeps the host attached to the relay while it is shown.
private struct HTMLCardView: View {
  private struct Selection: Hashable {
    let index: Int
    let view: CardViewDeclaration
  }

  let session: ProjectSession
  let cardKey: CardKey
  let index: Int
  let view: CardViewDeclaration
  @State private var host: HTMLViewHost?
  @State private var error: String?

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      if let host {
        HTMLViewRepresentable(host: host).id(ObjectIdentifier(host))
      } else if error == nil {
        ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
      }
      if let message = error ?? host?.error {
        Text(message).font(.caption).foregroundStyle(.red).textSelection(.enabled)
      }
    }
    .task(id: Selection(index: index, view: view)) { await show() }
  }

  private func show() async {
    let created = HTMLViewHost()
    host = created
    error = nil
    var attachment: UUID?
    do {
      let data = try await session.viewFiles.read(card: path(cardKey), index: index, name: view.name)
      guard let html = String(data: data, encoding: .utf8) else { throw CardViewFileError.notText }
      try Task.checkCancellation()
      try await created.mount(html)
      try Task.checkCancellation()
      attachment = session.cardViews.attach(cardKey, view: view.name) { [weak created] in created?.send($0) }
      while !Task.isCancelled { try await Task.sleep(for: .seconds(3600)) }
    } catch {
      if !Task.isCancelled { self.error = error.localizedDescription }
    }
    if let attachment { session.cardViews.detach(attachment) }
    created.close()
  }
}

private struct HTMLViewRepresentable: NSViewRepresentable {
  let host: HTMLViewHost
  func makeNSView(context: Context) -> WKWebView { host.web }
  func updateNSView(_ web: WKWebView, context: Context) {}
}

/// An isolated WebKit host for one `html` view. The view's document runs in a
/// sandboxed frame with an opaque origin and no network. It receives a
/// MessagePort with `{tsugumi: 1, port}`, then `{type: "data", bytes}` and
/// `{type: "run", state}` on that port, in the order the core sent them.
@MainActor @Observable final class HTMLViewHost: NSObject, WKNavigationDelegate {
  /// Inherited by the view's frame and also placed in its document.
  static let policy = "default-src 'none'; script-src 'unsafe-inline' 'unsafe-eval' 'wasm-unsafe-eval' blob:; style-src 'unsafe-inline' blob: data:; img-src data: blob:; font-src data: blob:; media-src data: blob:; worker-src blob:; connect-src 'none'; frame-src 'none'; form-action 'none'; base-uri 'none'"

  // Independent of the policy: WebKit blocks every load except in-document data.
  private static let resourceRules = """
  [
    {"trigger":{"url-filter":".*"},"action":{"type":"block"}},
    {"trigger":{"url-filter":"^data:"},"action":{"type":"ignore-previous-rules"}},
    {"trigger":{"url-filter":"^blob:"},"action":{"type":"ignore-previous-rules"}},
    {"trigger":{"url-filter":"^about:"},"action":{"type":"ignore-previous-rules"}}
  ]
  """

  private static let page = """
  <!doctype html><html><head><meta charset="utf-8">
  <meta http-equiv="Content-Security-Policy" content="\(policy)">
  <style>html,body{margin:0;height:100%;overflow:hidden}iframe{border:0;display:block;width:100%;height:100%}</style>
  </head><body><script>
  'use strict'
  let frame, port
  window.tsugumiMount = html => new Promise(resolve => {
    port?.close(); frame?.remove()
    frame = document.createElement('iframe')
    frame.sandbox = 'allow-scripts'
    frame.title = 'Card view'
    frame.addEventListener('load', () => {
      const channel = new MessageChannel()
      port = channel.port1
      frame.contentWindow.postMessage({ tsugumi: 1, port: channel.port2 }, '*', [channel.port2])
      resolve(true)
    }, { once: true })
    frame.srcdoc = html
    document.body.append(frame)
  })
  window.tsugumiData = text => {
    const raw = atob(text), bytes = new Uint8Array(raw.length)
    for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i)
    port?.postMessage({ type: 'data', bytes: bytes.buffer }, [bytes.buffer])
  }
  window.tsugumiRun = state => port?.postMessage({ type: 'run', state })
  window.tsugumiClose = () => { port?.close(); frame?.remove(); port = frame = undefined }
  </script></body></html>
  """

  private static var rules: WKContentRuleList?
  private static func compiledRules() async throws -> WKContentRuleList {
    if let rules { return rules }
    let compiled = try await WKContentRuleListStore.default().compileContentRuleList(
      forIdentifier: "TsugumiCardViewIsolation", encodedContentRuleList: resourceRules)
    guard let compiled else { throw CardViewFileError.malformed }
    rules = compiled
    return compiled
  }

  let web: WKWebView
  private(set) var error: String?
  @ObservationIgnored private var outbox = CardViewOutbox()
  @ObservationIgnored private var mounted = false
  @ObservationIgnored private var sending = false
  @ObservationIgnored private var closed = false
  @ObservationIgnored private var loaded: CheckedContinuation<Void, Error>?

  /// `configure` lets tests add their own observers before anything loads.
  init(configure: (WKWebViewConfiguration) -> Void = { _ in }) {
    let configuration = WKWebViewConfiguration()
    configuration.websiteDataStore = .nonPersistent()
    configure(configuration)
    web = WKWebView(frame: .zero, configuration: configuration)
    web.allowsLinkPreview = false
    super.init()
    web.navigationDelegate = self
  }

  /// Loads the host page and the view's document; returns once the port is handed over.
  func mount(_ html: String) async throws {
    let rules = try await Self.compiledRules()
    guard !closed else { throw CancellationError() }
    web.configuration.userContentController.add(rules)
    try await withCheckedThrowingContinuation { continuation in
      loaded = continuation
      web.loadHTMLString(Self.page, baseURL: nil)
    }
    let document = ArtifactHTMLPreview.document(html, policy: Self.policy)
    _ = try await web.callAsyncJavaScript("return await tsugumiMount(html)", arguments: ["html": document], contentWorld: .page)
    mounted = true
    flush()
  }

  func send(_ message: CardViewMessage) {
    guard !closed else { return }
    outbox.put(message)
    flush()
  }

  /// The view goes away with its port; nothing more is delivered.
  func close() {
    guard !closed else { return }
    closed = true
    outbox = CardViewOutbox()
    loaded?.resume(throwing: CancellationError())
    loaded = nil
    web.evaluateJavaScript("tsugumiClose()", completionHandler: nil)
    web.stopLoading()
    web.navigationDelegate = nil
  }

  private func flush() {
    guard mounted, !sending, !closed, let message = outbox.take() else { return }
    sending = true
    Task {
      defer { sending = false; flush() }
      do {
        switch message {
        case .data(let bytes):
          _ = try await web.callAsyncJavaScript("tsugumiData(text)", arguments: ["text": bytes.base64EncodedString()], contentWorld: .page)
        case .run(let started):
          _ = try await web.callAsyncJavaScript("tsugumiRun(state)", arguments: ["state": started ? "started" : "ended"], contentWorld: .page)
        }
      } catch {
        if !closed { self.error = error.localizedDescription }
      }
    }
  }

  func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
    loaded?.resume()
    loaded = nil
  }

  func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
    loaded?.resume(throwing: error)
    loaded = nil
  }

  func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
    loaded?.resume(throwing: error)
    loaded = nil
  }

  /// Only the host page and the view's srcdoc document load; nothing navigates away.
  func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
               decisionHandler: @escaping @MainActor (WKNavigationActionPolicy) -> Void) {
    decisionHandler(action.navigationType == .other && action.request.url?.scheme == "about" ? .allow : .cancel)
  }
}
