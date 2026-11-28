import Foundation
import Testing
import WebKit
@testable import TsugumiApp

/// The worker's dashboard page as tsugumi-workerd assembles it
/// (workerd/cmake/dashboard.cmake), without the English dictionary.
private func dashboardPage() throws -> Data {
  let folder = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    .appendingPathComponent("../../../../workerd/dashboard").standardizedFileURL
  func read(_ name: String) throws -> String {
    try String(contentsOf: folder.appendingPathComponent(name), encoding: .utf8)
  }
  let page = try read("index.html")
    .replacingOccurrences(of: "@dashboard_css@", with: read("dashboard.css"))
    .replacingOccurrences(of: "@dashboard_dictionary@", with: "{}")
    .replacingOccurrences(of: "@dashboard_js@", with: read("dashboard.js"))
  return Data(page.utf8)
}

/// Stands for the worker's dashboard listener. A state request is answered for
/// the session it was sent with, as the worker reads the request's cookie, and
/// can be held to stand for a slow answer.
@MainActor private final class DashboardWorker: NSObject, WKURLSchemeHandler {
  let page: Data
  var admin = false
  var hold = false
  private(set) var held: [(task: any WKURLSchemeTask, admin: Bool)] = []
  private var stopped: Set<ObjectIdentifier> = []

  init(page: Data) { self.page = page }

  func webView(_ webView: WKWebView, start task: any WKURLSchemeTask) {
    switch task.request.url?.path {
    case "/api/state":
      if hold { held.append((task, admin)) } else { answer(task, state(admin: admin)) }
    case "/api/login":
      admin = true
      answer(task, Data("{}".utf8))
    default:
      answer(task, page, type: "text/html; charset=utf-8")
    }
  }

  func webView(_ webView: WKWebView, stop task: any WKURLSchemeTask) {
    stopped.insert(ObjectIdentifier(task))
  }

  func release() {
    hold = false
    for (task, admin) in held where !stopped.contains(ObjectIdentifier(task)) {
      answer(task, state(admin: admin))
    }
    held = []
  }

  private func answer(_ task: any WKURLSchemeTask, _ body: Data, type: String = "application/json; charset=utf-8") {
    guard let url = task.request.url,
          let response = HTTPURLResponse(url: url, statusCode: 200, httpVersion: "HTTP/1.1",
                                         headerFields: ["Content-Type": type]) else { return }
    task.didReceive(response)
    task.didReceive(body)
    task.didFinish()
  }

  private func state(admin: Bool) -> Data {
    let state: [String: Any] = [
      "role": admin ? "admin" : "guest", "csrf": admin ? "token" : "",
      "login": ["configured": true, "locked_seconds": 0],
      "worker": ["name": "worker", "version": "0", "contract": 0, "revision": "", "system": "aarch64-linux",
                 "server_id": "", "uptime_seconds": 0, "listen": "tls://127.0.0.1:1", "dashboard_port": 2,
                 "running": 0, "restart_pending": false, "restart_required": false],
      "jobs": [] as [Any],
      "disk": ["measured": true, "store_bytes": 0, "jobs_bytes": 0, "free_bytes": 0, "total_bytes": 0],
      "settings": ["name": "", "listen_address": "127.0.0.1", "port": 1, "dashboard_port": 2, "max_jobs": 1,
                   "nix_max_jobs": 0, "nix_cores": 0, "pull_caches": [] as [Any]],
      "hosts": [] as [Any],
    ]
    return (try? JSONSerialization.data(withJSONObject: state)) ?? Data()
  }
}

@MainActor private func until(_ what: String, _ ready: () async throws -> Bool) async throws {
  let deadline = Date().addingTimeInterval(10)
  while try await !ready() {
    guard Date() < deadline else {
      Issue.record("Timed out waiting for \(what)")
      throw CancellationError()
    }
    try await Task.sleep(for: .milliseconds(20))
  }
}

@MainActor struct WorkerDashboardTests {
  /// A login while a state request is in flight: that request carried no
  /// session, so its answer must not show the page as a guest again, and the
  /// page asks again at once rather than at the next periodic refresh.
  @Test func loginShowsTheAdministratorEvenWithAStateRequestInFlight() async throws {
    let worker = DashboardWorker(page: try dashboardPage())
    let configuration = DashboardWebView.configuration()
    configuration.setURLSchemeHandler(worker, forURLScheme: "tsugumi-worker")
    let url = try #require(URL(string: "tsugumi-worker://worker/?lang=en"))
    let coordinator = DashboardWebView.Coordinator(url: url, certificate: "") { Issue.record("Dashboard failed: \($0)") }
    let view = DashboardWebView.view(url: url, configuration: configuration, coordinator: coordinator)
    defer { view.stopLoading() }
    func page(_ script: String) async throws -> Any? {
      try await view.callAsyncJavaScript(script, contentWorld: .page)
    }
    func admin() async throws -> Bool? {
      try await page("""
        const badge = document.getElementById('badge')
        if (!badge || document.body.classList.contains('loading')) return null
        return badge.classList.contains('admin')
        """) as? Bool
    }
    try await until("the guest page") { try await admin() == false }
    _ = try await page("""
      const badge = document.getElementById('badge')
      window.shown = []
      new MutationObserver(() => shown.push(badge.classList.contains('admin') ? 'admin' : 'guest'))
        .observe(badge, { childList: true, characterData: true, subtree: true })
      return 0
      """)
    worker.hold = true
    _ = try await page("void refresh(); return 0")
    try await until("the held state request") { worker.held.count == 1 }
    _ = try await page("""
      document.getElementById('login-open').click()
      document.getElementById('password').value = 'password'
      document.getElementById('login-form').requestSubmit()
      return 0
      """)
    try await until("the login") {
      let closed = try await page("return document.getElementById('login').hidden") as? Bool
      return worker.admin && closed == true
    }
    worker.release()
    try await until("the administrator page") { try await admin() == true }
    #expect(try await page("return shown") as? [String] == ["admin"])
  }
}
