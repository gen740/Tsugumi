import SwiftUI
import TsugumiAgentClient
import TsugumiRPC
import WebKit

struct WorkerDashboard: View {
  let service: ExecutionServiceConnection
  let close: () -> Void
  @State private var page: Tsugumi_Host_WorkerDashboardReplyT?
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
      } else if let page {
        DashboardWebView(id: service.id, generation: page.generation, html: page.body) { problem = $0 }
      } else {
        ProgressView("ダッシュボードを読み込み中…")
          .frame(maxWidth: .infinity, maxHeight: .infinity)
      }
    }
    .task {
      do {
        page = try await ExecutionServices.dashboard(service.id, operation: .page,
          language: AppPreferences.Language.running.rawValue)
      } catch {
        problem = error.localizedDescription
      }
    }
  }
}

private struct DashboardWebView: NSViewRepresentable {
  static let bridge = "workerDashboard"
  static let policy = "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'"

  let id: String
  let generation: String
  let html: String
  let report: (String) -> Void

  func makeCoordinator() -> Coordinator { Coordinator(id: id, generation: generation) }

  func makeNSView(context: Context) -> WKWebView {
    let configuration = WKWebViewConfiguration()
    configuration.websiteDataStore = .nonPersistent()
    let coordinator = context.coordinator
    configuration.userContentController.addScriptMessageHandler(coordinator, contentWorld: .page, name: Self.bridge)

    let view = WKWebView(frame: .zero, configuration: configuration)
    view.allowsLinkPreview = false
    view.navigationDelegate = coordinator
    WKContentRuleListStore.default().compileContentRuleList(
      forIdentifier: "TsugumiWorkerDashboard",
      encodedContentRuleList: ArtifactHTMLPreview.resourceRules
    ) { [weak view, weak coordinator] rules, error in
      guard let view, let coordinator, coordinator.active else { return }
      guard let rules else {
        report(error?.localizedDescription ?? "Could not prepare the dashboard resource policy")
        return
      }
      view.configuration.userContentController.add(rules)
      coordinator.allowLoad = true
      view.loadHTMLString(ArtifactHTMLPreview.document(html, policy: Self.policy), baseURL: nil)
    }
    return view
  }

  func updateNSView(_ view: WKWebView, context: Context) {}

  static func dismantleNSView(_ view: WKWebView, coordinator: Coordinator) {
    coordinator.active = false
    view.configuration.userContentController.removeScriptMessageHandler(forName: bridge, contentWorld: .page)
    view.stopLoading()
  }

  @MainActor final class Coordinator: NSObject, WKNavigationDelegate, WKScriptMessageHandlerWithReply {
    let id: String
    let generation: String
    var active = true
    var allowLoad = false
    private var pending = 0

    init(id: String, generation: String) {
      self.id = id
      self.generation = generation
    }

    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction)
      async -> WKNavigationActionPolicy {
      guard active, allowLoad, action.targetFrame?.isMainFrame == true,
            action.navigationType == .other, action.request.url?.absoluteString == "about:blank" else {
        return .cancel
      }
      allowLoad = false
      return .allow
    }

    func userContentController(_ userContentController: WKUserContentController,
                               didReceive message: WKScriptMessage) async -> (Any?, String?) {
      guard active, !allowLoad, message.frameInfo.isMainFrame,
            let request = message.body as? [String: String],
            let name = request["operation"], let body = request["body"],
            body.utf8.count <= 65536, pending < 4 else {
        return (nil, "Invalid dashboard request")
      }
      let operation: Tsugumi_Host_WorkerDashboardOperation
      switch name {
      case "Jobs": operation = .jobs
      case "Keys": operation = .keys
      case "Challenge": operation = .challenge
      case "RemoveJobs": operation = .removejobs
      case "AddKey": operation = .addkey
      case "RemoveKey": operation = .removekey
      default:
        return (nil, "Unknown dashboard operation")
      }
      pending += 1
      defer { pending -= 1 }
      do {
        let reply = try await ExecutionServices.dashboard(id, operation: operation, generation: generation, body: body)
        return (reply.body, nil)
      } catch {
        return (nil, error.localizedDescription)
      }
    }
  }
}
