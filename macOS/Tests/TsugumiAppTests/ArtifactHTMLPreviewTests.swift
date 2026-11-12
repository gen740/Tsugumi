import AppKit
import Foundation
import SwiftUI
import Testing
import WebKit
@testable import TsugumiApp

/// Stands for everything outside the document: a listener on this machine that counts
/// the connections made to it. A request the preview lets out arrives here.
private final class Outside: @unchecked Sendable {
  let port: UInt16
  private let socket: Int32
  private let lock = NSLock()
  private var seen: [String] = []
  private var stopped = false

  init() throws {
    let fd = Darwin.socket(AF_INET, SOCK_STREAM, 0)
    guard fd >= 0 else { throw POSIXError(.init(rawValue: errno) ?? .EIO) }
    var address = sockaddr_in()
    address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
    address.sin_family = sa_family_t(AF_INET)
    address.sin_addr.s_addr = inet_addr("127.0.0.1")
    let size = socklen_t(MemoryLayout<sockaddr_in>.size)
    let bound = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, size) } }
    guard bound == 0, listen(fd, 64) == 0 else {
      let error = POSIXError(.init(rawValue: errno) ?? .EIO)
      close(fd)
      throw error
    }
    var length = size
    _ = withUnsafeMutablePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &length) } }
    _ = fcntl(fd, F_SETFL, O_NONBLOCK)
    port = UInt16(bigEndian: address.sin_port)
    socket = fd
    Thread.detachNewThread { [self] in
      while !lock.withLock({ stopped }) {
        var waiting = pollfd(fd: socket, events: Int16(POLLIN), revents: 0)
        guard poll(&waiting, 1, 20) > 0 else { continue }
        let connection = accept(socket, nil, nil)
        guard connection >= 0 else { continue }
        // The request line says which part of the document got out.
        _ = fcntl(connection, F_SETFL, 0)
        var wait = timeval(tv_sec: 0, tv_usec: 300_000)
        setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &wait, socklen_t(MemoryLayout<timeval>.size))
        var bytes = [UInt8](repeating: 0, count: 512)
        let read = recv(connection, &bytes, bytes.count, 0)
        let line = String(decoding: bytes.prefix(max(read, 0)), as: UTF8.self).split(whereSeparator: \.isNewline).first.map(String.init) ?? "(no request)"
        lock.withLock { seen.append(line) }
        close(connection)
      }
      close(socket)
    }
  }

  var requests: [String] { lock.withLock { seen } }
  func stop() { lock.withLock { stopped = true } }
  func address(_ path: String) -> String { "http://127.0.0.1:\(port)/\(path)" }
}

/// A one-pixel PNG, so "the embedded image is shown" can be told from "nothing loaded".
private let pixel = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgaGj4DwADhAIAV8n6LgAAAABJRU5ErkJggg=="

/// Every way a saved document could run code, load something or leave. An HTML artifact
/// can come from any Card, including one the Agent wrote.
private func hostileDocument(_ out: Outside) -> String {
  let mark = "document.documentElement.dataset.ran"
  return """
  <!DOCTYPE html>
  <html>
  <head>
  <meta charset="utf-8">
  <base href="\(out.address("base/"))">
  <meta http-equiv="refresh" content="0; url=\(out.address("refresh"))">
  <meta http-equiv="Content-Security-Policy" content="default-src * data:">
  <link rel="stylesheet" href="\(out.address("sheet.css"))">
  <link rel="preconnect" href="\(out.address(""))">
  <link rel="prefetch" href="\(out.address("prefetch"))">
  <script>\(mark) = 'inline'</script>
  <script src="\(out.address("external.js"))"></script>
  <style>@import url("\(out.address("import.css"))"); body { background: url(\(out.address("bg.png"))) } @font-face { font-family: f; src: url(\(out.address("font.woff"))) } p { font-family: f }</style>
  </head>
  <body background="\(out.address("body.png"))" onload="\(mark) = 'onload'">
  <h1 id="top">Result 21</h1>
  <details id="source" open><summary>Source</summary><pre>saved input</pre></details>
  <p style="background: url(\(out.address("inline.png")))">kept text</p>
  <img id="embedded" src="\(pixel)" alt="figure">
  <img src="\(out.address("pixel.png"))" srcset="\(out.address("2x.png")) 2x" onerror="\(mark) = 'onerror'">
  <video poster="\(out.address("poster.png"))" autoplay><source src="\(out.address("movie.mp4"))"></video>
  <audio src="\(out.address("sound.mp3"))" autoplay></audio>
  <a id="out" href="\(out.address("link"))" ping="\(out.address("ping"))">out</a>
  <a id="js" href="javascript:\(mark)='href'">js</a>
  <form id="send" action="\(out.address("submit"))" method="post"><input name="q" value="secret"></form>
  <iframe src="\(out.address("frame"))"></iframe>
  <iframe srcdoc="<script>parent.\(mark)='srcdoc'</script><img src='\(out.address("srcdoc.png"))'>"></iframe>
  <object data="\(out.address("object"))"></object>
  <embed src="\(out.address("embed"))">
  <svg xmlns:xlink="http://www.w3.org/1999/xlink" onload="\(mark) = 'svg'">
    <script>\(mark) = 'svgscript'</script>
    <image xlink:href="\(out.address("svg.png"))" width="1" height="1"></image>
    <use href="\(out.address("sprite.svg"))#a"></use>
  </svg>
  </body>
  </html>
  """
}

