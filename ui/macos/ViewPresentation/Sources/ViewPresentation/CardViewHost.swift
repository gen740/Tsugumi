import AppKit
import Observation
import SwiftUI
import WebKit

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
  let frame, port, admission, forwardBytes = 0, reverseBytes = 0
  const sent = []
  const max = 1 << 24
  const credit = `<script>
    (() => {
      let port, used = 0, reportedDrop = false
      const queued = []
      const max = 1 << 24
      const api = {
        onmessage: null,
        send(bytes, route = new Uint8Array(16)) {
          if (!(bytes instanceof Uint8Array) || !(route instanceof Uint8Array) || route.length !== 16)
            throw new TypeError('send expects bytes and a 16-byte route')
          if (bytes.byteLength > max || used > max) {
            if (port && !reportedDrop) port.postMessage({ type: 'dropped' })
            reportedDrop = true
            return
          }
          used += Math.max(1, bytes.byteLength)
          const data = { type: 'data', bytes: new Uint8Array(bytes).buffer, route: new Uint8Array(route).buffer }
          if (port) port.postMessage(data, [data.bytes, data.route])
          else queued.push(data)
        }
      }
      Object.defineProperty(window, 'tsugumi', { value: api })
      addEventListener('message', event => {
        if (event.source !== parent || event.data?.tsugumi !== 1) return
        event.stopImmediatePropagation()
        port = event.data.port
        for (const data of queued) port.postMessage(data, [data.bytes, data.route])
        queued.length = 0
        if (reportedDrop) port.postMessage({ type: 'dropped' })
        port.onmessage = ({ data }) => {
          if (data?.type === 'reverseCredit') {
            used -= Math.max(1, data.size)
            reportedDrop = false
          } else if (data?.type === 'data') {
            try { api.onmessage?.(new Uint8Array(data.bytes)) }
            finally { port.postMessage({ type: 'credit' }) }
          }
        }
      }, { once: true })
    })()
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
          forwardBytes -= sent.shift() ?? 0
          flush()
        } else if (data?.type === 'dropped') {
          webkit.messageHandlers.tsugumiViewInput.postMessage({ dropped: true })
        } else if (data?.type === 'data' && data.bytes instanceof ArrayBuffer &&
                   data.route instanceof ArrayBuffer && data.route.byteLength === 16) {
          const bytes = new Uint8Array(data.bytes)
          if (bytes.length > max || reverseBytes > max) {
            webkit.messageHandlers.tsugumiViewInput.postMessage({ dropped: true })
            port.postMessage({ type: 'reverseCredit', size: bytes.length })
            return
          }
          const parts = []
          for (let i = 0; i < bytes.length; i += 16384)
            parts.push(String.fromCharCode(...bytes.subarray(i, i + 16384)))
          reverseBytes += Math.max(1, bytes.length)
          const route = btoa(String.fromCharCode(...new Uint8Array(data.route)))
          webkit.messageHandlers.tsugumiViewInput.postMessage({ bytes: btoa(parts.join('')), route })
        }
      }
      frame.contentWindow.postMessage({ tsugumi: 1, port: channel.port2 }, '*', [channel.port2])
      resolve(true)
    }, { once: true })
    // Keep a leading doctype in place and apply the policy before Card scripts.
    const doctype = html.match(/^(?:\\uFEFF)?(?:[\\t\\n\\r\\f ]|<!--[^>]*?-->)*<!doctype[^>]*>/i)
    const end = doctype?.[0].length ?? 0
    const policy = document.querySelector('meta[http-equiv="Content-Security-Policy"]').outerHTML
    frame.srcdoc = html.slice(0, end) + policy + credit + html.slice(end)
    document.body.append(frame)
  })
  function flush() {
    if (!port || !admission || forwardBytes > max) return
    const { text, resolve } = admission
    admission = undefined
    const raw = atob(text), bytes = new Uint8Array(raw.length)
    for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i)
    const size = Math.max(1, bytes.length)
    forwardBytes += size
    sent.push(size)
    port.postMessage({ type: 'data', bytes: bytes.buffer }, [bytes.buffer])
    resolve()
  }
  window.tsugumiData = text => new Promise(resolve => {
    if (!port) return resolve()
    admission = { text, resolve }
    flush()
  })
  window.tsugumiReverseCredit = size => {
    reverseBytes -= Math.max(1, size)
    port?.postMessage({ type: 'reverseCredit', size })
  }
  window.tsugumiClose = () => {
    admission?.resolve()
    port?.close(); frame?.remove(); port = frame = admission = undefined
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
  @ObservationIgnored private let sendToCard: ((UUID, Data) async throws -> Void)?
  @ObservationIgnored private var reverse: [(UUID, Data)] = []
  @ObservationIgnored private var sendingReverse = false
  @ObservationIgnored private var reverseBytes = 0
  private(set) var error: String?
  @ObservationIgnored private var outbox = CardViewOutbox()
  @ObservationIgnored private var mounted = false
  @ObservationIgnored private var sending = false
  @ObservationIgnored private var closed = false
  @ObservationIgnored private var loaded: CheckedContinuation<Void, Error>?

  /// `configure` lets tests add their own observers before anything loads.
  init(configure: (WKWebViewConfiguration) -> Void = { _ in },
       sendToCard: ((UUID, Data) async throws -> Void)? = nil) {
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
    _ = try await web.callAsyncJavaScript("return await tsugumiMount(html)", arguments: ["html": html], contentWorld: .page)
    guard !closed else { throw CancellationError() }
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
              bytes.count <= 1 << 24, let routeText = message["route"] as? String,
              let routeBytes = Data(base64Encoded: routeText), routeBytes.count == 16 {
      let route = routeBytes.withUnsafeBytes { viewRoute($0.bindMemory(to: UInt8.self).baseAddress!) }
      if reverseBytes <= 1 << 24 {
        reverseBytes += max(1, bytes.count)
        reverse.append((route, bytes))
        flushReverse()
      } else {
        error = String(localized: "ビューのメッセージを送信できませんでした")
      }
      web.evaluateJavaScript("tsugumiReverseCredit(\(bytes.count))", completionHandler: nil)
    }
  }

  private func flushReverse() {
    guard !closed, !sendingReverse, !reverse.isEmpty else { return }
    let (route, bytes) = reverse.removeFirst()
    sendingReverse = true
    Task {
      defer {
        reverseBytes -= max(1, bytes.count)
        sendingReverse = false
        flushReverse()
      }
      do {
        guard let sendToCard else { throw CardViewFileError.unavailable }
        try await sendToCard(route, bytes)
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
