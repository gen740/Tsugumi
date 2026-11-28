import CryptoKit
import SwiftUI
import TsugumiAgentClient
import WebKit

/// The worker's own dashboard page, loaded from its HTTPS listener. The
/// certificate is self-signed: the view trusts exactly the certificate whose
/// SHA-256 the worker reported over the host's pinned connection.
struct WorkerDashboard: View {
  let service: ExecutionServiceConnection
  let close: () -> Void
  @State private var problem: String?

  var body: some View {
    VStack(spacing: 0) {
      HStack {
        Button("戻る", action: close)
        Text(service.name).fontWeight(.semibold)
        Spacer()
      }.padding(12)
      Divider()
      if let problem {
        Text("ワーカーのダッシュボードを読み込めません: \(problem)")
          .foregroundStyle(.secondary).textSelection(.enabled)
          .frame(maxWidth: .infinity, maxHeight: .infinity)
      } else if let url = Self.url(service) {
        DashboardWebView(url: url, certificate: service.capabilities.dashboardCertificateSHA256) { problem = $0 }
      } else {
        Text("ワーカーのダッシュボードを読み込めません: \(service.capabilities.dashboardURL)")
          .foregroundStyle(.secondary).textSelection(.enabled)
          .frame(maxWidth: .infinity, maxHeight: .infinity)
      }
    }
  }

  static func url(_ service: ExecutionServiceConnection) -> URL? {
    guard var parts = URLComponents(string: service.capabilities.dashboardURL), parts.scheme == "https",
          service.capabilities.dashboardCertificateSHA256.count == 64 else { return nil }
    parts.queryItems = [URLQueryItem(name: "lang", value: AppPreferences.Language.running.rawValue)]
    return parts.url
  }
}

private struct DashboardWebView: NSViewRepresentable {
  let url: URL
  let certificate: String
  let report: (String) -> Void

  func makeCoordinator() -> Coordinator { Coordinator(url: url, certificate: certificate, report: report) }

  func makeNSView(context: Context) -> WKWebView {
    let configuration = WKWebViewConfiguration()
    // The session cookie lives only as long as this view.
    configuration.websiteDataStore = .nonPersistent()
    let view = WKWebView(frame: .zero, configuration: configuration)
    view.allowsLinkPreview = false
    view.navigationDelegate = context.coordinator
    view.load(URLRequest(url: url))
    return view
  }

  func updateNSView(_ view: WKWebView, context: Context) {}

  static func dismantleNSView(_ view: WKWebView, coordinator: Coordinator) {
    view.stopLoading()
  }

  @MainActor final class Coordinator: NSObject, WKNavigationDelegate {
    let url: URL
    let certificate: String
    let report: (String) -> Void
    private var mismatch = false

    init(url: URL, certificate: String, report: @escaping (String) -> Void) {
      self.url = url
      self.certificate = certificate
      self.report = report
    }

    /// Only the dashboard's own origin, in this view.
    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction)
      async -> WKNavigationActionPolicy {
      guard let target = action.request.url, target.scheme == url.scheme, target.host == url.host,
            target.port == url.port, action.targetFrame?.isMainFrame == true else { return .cancel }
      return .allow
    }

    func webView(_ webView: WKWebView, respondTo challenge: URLAuthenticationChallenge)
      async -> (URLSession.AuthChallengeDisposition, URLCredential?) {
      let space = challenge.protectionSpace
      guard space.authenticationMethod == NSURLAuthenticationMethodServerTrust,
            space.host == url.host, space.port == url.port, let trust = space.serverTrust,
            let leaf = (SecTrustCopyCertificateChain(trust) as? [SecCertificate])?.first else {
        return (.cancelAuthenticationChallenge, nil)
      }
      let digest = SHA256.hash(data: SecCertificateCopyData(leaf) as Data)
        .map { String(format: "%02x", $0) }.joined()
      guard digest == certificate else {
        mismatch = true
        report(String(localized: "ワーカーの証明書が登録時と一致しません。接続を確認し直してください。"))
        return (.cancelAuthenticationChallenge, nil)
      }
      return (.useCredential, URLCredential(trust: trust))
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
      if !mismatch { report(error.localizedDescription) }
    }
  }
}
