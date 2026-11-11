import SwiftUI
import WebKit

/// Saved reports embed their resources; no scripts, network or live files are needed.
struct ReportPreview: NSViewRepresentable {
  let html: String

  func makeCoordinator() -> Coordinator { Coordinator() }
  func makeNSView(context: Context) -> WKWebView {
    let configuration = WKWebViewConfiguration()
    configuration.websiteDataStore = .nonPersistent()
    configuration.defaultWebpagePreferences.allowsContentJavaScript = false
    let view = WKWebView(frame: .zero, configuration: configuration)
    view.navigationDelegate = context.coordinator
    return view
  }

  func updateNSView(_ view: WKWebView, context: Context) {
    guard context.coordinator.html != html else { return }
    context.coordinator.html = html
    let policy = "default-src 'none'; img-src data:; style-src 'unsafe-inline' data:; font-src data:; base-uri 'none'; form-action 'none'"
    view.loadHTMLString("<meta http-equiv=\"Content-Security-Policy\" content=\"\(policy)\">" + html, baseURL: nil)
  }

  final class Coordinator: NSObject, WKNavigationDelegate {
    var html: String?
    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
      decisionHandler(action.navigationType == .other && action.request.url?.scheme == "about" ? .allow : .cancel)
    }
  }
}
