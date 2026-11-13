import AppKit
import WebKit

// No window, app activation or permissions; suitable for a shared development Mac.
@MainActor final class Probe: NSObject, WKNavigationDelegate {
  let web: WKWebView = {
    let config = WKWebViewConfiguration()
    config.websiteDataStore = .nonPersistent()
    return WKWebView(frame: CGRect(x: 0, y: 0, width: 64, height: 64), configuration: config)
  }()
  func run() {
    web.navigationDelegate = self
    web.loadHTMLString("<!doctype html><title>WebGPU probe</title>", baseURL: URL(string: "https://localhost/"))
    DispatchQueue.main.asyncAfter(deadline: .now() + 20) { print("timeout"); exit(2) }
  }
  func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
    Task {
      let result = try? await web.callAsyncJavaScript("""
      const gpu = !!navigator.gpu;
      try {
        const adapter = gpu ? await navigator.gpu.requestAdapter() : null;
        return JSON.stringify({secure: isSecureContext, gpu, adapter: !!adapter});
      } catch (e) { return JSON.stringify({secure: isSecureContext, gpu, error: String(e)}); }
      """, arguments: [:], in: nil, contentWorld: .page)
        print(result)
        exit(0)
      }
  }
}
MainActor.assumeIsolated {
  let app = NSApplication.shared
  app.setActivationPolicy(.accessory)
  let probe = Probe()
  probe.run()
  app.run()
}
