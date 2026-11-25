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
      const replies = new Map()
      let nextSend = 0
      const max = 1 << 24
      const api = {
        onmessage: null,
        async send(bytes, to) {
          if (!(bytes instanceof Uint8Array) || typeof to !== 'string' || !to || to.includes(String.fromCharCode(0)))
            throw new TypeError('Expected Uint8Array bytes and a Card path or "*"')
          const size = Math.max(1, bytes.byteLength) + 2 * to.length
          if (bytes.byteLength > max || to.length > max / 2 || used > max) {
            if (port && !reportedDrop) port.postMessage({ type: 'dropped' })
            reportedDrop = true
            throw new Error('The view message queue is full')
          }
          used += size
          const id = ++nextSend
          return await new Promise((resolve, reject) => {
            replies.set(id, { size, resolve, reject })
            const data = { type: 'data', bytes: new Uint8Array(bytes).buffer, to, id }
            if (port) port.postMessage(data, [data.bytes])
            else queued.push(data)
          })
        }
      }
      Object.defineProperty(window, 'tsugumi', { value: api })
      addEventListener('message', event => {
        if (event.source !== parent || event.data?.tsugumi !== 1) return
        event.stopImmediatePropagation()
        port = event.data.port
        for (const data of queued) port.postMessage(data, [data.bytes])
        queued.length = 0
        if (reportedDrop) port.postMessage({ type: 'dropped' })
        port.onmessage = ({ data }) => {
          if (data?.type === 'sendResult') {
            const reply = replies.get(data.id)
            if (!reply) return
            replies.delete(data.id)
            used -= reply.size
            reportedDrop = false
            if (data.error) reply.reject(new Error(data.error))
            else reply.resolve()
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
                   typeof data.to === 'string' && Number.isSafeInteger(data.id)) {
          const bytes = new Uint8Array(data.bytes)
          if (bytes.length > max || data.to.length > max / 2 || reverseBytes > max) {
            webkit.messageHandlers.tsugumiViewInput.postMessage({ dropped: true })
            port.postMessage({ type: 'sendResult', id: data.id, error: 'The view message queue is full' })
            return
          }
          const parts = []
          for (let i = 0; i < bytes.length; i += 16384)
            parts.push(String.fromCharCode(...bytes.subarray(i, i + 16384)))
          reverseBytes += Math.max(1, bytes.length) + 2 * data.to.length
          webkit.messageHandlers.tsugumiViewInput.postMessage({ bytes: btoa(parts.join('')), to: data.to, id: data.id })
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
  window.tsugumiSendResult = (id, size, error) => {
    reverseBytes -= size
    port?.postMessage({ type: 'sendResult', id, error })
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
  @ObservationIgnored private let sendToCard: ((String, Data) async throws -> Void)?
  @ObservationIgnored private var reverse: [(to: String, bytes: Data, id: Int, size: Int)] = []
  @ObservationIgnored private var sendingReverse = false
  private(set) var error: String?
  @ObservationIgnored private var outbox = CardViewOutbox()
  @ObservationIgnored private var mounted = false
  @ObservationIgnored private var sending = false
  @ObservationIgnored private var closed = false
  @ObservationIgnored private var loaded: CheckedContinuation<Void, Error>?

  /// `configure` lets tests add their own observers before anything loads.
  init(configure: (WKWebViewConfiguration) -> Void = { _ in },
       sendToCard: ((String, Data) async throws -> Void)? = nil) {
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

  func snapshot() async throws -> Data {
    guard mounted, !closed else { throw DawnViewError.unavailable }
    let image = try await web.takeSnapshot(configuration: nil)
    guard !closed, let tiff = image.tiffRepresentation, let bitmap = NSBitmapImageRep(data: tiff),
          let png = bitmap.representation(using: .png, properties: [:]) else {
      throw DawnViewError.snapshotFailed
    }
    return png
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
              bytes.count <= 1 << 24, let to = message["to"] as? String,
              let id = message["id"] as? Int {
      let size = max(1, bytes.count) + 2 * to.utf16.count
      reverse.append((to, bytes, id, size))
      flushReverse()
    }
  }

  private func flushReverse() {
    guard !closed, !sendingReverse, !reverse.isEmpty else { return }
    let (to, bytes, id, size) = reverse.removeFirst()
    sendingReverse = true
    Task {
      defer { sendingReverse = false; flushReverse() }
      var failure = ""
      do {
        guard let sendToCard else { throw CardViewFileError.unavailable }
        try await sendToCard(to, bytes)
      } catch {
        failure = error.localizedDescription
        if !closed { self.error = failure }
      }
      if !closed {
        _ = try? await web.callAsyncJavaScript("tsugumiSendResult(id, size, error)",
          arguments: ["id": id, "size": size, "error": failure], contentWorld: .page)
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
