import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiAgentClient
@testable import TsugumiLauncher

@Test func anExecutionServiceShowsItsIdOnlyWhenItDiffersFromItsName() {
  #expect(ExecutionServiceConnection.label(name: "local", id: "local") == "local")
  #expect(ExecutionServiceConnection.label(name: "", id: "gpu") == "gpu")
  #expect(ExecutionServiceConnection.label(name: "Lab GPU", id: "gpu") == "Lab GPU (gpu)")
}

/// After 登録 the form closes, so the same worker is not registered twice; a saved edit stays open.
@MainActor @Test func aNewRegistrationClosesTheForm() {
  let capabilities = ExecutionServiceCapabilities(serverId: "", nativeSystem: "aarch64-linux", systems: [], isolation: "",
                                                  resourceLimits: false, maxConcurrentJobs: 1, workerContract: 0, revision: "")
  let gpu = ExecutionServiceConnection(id: "gpu", name: "GPU", endpoint: "tls://gpu:8765", workerPublicKey: "ed25519:x",
                                       probes: [], capabilities: capabilities, outdated: false)
  #expect(ExecutionServicesSettings.editedAfterSave(registering: true, id: "gpu", in: [gpu]) == nil)
  #expect(ExecutionServicesSettings.editedAfterSave(registering: false, id: "gpu", in: [gpu]) == gpu)
  #expect(ExecutionServicesSettings.editedAfterSave(registering: false, id: "gone", in: [gpu]) == nil)
}

/// A worker hostd found built from another revision says so, with both contracts and what to do.
@MainActor @Test func anOutOfDateWorkerSaysHowToBringItUpToDate() {
  let capabilities = ExecutionServiceCapabilities(serverId: "server", nativeSystem: "x86_64-linux", systems: ["x86_64-linux"],
                                                  isolation: "pid_namespace", resourceLimits: false, maxConcurrentJobs: 1,
                                                  workerContract: 21, revision: "0ld0rev")
  let ttl = ExecutionServiceConnection(id: "ttl", name: "ttl", endpoint: "tls://ttl:8765", workerPublicKey: "ed25519:x",
                                       probes: [], capabilities: capabilities, outdated: true)
  let worker = "21", current = "22"
  #expect(ExecutionServicesSettings.outdatedText(ttl, contract: 22)
          == String(localized: "このワーカーは古い版です（契約 \(worker)、この Tsugumi は \(current)）。ワーカーを同じ版で作り直して再起動し、「接続を確認して更新」を押してください。"))
  let current22 = ExecutionServiceConnection(id: "ttl", name: "ttl", endpoint: "tls://ttl:8765", workerPublicKey: "ed25519:x",
                                             probes: [], capabilities: capabilities, outdated: false)
  #expect(ExecutionServicesSettings.outdatedText(current22, contract: 22) == nil)
}

/// A service whose stored record hostd cannot read says so on its own row, with what to do.
@MainActor @Test func anUnreadableRecordSaysHowToRecordItAgain() {
  let capabilities = ExecutionServiceCapabilities(serverId: "", nativeSystem: "", systems: [], isolation: "pid_namespace",
                                                  resourceLimits: false, maxConcurrentJobs: 0, workerContract: 0, revision: "")
  let error = "The stored capabilities of worker ttl cannot be read (invalid remote worker FlatBuffer)"
  let ttl = ExecutionServiceConnection(id: "ttl", name: "ttl", endpoint: "tls://ttl:8765", workerPublicKey: "ed25519:x",
                                       probes: [], capabilities: capabilities, outdated: false, recordError: error)
  #expect(ExecutionServicesSettings.recordErrorText(ttl)
          == String(localized: "保存された記録を読めません（\(error)）。「接続を確認して更新」を押すと記録し直します。"))
  let readable = ExecutionServiceConnection(id: "ttl", name: "ttl", endpoint: "tls://ttl:8765", workerPublicKey: "ed25519:x",
                                            probes: [], capabilities: capabilities, outdated: false)
  #expect(ExecutionServicesSettings.recordErrorText(readable) == nil)
}

@MainActor @Test func builtInSkillsHaveTheirOwnDescriptions() {
  for path in ["builtin:helpers", "builtin:review", "builtin:rerun-compare"] {
    let description = ChatController.builtinSkillDescription(path)
    #expect(description?.isEmpty == false, "\(path)")
    #expect(description != path.replacingOccurrences(of: "builtin:", with: ""))
  }
  #expect(ChatController.builtinSkillDescription("/skills/custom.md") == nil)
}

@Test func theChosenConnectionIsInUseOnlyWhenItCanBeUsed() {
  #expect(ConnectionChoiceBadge.kind(selected: true, ready: true) == .inUse)
  #expect(ConnectionChoiceBadge.kind(selected: true, ready: false) == .notSetUp)
  #expect(ConnectionChoiceBadge.kind(selected: true, ready: nil) == nil)
  #expect(ConnectionChoiceBadge.kind(selected: false, ready: false) == nil)
}

@Test func anUnsupportedMacSaysWhy() {
  let reasons = [
    VirtualizationSupport.reason(appleSilicon: false, hypervisor: false),
    VirtualizationSupport.reason(appleSilicon: true, hypervisor: false),
    VirtualizationSupport.reason(appleSilicon: true, hypervisor: true),
  ]
  #expect(Set(reasons).count == 3)
  #expect(reasons.allSatisfy { !$0.isEmpty })
}

@MainActor @Test func theSetupCheckEndsInADefiniteStateAfterABoundedWait() async throws {
  let model = AppModel(refreshOnInit: false, requestHome: { _, _ in [:] })
  var answer: CheckedContinuation<EnvironmentManager.SetupStatus, Never>?
  model.setupWait = .milliseconds(10)
  model.checkSetup = { await withCheckedContinuation { answer = $0 } }
  let refresh = Task { await model.refreshSetup() }
  try await waitForModel("no answer within the wait") {
    if case .failed? = model.setup { return true } else { return false }
  }
  #expect(model.setupNeeded)
  #expect(model.checkingSetup)
  // A late answer still replaces the failure.
  for _ in 0..<100 where answer == nil { await Task.yield() }
  answer?.resume(returning: .checked(virtualizationSupported: true, imageAvailable: true, imageVersion: "image"))
  await refresh.value
  #expect(model.setup == .checked(virtualizationSupported: true, imageAvailable: true, imageVersion: "image"))
  #expect(!model.setupNeeded && !model.checkingSetup)
}
