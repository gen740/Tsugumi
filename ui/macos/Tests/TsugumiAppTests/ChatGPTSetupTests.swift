import AppKit
import Foundation
import Observation
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
      let events = AsyncStream<Void> { _ in }
      for await _ in events {}
      throw CancellationError()
    }, load: { .init(accounts: [], result: nil) }, acknowledge: { _ in Issue.record("Unexpected welcome") })
    model.signIn { _ in Issue.record("Cancelled sign-in finished setup") }
    #expect(model.signingIn != nil)
    let pending = model.signingIn
    pending?.cancel()
    await pending?.value
    #expect(model.signingIn == nil && model.welcome == nil)
    #expect(model.message != nil)
  }

  @Test func noPlanPermissionOffersConsentAndDoesNotFinishSetup() async {
    let a = account(plan: false)
    var requests: [Bool] = []
    let model = ChatGPTSignInModel(authenticate: { client, consent in
      requests.append(consent)
      if consent { #expect(client == a.clientId) }
      return .init(account: a, welcome: false)
    }, load: { .init(accounts: [a], result: nil) }, acknowledge: { _ in Issue.record("Unexpected welcome") })
    model.signIn { _ in Issue.record("Plan permission was not granted") }
    await model.signingIn?.value
    #expect(model.active?.planEnabled == false && model.welcome == nil)
    #expect(model.message != nil)
    model.signIn(clientId: a.clientId, consent: true) { _ in Issue.record("Plan permission was not granted") }
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
    model.signIn { _ in finished += 1 }
    await model.signingIn?.value
    #expect(finished == 0 && model.message == "ChatGPT: fixture failure")
    model.signIn { _ in finished += 1 }
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
    setup.signIn { id in
      #expect(id == a.clientId)
      finished += 1
    }
    await setup.signingIn?.value
    #expect(setup.welcome == a.clientId && finished == 0)
    await setup.welcomed()
    await setup.welcomed()
    #expect(finished == 1 && acknowledgements == 1 && setup.welcome == nil)
    let settings = ChatGPTSignInModel(authenticate: authenticate, load: { .init(accounts: [a], result: nil) }, acknowledge: acknowledge)
    settings.signIn { _ in finished += 1 }
    await settings.signingIn?.value
    #expect(settings.welcome == nil && finished == 2 && acknowledgements == 1)
  }

  private func until(_ predicate: @escaping @MainActor () -> Bool) async throws {
    try await waitForModel("sign-in transition", predicate)
  }
  @Observable final class Counter { var value = 0 }

  @Test func aCompletedAttemptUsesItsAccountEvenAfterSelectionChanges() async throws {
    let (changes, change) = AsyncStream<Void>.makeStream()
    let (requests, requested) = AsyncStream<Void>.makeStream()
    var reads = requests.makeAsyncIterator()
    let statusReads = Counter()
    let auth = ChatGPTAuth(endpoints: .openAI, profile: "fixture", call: { method, fields in
      if method == "SignInBeginRequest" {
        #expect(fields["replace_pending"] as? Bool == true)
        return ["attempt_id": "first", "url": "https://fixture.invalid/authorize"]
      }
      #expect(method == "SignInStatusRequest" && fields["attempt_id"] as? String == "first")
      statusReads.value += 1
      if statusReads.value == 1 {
        requested.yield(())
        return ["state": "pending"]
      }
      let accounts: [[String: Any]] = [
        ["clientId": "first", "signedIn": false, "active": false],
        ["clientId": "second", "signedIn": true, "active": true],
      ]
      return ["state": "complete", "client_id": "first", "accounts": accounts, "plan_enabled": true]
    }, changes: { _ in changes })
    let signIn = Task { try await auth.signIn { _ in } }
    _ = await reads.next()
    change.yield(())
    let result = try await signIn.value
    #expect(result.account.clientId == "first")
    #expect(!result.account.signedIn && !result.account.active)
    #expect(result.planEnabled)
  }

  @Test(arguments: [false, true]) func dismissingTheViewCancelsAtTheHost(beginPending: Bool) async throws {
    NSApplication.shared.setActivationPolicy(.prohibited)
    let host = SignInHostDouble(delayBegin: beginPending)
    let auth = host.auth()
    let cancellation = Counter()
    let model = ChatGPTSignInModel(authenticate: { client, consent in
      try await withTaskCancellationHandler {
        try await auth.signIn(clientId: client, consent: consent) { _ in await host.openBrowser() }
      } onCancel: {
        Task { @MainActor in cancellation.value += 1 }
      }
    }, load: { await auth.status() })
    let view = NSHostingView(rootView: AnyView(ChatGPTConnection(model: model, onboarding: true) { _ in
      Issue.record("Dismissed sign-in finished setup")
    }))
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 500, height: 300),
                          styleMask: [.borderless], backing: .buffered, defer: true)
    window.contentView = view
    defer { window.contentView = nil }
    view.layoutSubtreeIfNeeded()
    try await until { host.statusReads > 0 }
    model.signIn { _ in Issue.record("Dismissed sign-in finished setup") }
    let pending = try #require(model.signingIn)
    try await until { host.begins == 1 && (beginPending || host.browserOpens == 1) }
    // Replace the actual SwiftUI subtree, invoking its onDisappear modifier.
    view.rootView = AnyView(EmptyView())
    view.layoutSubtreeIfNeeded()
    try await until { cancellation.value == 1 }
    #expect(pending.isCancelled)
    if beginPending { host.finishBegin() }
    try await until { host.cancels == 1 }
    host.finishCancel()
    await pending.value
    #expect(host.browserOpens == (beginPending ? 0 : 1))
    #expect(model.signingIn == nil)
  }

  @Test(arguments: [false, true]) func terminationWaitsForTheHostCancelReply(beginPending: Bool) async throws {
    let host = SignInHostDouble(delayBegin: beginPending)
    let auth = host.auth()
    let model = ChatGPTSignInModel(authenticate: { client, consent in
      try await auth.signIn(clientId: client, consent: consent) { _ in await host.openBrowser() }
    }, load: { await auth.status() })
    model.signIn { _ in Issue.record("Quit finished setup") }
    let pending = try #require(model.signingIn)
    try await until { host.begins == 1 && (beginPending || host.browserOpens == 1) }
    // onDisappear may already have cancelled the task when Quit is handled.
    pending.cancel()
    let delegate = AppDelegate()
    delegate.activityBeforeQuit = { false }
    delegate.localActivityBeforeQuit = { false }
    // AppModel.shared belongs to the whole test process: Quit would end the
    // sessions other tests have open there, or ask about their unsaved edits
    // in a modal alert that nobody answers.
    delegate.openSessions = { [] }
    delegate.stopRuntime = {}
    let replies = Counter()
    delegate.terminationReply = { allowed in #expect(allowed); replies.value += 1 }
    #expect(delegate.applicationShouldTerminate(NSApplication.shared) == .terminateLater)
    if beginPending {
      #expect(host.cancels == 0 && replies.value == 0)
      host.finishBegin()
    }
    try await until { host.cancels == 1 }
    #expect(replies.value == 0, "Quit must wait for the cancellation acknowledgement")
    #expect(delegate.applicationShouldTerminate(NSApplication.shared) == .terminateLater)
    host.finishCancel()
    await pending.value
    try await until { replies.value == 1 }
    #expect(host.browserOpens == (beginPending ? 0 : 1))
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
      model.signIn { try await guide.finishChatGPTSetup(clientId: $0) }
      await model.signingIn?.value
      #expect(model.welcome != nil && !guide.setupDone)
      // Reopening the app creates a new presentation model; hostd survives.
      let reopened = ChatGPTSignInModel(authenticate: authenticate)
      await reopened.refresh { try await guide.finishChatGPTSetup(clientId: $0) }
      #expect(reopened.welcome == model.welcome)
      await reopened.welcomed()
      let persisted = try await CoreHostClient.shared.call("SettingsGetRequest", ["profile": "desktop"])
      let entries = persisted["values"] as? [[String: String]]
      let provider = entries?.first { $0["key"] == "agent.provider" }?["value_json"]
      try #require(provider == #""chatgpt""#)
      let welcomeError = reopened.message
      try #require(welcomeError == nil)
      try await fixtures.until("ChatGPT finishes setup and connects the guide") { guide.setupDone && guide.connected }
      #expect(model.active?.email == "fixture@example.com")
      let id = try #require(model.active?.clientId)
      let settings = ChatGPTSignInModel(authenticate: authenticate)
      settings.signIn(clientId: id) { _ in }
      await settings.signingIn?.value
      #expect(settings.active?.planEnabled == true && settings.welcome == nil)
    }
  }
}

/// RPC boundary double: no browser, network, credential store or resident service.
@MainActor @Observable private final class SignInHostDouble {
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
    }, changes: { _ in AsyncStream<Void> { _ in } })
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
