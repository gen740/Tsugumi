import Foundation
import TsugumiRPC

struct RecordedEnvironmentIdentity: Equatable {
  let run: String
  let id: String
}

/// Public history RPC only. Injectable readers keep paging and navigation testable without Nix.
@MainActor struct EnvironmentHistorySource {
  var environments: (Tsugumi_RPC_RunEnvironmentsRequestT) async throws -> Tsugumi_RPC_BuildEnvironmentPageT
  var paths: (Tsugumi_RPC_RunEnvironmentPathsRequestT) async throws -> Tsugumi_RPC_EnvironmentPathPageT
  var uses: (Tsugumi_RPC_RunEnvironmentUsesRequestT) async throws -> Tsugumi_RPC_EnvironmentUsePageT

  init(rpc: RPCClient) {
    environments = { try await rpc.call(.runenvironmentsrequest, $0) }
    paths = { try await rpc.call(.runenvironmentpathsrequest, $0) }
    uses = { try await rpc.call(.runenvironmentusesrequest, $0) }
  }

  init(environments: @escaping (Tsugumi_RPC_RunEnvironmentsRequestT) async throws -> Tsugumi_RPC_BuildEnvironmentPageT,
       paths: @escaping (Tsugumi_RPC_RunEnvironmentPathsRequestT) async throws -> Tsugumi_RPC_EnvironmentPathPageT,
       uses: @escaping (Tsugumi_RPC_RunEnvironmentUsesRequestT) async throws -> Tsugumi_RPC_EnvironmentUsePageT) {
    self.environments = environments; self.paths = paths; self.uses = uses
  }

  func page(run: String, offset: UInt64) async throws -> Tsugumi_RPC_BuildEnvironmentPageT {
    try Task.checkCancellation()
    let request = Tsugumi_RPC_RunEnvironmentsRequestT()
    request.runId = run; request.offset = offset; request.limit = 100
    let page = try await environments(request)
    try Task.checkCancellation()
    if let next = page.nextOffset, next <= offset { throw EnvironmentPageError.nonAdvancing }
    return page
  }

  func find(_ identity: RecordedEnvironmentIdentity) async throws -> Tsugumi_RPC_BuildEnvironmentT? {
    guard !identity.id.isEmpty else { return nil }
    var offset: UInt64? = 0
    while let current = offset {
      let result = try await page(run: identity.run, offset: current)
      if let found = result.environments.compactMap({ $0 }).first(where: { $0.id == identity.id }) { return found }
      offset = result.nextOffset
    }
    return nil
  }

  func paths(_ identity: RecordedEnvironmentIdentity, offset: UInt64, query: String,
             excluding: RecordedEnvironmentIdentity? = nil) async throws -> Tsugumi_RPC_EnvironmentPathPageT {
    try Task.checkCancellation()
    let request = Tsugumi_RPC_RunEnvironmentPathsRequestT()
    request.runId = identity.run; request.environmentId = identity.id
    request.offset = offset; request.limit = 100; request.query = query
    request.excludingRunId = excluding?.run; request.excludingEnvironmentId = excluding?.id
    let page = try await paths(request)
    try Task.checkCancellation()
    if let next = page.nextOffset, next <= offset { throw EnvironmentPageError.nonAdvancing }
    return page
  }

  func uses(_ identity: RecordedEnvironmentIdentity, offset: UInt64) async throws -> Tsugumi_RPC_EnvironmentUsePageT {
    try Task.checkCancellation()
    let request = Tsugumi_RPC_RunEnvironmentUsesRequestT()
    request.runId = identity.run; request.environmentId = identity.id; request.offset = offset; request.limit = 100
    let page = try await uses(request)
    try Task.checkCancellation()
    if let next = page.nextOffset, next <= offset { throw EnvironmentPageError.nonAdvancing }
    return page
  }

  static func change(_ left: String, _ right: String) -> ComparedDifference {
    left.isEmpty || right.isEmpty ? .unknown : left == right ? .same : .changed
  }
}

enum EnvironmentPageError: Error { case nonAdvancing }
