import AppKit
import Foundation
import SwiftUI
import Testing
@testable import TsugumiAgentClient
@testable import TsugumiApp

/// Presentation doubles never call OAuth, Keychain, the browser or a model.
@MainActor @Suite struct ChatGPTSetupTests {
  private func account(plan: Bool = true) -> ChatGPTAccountInfo {
    .init(clientId: "fixture", email: "fixture@example.com", signedIn: true, planEnabled: plan, active: true)
  }

  @Test func signInWaitsForTheBrowserAndCanBeCancelled() async {
    let model = ChatGPTSignInModel(authenticate: { _, _ in
      try await Task.sleep(for: .seconds(30))
      throw CancellationError()
    }, load: { [] }, acknowledge: { _ in Issue.record("Unexpected welcome") })
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
    }, load: { [a] }, acknowledge: { _ in Issue.record("Unexpected welcome") })
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
    }, load: { [a] }, acknowledge: { _ in })
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
    let setup = ChatGPTSignInModel(authenticate: authenticate, load: { [a] }, acknowledge: acknowledge)
    var finished = 0
    setup.signIn { finished += 1 }
    await setup.signingIn?.value
    #expect(setup.welcome == a.clientId && finished == 0)
    await setup.welcomed()
    await setup.welcomed()
    #expect(finished == 1 && acknowledgements == 1 && setup.welcome == nil)
    let settings = ChatGPTSignInModel(authenticate: authenticate, load: { [a] }, acknowledge: acknowledge)
    settings.signIn { finished += 1 }
    await settings.signingIn?.value
    #expect(settings.welcome == nil && finished == 2 && acknowledgements == 1)
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
      await model.welcomed()
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