@MainActor private func webView(in view: NSView) -> WKWebView? {
  if let web = view as? WKWebView { return web }
  return view.subviews.lazy.compactMap { webView(in: $0) }.first
}

@MainActor private func until(_ seconds: Double, _ done: @MainActor () async -> Bool) async -> Bool {
  let end = Date().addingTimeInterval(seconds)
  while Date() < end {
    if await done() { return true }
    try? await Task.sleep(for: .milliseconds(25))
  }
  return false
}

/// What the document looks like from outside the page's own scripts, which stay off.
@MainActor private func state(_ web: WKWebView) async -> [String: Any] {
  let read = """
    const image = document.getElementById('embedded');
    return { ready: document.readyState === 'complete', ran: document.documentElement.dataset.ran || '',
      mode: document.compatMode, address: location.href, text: document.body ? document.body.innerText : '',
      image: !!image && image.complete && image.naturalWidth > 0 };
    """
  return (try? await web.callAsyncJavaScript(read, contentWorld: .defaultClient)) as? [String: Any] ?? [:]
}

/// Without this the test below could pass on a machine where nothing can reach the
/// listener or run a script anyway: an ordinary web view does both with the same document.
@MainActor @Test func anOrdinaryWebViewRunsAndLoadsTheHostileDocument() async throws {
  NSApplication.shared.setActivationPolicy(.prohibited)
  let out = try Outside()
  defer { out.stop() }
  let plain = WKWebView(frame: CGRect(x: 0, y: 0, width: 800, height: 600))
  plain.loadHTMLString("<!DOCTYPE html><script>document.documentElement.dataset.ran = 'inline'</script><img src=\"\(out.address("control.png"))\">", baseURL: nil)
  #expect(await until(20) { !out.requests.isEmpty }, "the listener does not see a web view's requests")
  #expect(await until(20) { await state(plain)["ran"] as? String == "inline" }, "the script marker is not readable")
}

@MainActor @Test func theHTMLArtifactPreviewRunsNothingAndLoadsNothingFromOutside() async throws {
  NSApplication.shared.setActivationPolicy(.prohibited)
  let out = try Outside()
  defer { out.stop() }
  let host = NSHostingView(rootView: ArtifactHTMLPreview(html: hostileDocument(out)))
  host.frame = CGRect(x: 0, y: 0, width: 800, height: 600)
  host.layoutSubtreeIfNeeded()
  let web = try #require(webView(in: host))
  #expect(web.configuration.defaultWebpagePreferences.allowsContentJavaScript == false)
  #expect(web.configuration.websiteDataStore.isPersistent == false)

  // The document is shown: its text and the image embedded in it.
  let loaded = await until(30) {
    host.layoutSubtreeIfNeeded()
    let s = await state(web)
    return s["ready"] as? Bool == true && s["image"] as? Bool == true
  }
  var shown = await state(web)
  #expect(loaded, "the preview did not show the document: \(shown)")
  // Time for the refresh, late loads and media to try.
  try await Task.sleep(for: .milliseconds(1500))
  shown = await state(web)
  #expect((shown["text"] as? String ?? "").contains("Result 21"))
  #expect(shown["ran"] as? String == "", "a script of the document ran")
  #expect(shown["address"] as? String == "about:blank", "the document was redirected")
  #expect(shown["mode"] as? String == "CSS1Compat", "the doctype was lost")
  #expect(out.requests == [], "the document loaded from outside: \(out.requests)")

  // A person clicking a link or sending the form does not leave the document either.
  _ = try? await web.callAsyncJavaScript("document.getElementById('out').click(); document.getElementById('js').click(); document.getElementById('send').submit()", contentWorld: .defaultClient)
  try await Task.sleep(for: .milliseconds(700))
  shown = await state(web)
  #expect(shown["address"] as? String == "about:blank")
  #expect(shown["ran"] as? String == "")
  #expect(web.url?.absoluteString == "about:blank")
  #expect(out.requests == [], "a link or form reached outside: \(out.requests)")
  let source = try await web.callAsyncJavaScript("""
    const source = document.getElementById('source');
    const wasOpen = source.open;
    source.querySelector('summary').click();
    return wasOpen && !source.open;
    """, contentWorld: .defaultClient) as? Bool
  #expect(source == true, "native HTML disclosure must work with document scripts disabled")
}

