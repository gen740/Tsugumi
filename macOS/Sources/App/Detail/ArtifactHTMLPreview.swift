import SwiftUI
import WebKit

/// A static view of an HTML artifact member. Any Card can save one, including a Card the
/// Agent wrote, so the document is untrusted: its scripts do not run, nothing is loaded
/// from outside the document, and no link, form or redirect leaves it.
struct ArtifactHTMLPreview: NSViewRepresentable {
  /// Larger HTML members are not previewed; they stay exportable.
  static let limit = 32 * 1024 * 1024
  /// Enforced by WebKit for everything after it, so nothing that can load comes before it.
  static let policy = "default-src 'none'; img-src data:; style-src 'unsafe-inline' data:; font-src data:; base-uri 'none'; form-action 'none'"

  // WebKit's speculative connections can bypass a document's policy. Install
  // an independent WebKit resource blocker before loading untrusted markup.
  private static let resourceRules = """
  [
    {"trigger":{"url-filter":".*"},"action":{"type":"block"}},
    {"trigger":{"url-filter":"^data:"},"action":{"type":"ignore-previous-rules"}},
    {"trigger":{"url-filter":"^about:"},"action":{"type":"ignore-previous-rules"}}
  ]
  """

  let html: String

  /// The text of an artifact member, when the member is HTML that can be previewed.
  /// Anything else (another type, too large, not UTF-8) is left to the other previews.
  static func text(member: String, data: Data, limit: Int = limit) -> String? {
    guard ["html", "htm"].contains((member as NSString).pathExtension.lowercased()), data.count <= limit else { return nil }
    return String(data: data, encoding: .utf8)
  }

  /// The document as the view loads it. The policy goes after a leading doctype, never
  /// before it, so the document keeps its own rendering mode. Whatever this cannot read
  /// with certainty as "nothing but a doctype so far" gets the policy at the very start.
  static func document(_ html: String) -> String {
    let meta = "<meta http-equiv=\"Content-Security-Policy\" content=\"\(policy)\">"
    // Scalars, not characters: a combining mark must not hide a closing bracket.
    let text = html.unicodeScalars
    func starts(_ i: String.Index, _ word: String) -> Bool {
      var at = i
      for expected in word.unicodeScalars {
        guard at < text.endIndex, String(text[at]).lowercased() == String(expected) else { return false }
        at = text.index(after: at)
      }
      return true
    }
    var i = text.startIndex
    // Only a byte order mark, white space and comments may come before a doctype.
    while i < text.endIndex {
      if text[i] == "\u{FEFF}" || [" ", "\t", "\n", "\r", "\u{0C}"].contains(text[i]) {
        i = text.index(after: i)
      } else if starts(i, "<!--") {
        // A browser can end a comment early at a bracket ("<!-->", "--!>"); only a
        // comment with no bracket before its "-->" is read the same way everywhere.
        var at = text.index(i, offsetBy: 4)
        while at < text.endIndex, text[at] != ">", !starts(at, "-->") { at = text.index(after: at) }
        guard starts(at, "-->") else { return meta + html }
        i = text.index(at, offsetBy: 3)
      } else {
        break
      }
    }
    guard starts(i, "<!doctype"), let close = text[i...].firstIndex(of: ">") else { return meta + html }
    let after = text.index(after: close)
    return String(text[..<after]) + meta + String(text[after...])
  }

  func makeCoordinator() -> Coordinator { Coordinator() }
  func makeNSView(context: Context) -> WKWebView {
    let configuration = WKWebViewConfiguration()
    configuration.websiteDataStore = .nonPersistent()
    configuration.defaultWebpagePreferences.allowsContentJavaScript = false
    // A click can preconnect before the navigation delegate denies it. The
    // host's isolated script handles the interaction before WebKit navigates.
    let preventNavigation = """
      for (const type of ['click', 'auxclick']) {
        document.addEventListener(type, event => {
          if (event.target?.closest?.('a, area')) event.preventDefault();
        }, true);
      }
      document.addEventListener('submit', event => event.preventDefault(), true);
      document.addEventListener('DOMContentLoaded', () => {
        for (const link of document.querySelectorAll('a, area')) {
          for (const name of ['href', 'xlink:href', 'ping']) link.removeAttribute(name);
        }
        for (const form of document.querySelectorAll('form[action], [formaction]')) {
          form.removeAttribute('action');
          form.removeAttribute('formaction');
        }
      });
      """
    configuration.userContentController.addUserScript(WKUserScript(
      source: preventNavigation, injectionTime: .atDocumentStart, forMainFrameOnly: false, in: .defaultClient
    ))
    let view = WKWebView(frame: .zero, configuration: configuration)
    view.allowsLinkPreview = false
    let coordinator = context.coordinator
    view.navigationDelegate = coordinator
    WKContentRuleListStore.default().compileContentRuleList(
      forIdentifier: "TsugumiStaticArtifactPreview",
      encodedContentRuleList: Self.resourceRules
    ) { [weak view, weak coordinator] rules, _ in
      guard let view, let coordinator else { return }
      if let rules {
        view.configuration.userContentController.add(rules)
        coordinator.state = .ready
      } else {
        coordinator.state = .plainText
      }
      coordinator.display(in: view)
    }
    return view
  }

  func updateNSView(_ view: WKWebView, context: Context) {
    guard context.coordinator.html != html else { return }
    context.coordinator.html = html
    context.coordinator.display(in: view)
  }

  final class Coordinator: NSObject, WKNavigationDelegate {
    enum State { case preparing, ready, plainText }

    var html: String?
    var state = State.preparing

    func display(in view: WKWebView) {
      guard let html else { return }
      switch state {
      case .preparing:
        break
      case .ready:
        view.loadHTMLString(ArtifactHTMLPreview.document(html), baseURL: nil)
      case .plainText:
        let escaped = html.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;")
        view.loadHTMLString(ArtifactHTMLPreview.document("<!doctype html><pre>" + escaped + "</pre>"), baseURL: nil)
      }
    }

    /// Only the load of the document itself is a navigation; links, forms and redirects are not followed.
    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
      decisionHandler(action.navigationType == .other && action.request.url?.scheme == "about" ? .allow : .cancel)
    }
  }
}
