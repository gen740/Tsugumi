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
    let created = HTMLViewHost(sendToCard: { bytes in
      guard let rpc = session.rpc else { throw CardViewFileError.unavailable }
      try await rpc.sendView(card: path(cardKey), view: view.name, bytes: bytes)
    })
    host = created
    error = nil
    var attachment: UUID?
    do {
      let data = try await session.viewFiles.read(card: path(cardKey), index: index, name: view.name)
      guard let html = String(data: data, encoding: .utf8) else { throw CardViewFileError.notText }
      try Task.checkCancellation()
      try await created.mount(html)
      try Task.checkCancellation()
      attachment = session.cardViews.attach(cardKey, view: view.name) { [weak created] message, complete in
        if let created { created.send(message, complete: complete) }
        else { complete() }
      }
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
/// MessagePort with `{tsugumi: 1, port}`, then `{type: "data", bytes}`
/// on that port, in the order the core sent them.
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
  let frame, port, acknowledge, reverseBytes = 0
  const max = 1 << 24
  const credit = `<script>
    addEventListener('message', event => {
      if (event.source !== parent || event.data?.tsugumi !== 1) return
      const port = event.data.port
      port.addEventListener('message', message => {
        if (message.data?.type === 'data')
          setTimeout(() => port.postMessage({ type: 'credit' }), 0)
      })
      port.start()
    }, { once: true })
  </scr` + `ipt>`
  window.tsugumiMount = html => new Promise(resolve => {
    port?.close(); frame?.remove()
    frame = document.createElement('iframe')
    frame.sandbox = 'allow-scripts'
    frame.title = 'Card view'
    frame.addEventListener('load', () => {
      const channel = new MessageChannel()
      port = channel.port1
      port.onmessage = ({ data }) => {
        if (data?.type === 'credit') {
          acknowledge?.()
          acknowledge = undefined
        } else if (data?.type === 'data' && data.bytes instanceof ArrayBuffer) {
          const bytes = new Uint8Array(data.bytes)
          if (bytes.length > max || reverseBytes > max) {
            webkit.messageHandlers.tsugumiViewInput.postMessage({ dropped: true })
            return
          }
          const parts = []
          for (let i = 0; i < bytes.length; i += 16384)
            parts.push(String.fromCharCode(...bytes.subarray(i, i + 16384)))
          reverseBytes += Math.max(1, bytes.length)
          webkit.messageHandlers.tsugumiViewInput.postMessage({ bytes: btoa(parts.join('')) })
        }
      }
      frame.contentWindow.postMessage({ tsugumi: 1, port: channel.port2 }, '*', [channel.port2])
      resolve(true)
    }, { once: true })
    // Insert before Card scripts, after the isolation policy.
    const end = html.indexOf('>', html.indexOf('<meta')) + 1
    frame.srcdoc = html.slice(0, end) + credit + html.slice(end)
    document.body.append(frame)
  })
  window.tsugumiData = text => new Promise(resolve => {
    if (!port) return resolve()
    const raw = atob(text), bytes = new Uint8Array(raw.length)
    for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i)
    acknowledge = resolve
    port.postMessage({ type: 'data', bytes: bytes.buffer }, [bytes.buffer])
  })
  window.tsugumiReverseCredit = size => { reverseBytes -= Math.max(1, size) }
  window.tsugumiClose = () => {
    acknowledge?.()
    port?.close(); frame?.remove(); port = frame = acknowledge = undefined
  }
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

  @MainActor private final class InputHandler: NSObject, WKScriptMessageHandler {
    weak var host: HTMLViewHost?

    func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
      if message.frameInfo.isMainFrame { host?.receive(message.body) }
    }
  }

  let web: WKWebView
  @ObservationIgnored private let sendToCard: ((Data) async throws -> Void)?
  @ObservationIgnored private var reverse: [Data] = []
  @ObservationIgnored private var sendingReverse = false
  private(set) var error: String?
  @ObservationIgnored private var outbox = CardViewOutbox()
  @ObservationIgnored private var mounted = false
  @ObservationIgnored private var sending = false
  @ObservationIgnored private var closed = false
  @ObservationIgnored private var loaded: CheckedContinuation<Void, Error>?

  /// `configure` lets tests add their own observers before anything loads.
  init(configure: (WKWebViewConfiguration) -> Void = { _ in },
       sendToCard: ((Data) async throws -> Void)? = nil) {
    self.sendToCard = sendToCard
    let configuration = WKWebViewConfiguration()
    configuration.websiteDataStore = .nonPersistent()
    configure(configuration)
    web = WKWebView(frame: .zero, configuration: configuration)
    web.allowsLinkPreview = false
    super.init()
    let input = InputHandler()
    input.host = self
    web.configuration.userContentController.add(input, name: "tsugumiViewInput")
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

  func send(_ message: CardViewMessage, complete: @escaping () -> Void = {}) {
    guard !closed else { complete(); return }
    outbox.put(message, complete: complete)
    flush()
  }

  /// The view goes away with its port; nothing more is delivered.
  func close() {
    guard !closed else { return }
    closed = true
    outbox.discard()
    reverse.removeAll()
    web.configuration.userContentController.removeScriptMessageHandler(forName: "tsugumiViewInput")
    loaded?.resume(throwing: CancellationError())
    loaded = nil
    web.evaluateJavaScript("tsugumiClose()", completionHandler: nil)
    web.stopLoading()
    web.navigationDelegate = nil
  }

  private func flush() {
    guard mounted, !sending, !closed, let (message, complete) = outbox.take() else { return }
    sending = true
    Task {
      defer { complete(); sending = false; flush() }
      do {
        switch message {
        case .data(let bytes):
          _ = try await web.callAsyncJavaScript("return await tsugumiData(text)", arguments: ["text": bytes.base64EncodedString()], contentWorld: .page)
        }
      } catch {
        if !closed { self.error = error.localizedDescription }
      }
    }
  }

  private func receive(_ body: Any) {
    guard !closed, let message = body as? [String: Any] else { return }
    if message["dropped"] as? Bool == true {
      error = String(localized: "ビューのメッセージを送信できませんでした")
    } else if let text = message["bytes"] as? String, let bytes = Data(base64Encoded: text),
              bytes.count <= 1 << 24 {
      reverse.append(bytes)
      flushReverse()
    }
  }

  private func flushReverse() {
    guard !closed, !sendingReverse, !reverse.isEmpty else { return }
    let bytes = reverse.removeFirst()
    sendingReverse = true
    Task {
      defer { sendingReverse = false; flushReverse() }
      do {
        guard let sendToCard else { throw CardViewFileError.unavailable }
        try await sendToCard(bytes)
      } catch {
        if !closed { self.error = error.localizedDescription }
      }
      if !closed {
        _ = try? await web.callAsyncJavaScript("tsugumiReverseCredit(size)",
          arguments: ["size": bytes.count], contentWorld: .page)
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