@Test func thePolicyComesAfterALeadingDoctypeAndBeforeEverythingElse() {
  let meta = "<meta http-equiv=\"Content-Security-Policy\" content=\"\(ArtifactHTMLPreview.policy)\">"
  #expect(ArtifactHTMLPreview.document("<!DOCTYPE html><html><p>x</p>") == "<!DOCTYPE html>\(meta)<html><p>x</p>")
  #expect(ArtifactHTMLPreview.document("\u{FEFF}\n <!-- saved report -->\n<!doctype HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\">\n<p>x</p>")
    == "\u{FEFF}\n <!-- saved report -->\n<!doctype HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\">\(meta)\n<p>x</p>")
  // No doctype: the document is in quirks mode by its own choice, and the policy leads.
  #expect(ArtifactHTMLPreview.document("<p>x</p>") == meta + "<p>x</p>")
  #expect(ArtifactHTMLPreview.document("") == meta)
  // Nothing that can load may come before the policy, however the start is dressed up.
  for start in [
    "<img src=x><!DOCTYPE html>",
    "<!--><img src=x>--><!DOCTYPE html>",
    "<!---><img src=x>--><!DOCTYPE html>",
    "<!-- a --!><img src=x>--><!DOCTYPE html>",
    "<!-- never closed <!DOCTYPE html><img src=x>",
    "\u{00A0}<img src=x><!DOCTYPE html>",
  ] {
    #expect(ArtifactHTMLPreview.document(start + "<p>x</p>").hasPrefix(meta), "\(start)")
  }
  // A combining mark after the bracket does not move the end of the doctype.
  #expect(ArtifactHTMLPreview.document("<!DOCTYPE html>\u{0301}<img src=x>") == "<!DOCTYPE html>\(meta)\u{0301}<img src=x>")
  #expect(ArtifactHTMLPreview.policy.hasPrefix("default-src 'none'"))
  #expect(!ArtifactHTMLPreview.policy.contains("script-src") && !ArtifactHTMLPreview.policy.contains("http") && !ArtifactHTMLPreview.policy.contains("*"))
}

@Test func onlyHTMLMembersWithinTheLimitGetTheHTMLPreview() {
  let page = Data("<!DOCTYPE html><p>size</p>".utf8)
  #expect(ArtifactHTMLPreview.limit == 32 * 1024 * 1024)
  #expect(ArtifactHTMLPreview.text(member: "report.html", data: page) == "<!DOCTYPE html><p>size</p>")
  #expect(ArtifactHTMLPreview.text(member: "nested/INDEX.HTM", data: page) != nil)
  for member in ["plot.png", "data.csv", "notes.html.txt", "html", "report.xhtml"] {
    #expect(ArtifactHTMLPreview.text(member: member, data: page) == nil, "\(member)")
  }
  #expect(ArtifactHTMLPreview.text(member: "report.html", data: Data([0x3c, 0x70, 0x3e, 0xff, 0xfe])) == nil, "not UTF-8")
  #expect(ArtifactHTMLPreview.text(member: "report.html", data: page, limit: page.count) != nil, "at the limit")
  #expect(ArtifactHTMLPreview.text(member: "report.html", data: page, limit: page.count - 1) == nil, "one byte over the limit")
  #expect(ArtifactHTMLPreview.text(member: "report.html", data: Data(count: ArtifactHTMLPreview.limit + 1)) == nil, "over 32 MiB")
}
