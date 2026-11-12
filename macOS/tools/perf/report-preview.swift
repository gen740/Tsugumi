// Runs the production report preview offscreen, without opening a window.
// swiftc macOS/Sources/App/Detail/ReportPreview.swift macOS/tools/perf/report-preview.swift -o /tmp/report-preview
// /tmp/report-preview <saved-report.html> ...
import AppKit
import SwiftUI
import WebKit

@MainActor
func checkReport(_ file: String) async throws {
  let html = try String(contentsOfFile: file, encoding: .utf8)
    + "<script>document.body.dataset.unexpected = 'script ran'</script>"
  let host = NSHostingView(rootView: ReportPreview(html: html))
  host.frame = CGRect(x: 0, y: 0, width: 800, height: 600)
  host.layoutSubtreeIfNeeded()
  func find(_ view: NSView) -> WKWebView? {
    if let web = view as? WKWebView { return web }
    return view.subviews.lazy.compactMap(find).first
  }
  for _ in 0..<400 {
    host.layoutSubtreeIfNeeded()
    if let web = find(host), let state = try? await web.callAsyncJavaScript("""
      return {ready:document.readyState === 'complete', tables:document.querySelectorAll('table').length,
        images:Array.from(document.images).filter(i=>i.complete && i.naturalWidth>0).length,
        unexpected:document.body?.dataset.unexpected || ''};
      """, contentWorld: .defaultClient) as? [String: Any], state["ready"] as? Bool == true,
       (state["images"] as? Int ?? 0) > 0 {
      guard state["tables"] as? Int ?? 0 > 0, state["unexpected"] as? String == "" else {
        throw NSError(domain: "Report", code: 1, userInfo: [NSLocalizedDescriptionKey: "Invalid report content: \(state)"])
      }
      print("\((file as NSString).lastPathComponent): table and embedded image rendered; document script disabled")
      web.stopLoading()
      return
    }
    try await Task.sleep(for: .milliseconds(25))
  }
  throw NSError(domain: "Report", code: 2, userInfo: [NSLocalizedDescriptionKey: "Report preview did not load"])
}

@main
struct ReportPreviewCheck {
  static func main() {
    NSApplication.shared.setActivationPolicy(.prohibited)
    Task { @MainActor in
      do {
        for file in CommandLine.arguments.dropFirst() { try await checkReport(file) }
        exit(0)
      } catch { fputs("\(error)\n", stderr); exit(1) }
    }
    RunLoop.main.run()
  }
}
