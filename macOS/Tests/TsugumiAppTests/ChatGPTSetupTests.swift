import AppKit
import Foundation
import SwiftUI
import Testing
@testable import TsugumiAgentClient
@testable import TsugumiApp

/// Presentation doubles never call OAuth, Keychain, the browser or a model.
@MainActor @Suite(.serialized) struct ChatGPTSetupTests {
  private func account(plan: Bool = true) -> ChatGPTAccountInfo {
    .init(clientId: "fixture", email: "fixture@example.com", signedIn: true, planEnabled: plan, active: true)
  }

  @Test func signInWaitsForTheBrowserAndCanBeCancelled() async {
    let model = ChatGPTSignInModel(authenticate: { _, _ in
      try await Task.sleep(for: .seconds(30))
      throw CancellationError()
    }, load: { .init(accounts: [], result: nil) }, acknowledge: { _ in Issue.record("Unexpected welcome") })
    model.signIn { Issue.record("Cancelled sign-in finished setup") }
    #expect(model.signingIn != nil)
    let pending = model.signingIn
    pending?.cancel()
    await pending?.value
    #expect(model.signingIn == nil && model.welcome == nil)
    #expect(model.message == String(localized: "サインインを中止しました。"))
  }

  @Test func noPlanPermissionOffersConsentAndDoesNotFinishSetup() async {
    let a = account(plan: false)
    var requests: [Bool] = []
    let model = ChatGPTSignInModel(authenticate: { client, consent in
      requests.append(consent)
      if consent { #expect(client == a.clientId) }
      return .init(account: a, welcome: false)
    }, load: { .init(accounts: [a], result: nil) }, acknowledge: { _ in Issue.record("Unexpected welcome") })
    model.signIn { Issue.record("Plan permission was not granted") }
    await model.signingIn?.value
    #expect(model.active?.planEnabled == false && model.welcome == nil)
    #expect(model.message == String(localized: "サインインしましたが、ChatGPT プランの利用は許可されませんでした。「プランの利用を許可」から許可するか、API キーの接続を使ってください。"))
    model.signIn(clientId: a.clientId, consent: true) { Issue.record("Plan permission was not granted") }
    await model.signingIn?.value
    #expect(requests == [false, true])
  }

  @Test func failureCanBeRetriedWithoutFinishingSetup() async {
    var attempts = 0
    let a = account()
    let model = ChatGPTSignInModel(authenticate: { _, _ in
      attempts += 1
      if attempts == 1 { throw ChatGPTError.failed("fixture failure") }
      return .init(account: a, welcome: false)
    }, load: { .init(accounts: [a], result: nil) }, acknowledge: { _ in })
    var finished = 0
    model.signIn { finished += 1 }
    await model.signingIn?.value
    #expect(finished == 0 && model.message == "ChatGPT: fixture failure")
    model.signIn { finished += 1 }
    await model.signingIn?.value
    #expect(finished == 1 && model.welcome == nil)
  }

  @Test func welcomeIsAcknowledgedOnceAcrossFirstStartAndSettings() async {
    let a = account()
    var welcomed = false
    var acknowledgements = 0
    let authenticate: (String?, Bool) async throws -> ChatGPTAuth.SignInResult = { _, _ in
      .init(account: a, welcome: !welcomed)
    }
    let acknowledge: (String) async throws -> Void = { id in
      #expect(id == a.clientId)
      welcomed = true
      acknowledgements += 1
    }
    let setup = ChatGPTSignInModel(authenticate: authenticate, load: { .init(accounts: [a], result: nil) }, acknowledge: acknowledge)
    var finished = 0
    setup.signIn { finished += 1 }
    await setup.signingIn?.value
    #expect(setup.welcome == a.clientId && finished == 0)
    await setup.welcomed()
    await setup.welcomed()
    #expect(finished == 1 && acknowledgements == 1 && setup.welcome == nil)
    let settings = ChatGPTSignInModel(authenticate: authenticate, load: { .init(accounts: [a], result: nil) }, acknowledge: acknowledge)
    settings.signIn { finished += 1 }
    await settings.signingIn?.value
    #expect(settings.welcome == nil && finished == 2 && acknowledgements == 1)
  }

  private func until(_ predicate: () -> Bool) async throws {
    let deadline = ContinuousClock.now + .seconds(3)
    while !predicate(), ContinuousClock.now < deadline { try await Task.sleep(for: .milliseconds(10)) }
    try #require(predicate())
  }

  @Test(arguments: [false, true]) func dismissingTheViewCancelsAtTheHost(beginPending: Bool) async throws {
    NSApplication.shared.setActivationPolicy(.prohibited)
    let host = SignInHostDouble(delayBegin: beginPending)
    let auth = host.auth()
    let model = ChatGPTSignInModel(authenticate: { client, consent in
      try await auth.signIn(clientId: client, consent: consent) { _ in await host.openBrowser() }
    }, load: { await auth.status() })
    let view = NSHostingView(rootView: AnyView(ChatGPTConnection(model: model, onboarding: true) {
      Issue.record("Dismissed sign-in finished setup")
    }))
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 500, height: 300),
                          styleMask: [.borderless], backing: .buffered, defer: true)
    window.contentView = view
    defer { window.contentView = nil }
    view.layoutSubtreeIfNeeded()
    try await until { host.statusReads > 0 }
    model.signIn { Issue.record("Dismissed sign-in finished setup") }
    let pending = try #require(model.signingIn)
    try await until { host.begins == 1 && (beginPending || host.browserOpens == 1) }
    // Replace the actual SwiftUI subtree, invoking its onDisappear modifier.
    view.rootView = AnyView(EmptyView())
    view.layoutSubtreeIfNeeded()
    try await until { pending.isCancelled }
    if beginPending { host.finishBegin() }
    try await until { host.cancels == 1 }
    host.finishCancel()
    await pending.value
    #expect(host.browserOpens == (beginPending ? 0 : 1))
    #expect(model.signingIn == nil && !ChatGPTSignInModel.hasPendingSignIns)
  }

  @Test(arguments: [false, true]) func terminationWaitsForTheHostCancelReply(beginPending: Bool) async throws {
    let host = SignInHostDouble(delayBegin: beginPending)
    let auth = host.auth()
    let model = ChatGPTSignInModel(authenticate: { client, consent in
      try await auth.signIn(clientId: client, consent: consent) { _ in await host.openBrowser() }
    }, load: { await auth.status() })
    model.signIn { Issue.record("Quit finished setup") }
    let pending = try #require(model.signingIn)
    try await until { host.begins == 1 && (beginPending || host.browserOpens == 1) }
    // onDisappear may already have cancelled the task when Quit is handled.
    pending.cancel()
    let delegate = AppDelegate()
    var replies = 0
    delegate.terminationReply = { allowed in #expect(allowed); replies += 1 }
    #expect(delegate.applicationShouldTerminate(NSApplication.shared) == .terminateLater)
    if beginPending {
      #expect(host.cancels == 0 && replies == 0)
      host.finishBegin()
    }
    try await until { host.cancels == 1 }
    #expect(replies == 0, "Quit must wait for the cancellation acknowledgement")
    #expect(delegate.applicationShouldTerminate(NSApplication.shared) == .terminateLater)
    host.finishCancel()
    await pending.value
    try await until { replies == 1 }
    #expect(host.browserOpens == (beginPending ? 0 : 1))
    #expect(!ChatGPTSignInModel.hasPendingSignIns)
  }

  @Test func terminationHasABoundedWaitForAnUnresponsiveHost() async throws {
    let host = SignInHostDouble(delayBegin: false)
    let auth = host.auth()
    let model = ChatGPTSignInModel(authenticate: { _, _ in
      try await auth.signIn { _ in await host.openBrowser() }
    }, load: { await auth.status() })
    model.signIn { Issue.record("Quit finished setup") }
    let pending = try #require(model.signingIn)
    try await until { host.browserOpens == 1 }
    let delegate = AppDelegate()
    delegate.signInTerminationTimeout = .milliseconds(30)
    var replies = 0
    delegate.terminationReply = { allowed in #expect(allowed); replies += 1 }
    #expect(delegate.applicationShouldTerminate(NSApplication.shared) == .terminateLater)
    try await until { host.cancels == 1 && replies == 1 }
    host.finishCancel()
    await pending.value
    try await Task.sleep(for: .milliseconds(50))
    #expect(replies == 1, "A late acknowledgement must not reply twice")
  }

  @Test(.hostService) func successSelectsChatGPTAndFinishesFirstStart() async throws {
    NSApplication.shared.setActivationPolicy(.prohibited)
    let fixtures = ChatResumeTests()
    try await fixtures.withFixture { fixture in
      await CorePreferences.shared.reload()
      let guide = GuideController.shared
      guide.resetIntroduction()
      // The production auth adapter talks only to the existing local fixture.
      let authenticate: (String?, Bool) async throws -> ChatGPTAuth.SignInResult = {
        try await fixture.signInChatGPT(clientId: $0, consent: $1)
      }
      let model = ChatGPTSignInModel(authenticate: authenticate)
      model.signIn { try await guide.finishChatGPTSetup() }
      await model.signingIn?.value
      #expect(model.welcome != nil && !guide.setupDone)
      // Reopening the app creates a new presentation model; hostd survives.
      let reopened = ChatGPTSignInModel(authenticate: authenticate)
      await reopened.refresh { try await guide.finishChatGPTSetup() }
      #expect(reopened.welcome == model.welcome)
      await reopened.welcomed()
      await CorePreferences.shared.reload()
      #expect(CorePreferences.shared.string(forKey: "agent.provider") == "chatgpt")
      #expect(guide.setupDone)
      try await fixtures.until("ChatGPT connects the guide") { guide.connected }
      let email = "fixture@example.com"
      #expect(model.message == String(localized: "\(email) でサインインしました。"))
      let id = try #require(model.active?.clientId)
      let settings = ChatGPTSignInModel(authenticate: authenticate)
      settings.signIn(clientId: id) {}
      await settings.signingIn?.value
      #expect(settings.active?.planEnabled == true && settings.welcome == nil)
    }
  }
}

/// RPC boundary double: no browser, network, credential store or resident service.
@MainActor private final class SignInHostDouble {
  let delayBegin: Bool
  var begins = 0
  var statusReads = 0
  var cancels = 0
  var browserOpens = 0
  private var beginReply: CheckedContinuation<Void, Never>?
  private var cancelReply: CheckedContinuation<Void, Never>?
  init(delayBegin: Bool) { self.delayBegin = delayBegin }
  func auth() -> ChatGPTAuth {
    ChatGPTAuth(endpoints: .openAI, profile: "fixture", call: { method, fields in
      await self.call(method, fields)
    })
  }
  func openBrowser() { browserOpens += 1 }
  func finishBegin() { beginReply?.resume(); beginReply = nil }
  func finishCancel() { cancelReply?.resume(); cancelReply = nil }
  private func call(_ method: String, _ fields: [String: Any]) async -> [String: Any] {
    #expect(fields["profile"] as? String == "fixture")
    switch method {
    case "SignInBeginRequest":
      begins += 1
      if delayBegin { await withCheckedContinuation { beginReply = $0 } }
      return ["attempt_id": "fixture-attempt", "url": "https://fixture.invalid/authorize"]
    case "SignInStatusRequest":
      statusReads += 1
      return ["state": "pending", "accounts": []]
    case "SignInActionRequest":
      #expect(fields["action"] as? String == "cancel")
      #expect(fields["attempt_id"] as? String == "fixture-attempt")
      #expect(!Task.isCancelled, "Cancel must be sent by an uncancelled task")
      cancels += 1
      await withCheckedContinuation { cancelReply = $0 }
      return ["state": "cancelled"]
    default:
      Issue.record("Unexpected host request: \(method)")
      return [:]
    }
  }
}
