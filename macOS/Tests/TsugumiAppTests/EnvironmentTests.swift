import Testing
@testable import TsugumiRPC
@testable import TsugumiApp

@Test func recordedStorePathsAreShownByPackageName() {
  let hash = String(repeating: "a", count: 32)
  #expect(recordedStoreName("/nix/store/\(hash)-python-3.13") == "python-3.13")
  // Anything that is not a store hash and a name is shown as it is.
  #expect(recordedStoreName("/nix/store/short-name") == "short-name")
  #expect(recordedStoreName("/nix/store/\(String(repeating: "A", count: 32))-upper") == "\(String(repeating: "A", count: 32))-upper")
  #expect(recordedStoreName("") == "")
}

@MainActor struct RecordedEnvironmentBehaviorTests {
  @Test func navigationFindsRecordedEnvironmentsAndMissingRecords() async throws {
    var calls: [String] = []
    let source = EnvironmentHistorySource(environments: { request in
      calls.append("\(request.runId):\(request.offset ?? 0)")
      let page = Tsugumi_RPC_BuildEnvironmentPageT()
      let environment = Tsugumi_RPC_BuildEnvironmentT()
      environment.id = request.offset == 0 ? "shared" : "other"
      environment.system = "aarch64-linux"
      environment.manifest = "/nix/store/recorded-manifest"
      environment.derivation = "/nix/store/recorded.drv"
      page.environments = [environment]
      page.nextOffset = request.offset == 0 ? 100 : nil
      return page
    }, paths: { _ in Issue.record("Navigation must not load closure paths"); return Tsugumi_RPC_EnvironmentPathPageT() },
       uses: { _ in Issue.record("Navigation must not load every invocation"); return Tsugumi_RPC_EnvironmentUsePageT() })
    #expect(try await source.find(.init(run: "before-restart", id: "shared"))?.manifest == "/nix/store/recorded-manifest")
    #expect(calls == ["before-restart:0"])
    calls = []
    #expect(try await source.find(.init(run: "imported", id: "other"))?.derivation == "/nix/store/recorded.drv")
    #expect(calls == ["imported:0", "imported:100"])
    calls = []
    #expect(try await source.find(.init(run: "changed-definitions", id: "missing")) == nil)
    #expect(calls == ["changed-definitions:0", "changed-definitions:100"])
    calls = []
    #expect(try await source.find(.init(run: "imported", id: "")) == nil)
    #expect(calls.isEmpty)
  }

  @Test func dependenciesAndSharedUsersArePagedAndSearchUsesRequestedOffset() async throws {
    var pathCalls: [Tsugumi_RPC_RunEnvironmentPathsRequestT] = []
    var useCalls: [UInt64] = []
    let source = EnvironmentHistorySource(environments: { _ in Tsugumi_RPC_BuildEnvironmentPageT() }, paths: { request in
      pathCalls.append(request)
      let page = Tsugumi_RPC_EnvironmentPathPageT()
      page.paths = [request.excludingRunId == "left" ? "/nix/store/added" : "/nix/store/removed"]
      page.total = 201; page.nextOffset = request.offset == 0 ? 100 : nil
      return page
    }, uses: { request in
      useCalls.append(request.offset ?? 0)
      let use = Tsugumi_RPC_EnvironmentUseT()
      use.card = request.offset == 0 ? "removed-card" : "other-card"; use.invocation = 9
      let page = Tsugumi_RPC_EnvironmentUsePageT()
      page.uses = [use]; page.nextOffset = request.offset == 0 ? 100 : nil
      return page
    })
    let left = RecordedEnvironmentIdentity(run: "left", id: "old"), right = RecordedEnvironmentIdentity(run: "right", id: "new")
    let first = try await source.paths(left, offset: 0, query: "", excluding: right)
    #expect(first.paths == ["/nix/store/removed"] && first.nextOffset == 100)
    let second = try await source.paths(left, offset: first.nextOffset!, query: "", excluding: right)
    #expect(second.nextOffset == nil)
    _ = try await source.paths(left, offset: 0, query: "Python", excluding: right)
    #expect(pathCalls.map { $0.offset } == [0, 100, 0])
    #expect(pathCalls.last?.query == "Python" && pathCalls.last?.limit == 100)
    #expect(pathCalls.last?.excludingRunId == "right" && pathCalls.last?.excludingEnvironmentId == "new")
    #expect(try await source.paths(right, offset: 0, query: "", excluding: left).paths == ["/nix/store/added"])
    let users = try await source.uses(left, offset: 0)
    #expect(users.uses.first??.card == "removed-card")
    let moreUsers = try await source.uses(left, offset: users.nextOffset!)
    #expect(moreUsers.uses.first??.card == "other-card" && moreUsers.nextOffset == nil)
    #expect(useCalls == [0, 100])
    #expect(EnvironmentHistorySource.change("old", "new") == .changed)
    #expect(EnvironmentHistorySource.change("old", "old") == .same)
    #expect(EnvironmentHistorySource.change("", "new") == .unknown)
  }

  @Test func nonAdvancingPagesFailRatherThanLoop() async throws {
    let source = EnvironmentHistorySource(environments: { _ in
      let page = Tsugumi_RPC_BuildEnvironmentPageT(); page.nextOffset = 0; return page
    }, paths: { _ in
      let page = Tsugumi_RPC_EnvironmentPathPageT(); page.nextOffset = 100; return page
    }, uses: { _ in
      let page = Tsugumi_RPC_EnvironmentUsePageT(); page.nextOffset = 0; return page
    })
    let identity = RecordedEnvironmentIdentity(run: "run", id: "environment")
    await #expect(throws: EnvironmentPageError.self) { try await source.find(identity) }
    await #expect(throws: EnvironmentPageError.self) { try await source.paths(identity, offset: 100, query: "") }
    await #expect(throws: EnvironmentPageError.self) { try await source.uses(identity, offset: 0) }
  }
}
