import AppKit
import SwiftUI
import WebKit

// "Tsugumi を知る": the story pages of the documentation site in a web view, so
// the app and the site show the same words and the same figures from one source
// (docs/site/build.mjs, docs/site/js/). The pages use ES modules, which WebKit
// does not load from file:, so they are served under a scheme of the app's own.
// Nothing leaves the app from here: every other load is blocked, and a link to
// anything not bundled opens in the browser.
enum Explainer {
  static let scheme = "tsugumi-docs"
  static let start = URL(string: "\(scheme)://app/pages/problem.html")!
  static let published = URL(string: "https://gen740.github.io/Tsugumi/")!

  /// Contents/Resources/explainer (build-app.sh), or docs/ of the repository in
  /// a development build: the same layout.
  static let root: URL? = {
    if let dir = Bundle.main.url(forResource: "explainer", withExtension: nil) { return dir }
    #if DEBUG
    var repo = URL(fileURLWithPath: #filePath)
    for _ in 0..<5 { repo.deleteLastPathComponent() }
    let docs = repo.appendingPathComponent("docs")
    if FileManager.default.fileExists(atPath: docs.appendingPathComponent("pages/problem.html").path) { return docs }
    #endif
    return nil
  }()

  /// The bundled file behind a URL of the scheme, if there is one.
  static func file(for url: URL) -> URL? {
    guard url.scheme == scheme, let root else { return nil }
    let file = root.appendingPathComponent(String(url.path.drop { $0 == "/" })).standardizedFileURL
    let inside = file.path.hasPrefix(root.standardizedFileURL.path + "/")
    var isDirectory: ObjCBool = false
    guard inside, FileManager.default.fileExists(atPath: file.path, isDirectory: &isDirectory), !isDirectory.boolValue else { return nil }
    return file
  }

  /// The pages there are here, as paths from the root ("pages/name.html").
  static var pages: [String] {
    guard let root else { return [] }
    let names = (try? FileManager.default.contentsOfDirectory(atPath: root.appendingPathComponent("pages").path)) ?? []
    return names.filter { $0.hasSuffix(".html") && isStory($0, root) }.map { "pages/\($0)" }
  }

  /// A story page, among all the pages a development build finds in docs/.
  private static func isStory(_ name: String, _ root: URL) -> Bool {
    let url = root.appendingPathComponent("pages/\(name)")
    guard let head = try? FileHandle(forReadingFrom: url).read(upToCount: 400) else { return false }
    return String(decoding: head, as: UTF8.self).contains("class=\"story\"")
  }

  /// Marks the page as being in the app, and leaves in its list only what is here.
  static var pageScript: WKUserScript {
    let list = (try? JSONSerialization.data(withJSONObject: pages)).map { String(decoding: $0, as: UTF8.self) } ?? "[]"
    return WKUserScript(source: """
      document.documentElement.classList.add('in-app')
      addEventListener('DOMContentLoaded', () => {
        const here = new Set(\(list))
        const path = (a) => new URL(a.href).pathname.slice(1)
        for (const a of document.querySelectorAll('.nav-group li a')) if (!here.has(path(a)) && a.getAttribute('href') !== '#') a.closest('li').remove()
        for (const g of document.querySelectorAll('.nav-group')) if (!g.querySelector('li')) g.remove()
        const home = document.querySelector('.brand > a')
        if (home) home.href = new URL('/pages/problem.html', location.href).href
      })
      """, injectionTime: .atDocumentStart, forMainFrameOnly: true)
  }

  struct ExplainerWindow: View {
    var body: some View {
      Group {
        if root == nil {
          Text("Tsugumi を知る のページが見つかりません").foregroundStyle(.secondary).frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
          Page()
        }
      }
      .background(Color(red: 0x1b / 255, green: 0x1d / 255, blue: 0x1f / 255))
    }
  }

  struct Page: NSViewRepresentable {
    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> WKWebView {
      let config = WKWebViewConfiguration()
      config.websiteDataStore = .nonPersistent()
      config.setURLSchemeHandler(Files(), forURLScheme: scheme)
      config.userContentController.addUserScript(pageScript)
      let web = WKWebView(frame: .zero, configuration: config)
      web.navigationDelegate = context.coordinator
      web.allowsBackForwardNavigationGestures = true
      web.underPageBackgroundColor = NSColor(srgbRed: 0x1b / 255, green: 0x1d / 255, blue: 0x1f / 255, alpha: 1)
      let rules = #"[{"trigger":{"url-filter":".*"},"action":{"type":"block"}},"# +
        #"{"trigger":{"url-filter":"^\#(scheme):"},"action":{"type":"ignore-previous-rules"}}]"#
      WKContentRuleListStore.default().compileContentRuleList(forIdentifier: "tsugumi-explainer", encodedContentRuleList: rules) { compiled, _ in
        Task { @MainActor in
          if let compiled { config.userContentController.add(compiled) }
          web.load(URLRequest(url: start))
        }
      }
      return web
    }

    func updateNSView(_ web: WKWebView, context: Context) {}

    @MainActor
    final class Coordinator: NSObject, WKNavigationDelegate {
      func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction) async -> WKNavigationActionPolicy {
        guard let url = action.request.url else { return .cancel }
        if url.scheme == scheme {
          if file(for: url) != nil { return .allow }
          // A document that is only on the site.
          var page = URLComponents(url: published.appendingPathComponent(String(url.path.drop { $0 == "/" })), resolvingAgainstBaseURL: false)
          page?.fragment = url.fragment
          if let outside = page?.url { NSWorkspace.shared.open(outside) }
          return .cancel
        }
        if url.scheme == "about" { return .allow }
        NSWorkspace.shared.open(url)
        return .cancel
      }
    }
  }

  /// Serves the bundled files. Module scripts need their media type.
  @MainActor
  final class Files: NSObject, WKURLSchemeHandler {
    private static let types = [
      "html": "text/html; charset=utf-8", "css": "text/css; charset=utf-8", "js": "text/javascript; charset=utf-8",
      "svg": "image/svg+xml", "png": "image/png", "woff": "font/woff", "txt": "text/plain; charset=utf-8",
    ]

    func webView(_ webView: WKWebView, start task: any WKURLSchemeTask) {
      guard let url = task.request.url else { return task.didFailWithError(URLError(.badURL)) }
      guard let file = file(for: url), let data = try? Data(contentsOf: file) else {
        task.didReceive(HTTPURLResponse(url: url, statusCode: 404, httpVersion: "HTTP/1.1", headerFields: nil)!)
        task.didFinish()
        return
      }
      let type = Self.types[file.pathExtension.lowercased()] ?? "application/octet-stream"
      task.didReceive(HTTPURLResponse(url: url, statusCode: 200, httpVersion: "HTTP/1.1", headerFields: ["Content-Type": type, "Content-Length": String(data.count)])!)
      task.didReceive(data)
      task.didFinish()
    }

    func webView(_ webView: WKWebView, stop task: any WKURLSchemeTask) {}
  }
}
