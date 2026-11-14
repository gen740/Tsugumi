import Foundation
import Testing
@testable import TsugumiApp

@MainActor private final class HomeReplies {
  var pending: [String: CheckedContinuation<[String: Any], Error>] = [:]
  var calls: [String: Int] = [:]
  func request(_ method: String, _ fields: [String: Any]) async throws -> [String: Any] {
    calls[method, default: 0] += 1
    return try await withCheckedThrowingContinuation { pending[method] = $0 }
  }
  func answer(_ method: String, _ value: [String: Any]) { pending.removeValue(forKey: method)?.resume(returning: value) }
}

@MainActor @Test func homeReadsRunConcurrentlyAndPublishRecentsBeforeCatalog() async {
  let replies = HomeReplies()
  let model = AppModel(refreshOnInit: false, requestHome: replies.request)
  #expect(!model.homeLoaded && !model.demosLoaded)
  let first = Task { await model.refreshHome() }
  // Yield to the requests, without wall-clock sleeps or any service.
  for _ in 0..<100 where replies.pending.count < 3 { await Task.yield() }
  #expect(replies.pending.count == 3)
  var joining = false
  let joined = Task { joining = true; await model.refreshHome() }
  for _ in 0..<100 where !joining { await Task.yield() }
  replies.answer("HostRecentListRequest", ["value": [["path": "/project", "name": "Project", "openedAt": 0]]])
  for _ in 0..<100 where !model.homeLoaded { await Task.yield() }
  #expect(model.homeLoaded && model.recents.first?.name == "Project")
  #expect(!model.demosLoaded)
  replies.answer("HostDemoListRequest", ["value": []])
  replies.answer("HostProjectDefaultsRequest", ["nixpkgsPin": "pin"])
  await first.value
  await joined.value
  #expect(model.demosLoaded && model.nixpkgsPin == "pin")
  #expect(replies.calls.values.allSatisfy { $0 == 1 })
  #expect(!model.setupNeeded) // Unknown is never reported as missing.
}

@MainActor @Test func firstHomeJoinsEvenAnAlreadyCompletedInitialRead() async {
  var calls = 0
  let model = AppModel(requestHome: { method, _ in
    calls += 1
    return method == "HostProjectDefaultsRequest" ? ["nixpkgsPin": "pin"] : ["value": []]
  })
  await model.initialRefresh?.value
  await model.showHome()
  #expect(calls == 3)
  #expect(model.homeLoaded && model.demosLoaded && model.initialRefresh == nil)
}

@MainActor @Test func aFailedRecentListDoesNotPretendToBeEmptyOrBlockDemos() async {
  let model = AppModel(refreshOnInit: false, requestHome: { method, _ in
    if method == "HostRecentListRequest" { throw CocoaError(.fileReadNoPermission) }
    return method == "HostProjectDefaultsRequest" ? ["nixpkgsPin": "pin"] : ["value": []]
  })
  await model.refreshHome()
  #expect(!model.homeLoaded && model.recentsError != nil)
  #expect(model.demosLoaded && model.nixpkgsPin == "pin")
  #expect(!model.setupNeeded)
}
