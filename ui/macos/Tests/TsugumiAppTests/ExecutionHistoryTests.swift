import TsugumiVMRuntime
import AppKit
import SwiftUI
import Darwin
import Foundation
import Observation
import FlatBuffers
import Testing
@testable import TsugumiLauncher
@testable import TsugumiApp
@testable import TsugumiRPC

// Serialize with the fixtures that temporarily override the core service state directory.
extension ChatResumeTests {
  @MainActor @Test(arguments: ["incompatible_history", "invalid_document"])
  func startupFailurePreservesTheDiagnostic(kind: String) async throws {
    let dir = try SyncTests.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let support = dir.appendingPathComponent("test-support")
    try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
    let identity = try ProjectIdentity(url: dir)
    let path = "definitions/record/_card.json"
    let validationMessage = "\(path): workerCapabilities is not allowed on type code"
    // Model hostd's public Host contract; the UI never speaks to VMHost.
    let socket = VMHostIPC.socketPath(support: support)
    let listener = try VMHostIPC.listen(path: socket)
    defer { Darwin.close(listener); unlink(socket) }
    let state = support.appendingPathComponent("host-core")
    try FileManager.default.createDirectory(at: state, withIntermediateDirectories: true)
    try JSONSerialization.data(withJSONObject: ["protocol": 1, "pid": getpid(),
      "unix_socket": socket, "host_socket": socket, "user_token": "fixture"])
      .write(to: state.appendingPathComponent("host.ready"))
    let host = Task {
      try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Int, Error>) in
        Thread.detachNewThread {
          continuation.resume(with: Result {
            func acceptPeer() throws -> Int32 {
              var incoming = pollfd(fd: listener, events: Int16(POLLIN), revents: 0)
              guard poll(&incoming, 1, 60_000) == 1 else { throw POSIXError(.ETIMEDOUT) }
              let peer = accept(listener, nil, nil)
              guard peer >= 0 else { throw POSIXError(.ECONNABORTED) }
              var timeout = timeval(tv_sec: 60, tv_usec: 0)
              setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
              return peer
            }
            let hello = Tsugumi_Service_HelloResultT()
            hello.serverId = "history-fixture"; hello.principal = "user"
            // CoreHostClient authenticates both its Agent and Host connections.
            let agent = try acceptPeer()
            defer { Darwin.close(agent) }
            guard let agentBytes = try VMHostIPC.read(agent) else { throw POSIXError(.EPROTO) }
            var agentBuffer = ByteBuffer(data: agentBytes)
            let agentRoot: Tsugumi_Agent_Message = try getCheckedRoot(byteBuffer: &agentBuffer, fileId: "TSA1")
            let agentRequest = Tsugumi_Agent_MessageT(agentRoot)
            guard (agentRequest.payload?.value as? Tsugumi_Agent_RequestT)?.value?.type == .hellorequest else { throw POSIXError(.EPROTO) }
            let agentReply = Tsugumi_Agent_ReplyT(); agentReply.value = Tsugumi_Agent_ReplyPayloadUnion(hello, type: .helloresult)
            let agentMessage = Tsugumi_Agent_MessageT(); agentMessage.requestId = agentRequest.requestId
            agentMessage.payload = Tsugumi_Agent_PayloadUnion(agentReply, type: .reply)
            var builder = FlatBufferBuilder(initialSize: 256); var agentObject: Tsugumi_Agent_MessageT? = agentMessage
            let offset = Tsugumi_Agent_Message.pack(&builder, obj: &agentObject)
            Tsugumi_Agent_Message.finish(&builder, end: offset)
            try VMHostIPC.write(builder.data, to: agent)

            let peer = try acceptPeer()
            defer { Darwin.close(peer) }
            var attempts = 0
            var latest = Tsugumi_Host_EnvironmentStateT()
            latest.project = identity.url.path; latest.key = identity.id; latest.phase = .stopped
            while attempts < 1 {
              guard let bytes = try VMHostIPC.read(peer) else { throw POSIXError(.EPROTO) }
              var buffer = ByteBuffer(data: bytes)
              let root: Tsugumi_Host_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSH1")
              let message = Tsugumi_Host_MessageT(root)
              guard let request = message.payload?.value as? Tsugumi_Host_RequestT else { throw POSIXError(.EPROTO) }
              let reply = Tsugumi_Host_ReplyT()
              if request.value?.type == .hellorequest {
                reply.value = Tsugumi_Host_ReplyPayloadUnion(hello, type: .helloresult)
              } else if let input = request.value?.value as? Tsugumi_Host_EnvironmentRequestT {
                guard input.project == identity.url.path else { throw POSIXError(.EPROTO) }
                if input.action == .start {
                  let environment = Tsugumi_Host_EnvironmentStateT()
                  environment.project = identity.url.path; environment.key = identity.id
                  attempts += 1
                  environment.phase = .failed; environment.message = "cannot open history"
                  let diagnostic = Tsugumi_Host_StartupErrorT()
                  diagnostic.kind = kind
                  if kind == "incompatible_history" {
                    diagnostic.found = "2"; diagnostic.expected = "3"
                  } else {
                    diagnostic.path = path; diagnostic.message = validationMessage
                  }
                  environment.startupError = diagnostic
                  latest = environment
                } else {
                  guard input.action == .state else { throw POSIXError(.EPROTO) }
                }
                reply.value = Tsugumi_Host_ReplyPayloadUnion(latest, type: .environmentstate)
              } else { throw POSIXError(.EPROTO) }
              let response = Tsugumi_Host_MessageT(); response.requestId = message.requestId
              response.payload = Tsugumi_Host_PayloadUnion(reply, type: .reply)
              builder = FlatBufferBuilder(initialSize: 512); var object: Tsugumi_Host_MessageT? = response
              let result = Tsugumi_Host_Message.pack(&builder, obj: &object)
              Tsugumi_Host_Message.finish(&builder, end: result)
              try VMHostIPC.write(builder.data, to: peer)
            }
            return attempts
          })
        }
      }
    }
    let session = ProjectSession(identity: identity, environments: EnvironmentManager(support: support, image: nil))
    session.start()
    try await waitForModel {
      guard EnvironmentOverlay.hasProblem(session), session.connectionMessage != nil else { return false }
      switch (kind, session.environment) {
      case ("incompatible_history", .incompatibleHistory), ("invalid_document", .invalidDocument): return true
      default: return false
      }
    }
    switch (kind, session.environment) {
    case ("incompatible_history", .incompatibleHistory(let mismatch)):
      #expect(mismatch.found == "2")
      #expect(mismatch.expected == "3")
    case ("invalid_document", .invalidDocument(let file, let message, _)):
      #expect(file == path)
      #expect(message == validationMessage)
    default:
      Issue.record("Expected a typed startup error for \(kind)")
    }
    #expect(try await host.value == 1)
    await session.close()
  }
}

@Test func executionHistoryFindsTheActiveRunWithoutReplacingAHistoricalSelection() {
  let runs = ["active", "stopped", "older"]
  #expect(ExecutionHistorySelection.run(nil, available: runs, active: "active") == "active")
  #expect(ExecutionHistorySelection.run("stopped", available: runs, active: "active") == "stopped")
  #expect(ExecutionHistorySelection.run("deleted", available: runs, active: "active") == "active")
  #expect(ExecutionHistorySelection.run(nil, available: ["stopped"], active: "unrelated") == "stopped")
  #expect(ExecutionHistorySelection.run(nil, available: [], active: "unrelated") == nil)
}

@Test func executionHistoryFollowsNewInvocationsButPreservesAnEarlierCall() {
  #expect(ExecutionHistorySelection.invocation(nil, previous: [], available: [1]) == 1)
  #expect(ExecutionHistorySelection.invocation(1, previous: [1], available: [1, 2]) == 2)
  #expect(ExecutionHistorySelection.invocation(1, previous: [1, 2], available: [1, 2, 3]) == 1)
  #expect(ExecutionHistorySelection.invocation(3, previous: [1, 2, 3], available: []) == nil)
}

@Test func executionHistoryDetectsSilentBuildAndTerminationTransitions() {
  var building = CardRunState(state: .running, invocation: 1, phase: .build)
  building.stages = [StageRecord(stage: .build, outcome: .running, start: 1)]
  var executing = building
  executing.phase = .execute
  executing.stages = [StageRecord(stage: .build, outcome: .cached, start: 1, end: 2),
                      StageRecord(stage: .execute, outcome: .running, start: 2)]
  #expect(building != executing)
  var finished = executing
  finished.state = .succeeded
  finished.endedAt = 3
  finished.stages[1].outcome = .done
  finished.stages[1].end = 3
  #expect(executing != finished)
  var cancelled = building
  cancelled.state = .cancelled
  cancelled.stages[0].outcome = .cancelled
  #expect(building != cancelled)
}

// Its project service and its save live in the host scope's storage, which
// another test's scope would otherwise select and remove under it.
@MainActor @Test(.hostService) func executionHistoryRefreshesAfterARunWithoutStandardOutput() async throws {
  let dir = try SyncTests.project()
  defer { try? FileManager.default.removeItem(at: dir) }
  let daemon = try SyncTests.Daemon(project: dir)
  defer { daemon.stop() }
  let session = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
  session.endpointOverride = daemon.endpoint
  session.start()
  try await SyncTests.connected(session)
  session.deleteCards(["a", "b", "c"], in: [])
  await session.save()
  #expect(session.saveError == nil)
  let before = ExecutionHistoryRefresh(session: session, card: "start")
  try await session.run(card: ["start"], propagate: true)
  try await waitForModel { !(session.runActive) }
  #expect(!session.runActive)
  #expect(session.runStatus?.state == .succeeded)
  let after = ExecutionHistoryRefresh(session: session, card: "start")
  #expect(before != after)
  #expect(after.card?.state == .succeeded)
  let output = try await session.rpc!.logRange(runId: after.runId!, card: ["start"], invocation: 1,
                                                stream: .output, offset: 0, limit: 1024)
  #expect(output.text.isEmpty)
  let history = try await session.rpc!.history(card: ["start"], limit: 100)
  #expect(history.runs.first??.status.runId == after.runId)
  #expect(history.runs.first??.status.state == .succeeded)
  await session.close()
}

/// These hosts stay offscreen: no application launch, activation or saved settings.
@MainActor @Suite(.serialized) struct ExecutionHistoryLayoutTests {
  @Observable final class Measurement {
    var frames: [String: NSView] = [:]
    var controls: [String: CGRect] = [:]
  }
  @Test(arguments: [860.0, 760.0, 620.0], ["ja", "en"])
  func panesUseTheAvailableCardWindow(width: Double, language: String) async throws {
    NSApplication.shared.setActivationPolicy(.prohibited)
    let measurement = Measurement()
    var frames: [String: NSView] { measurement.frames }
    var controlFrames: [String: CGRect] { measurement.controls }
    func probe(_ name: String) -> HistoryLayoutProbe {
      HistoryLayoutProbe { measurement.frames[name] = $0 }
    }
    let header = ExecutionHistoryDetailHeader {
      ExecutionHistoryInvocationPicker(invocations: Self.invocations, selection: .constant(2), paged: true)
      Text("×3").fixedSize()
      Button {} label: { Image(systemName: "chevron.left") }
      Button {} label: { Image(systemName: "chevron.right") }
    } trailing: {
      Button("ログ") {}
    }
    let host = NSHostingView(rootView:
      HStack(spacing: 0) {
        Color.clear.frame(width: 161)
        ExecutionHistoryPanes {
          List {
            ExecutionHistoryRunRow(run: Self.run, target: String(repeating: "nustar_pipeline – RCW 86 / ", count: 12))
          }.background(probe("runs"))
        } detail: {
          VStack(spacing: 0) {
            header.background(probe("header"))
              .overlayPreferenceValue(ExecutionHistoryControlBounds.self) { anchors in
                GeometryReader { geometry in
                  Color.clear.preference(key: HistoryMeasuredBounds.self, value: anchors.mapValues { geometry[$0] })
                }
              }
              .onPreferenceChange(HistoryMeasuredBounds.self) { measurement.controls = $0 }
            Color.clear
          }.background(probe("detail"))
        }
      }.environment(\.locale, Locale(identifier: language))
    )
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: width, height: 500),
                          styleMask: [.borderless], backing: .buffered, defer: true)
    window.contentView = host
    defer { window.contentView = nil }
    host.layoutSubtreeIfNeeded()
    try await waitForModel {
      Set(measurement.frames.keys).isSuperset(of: ["runs", "detail", "header"]) &&
        Set(measurement.controls.keys).isSuperset(of: ["label", "invocation"])
    }
    func rect(_ name: String) throws -> CGRect {
      let view = try #require(frames[name])
      return view.convert(view.bounds, to: host)
    }
    let runs = try rect("runs"), detail = try rect("detail"), headerRect = try rect("header")
    for rect in [runs, detail, headerRect] {
      #expect(rect.minX >= 160 && rect.maxX <= width + 1)
      #expect(rect.minY >= -1 && rect.maxY <= 501)
    }
    for rect in controlFrames.values {
      #expect(rect.minX >= -1 && rect.maxX <= headerRect.width + 1, "A history control is clipped")
      #expect(rect.height <= 30, "A history control must stay on one line")
    }
    #expect(headerRect.height <= 64, "The invocation label must not become a column of characters")
    #expect(detail.height >= 280, "The controls must leave room for the invocation's content")
    if width >= 801 {
      #expect(runs.maxX <= detail.minX + 1)
      #expect(runs.height >= 490, "The history must take the height, not leave it above a toolbar")
    } else {
      #expect(runs.width >= width - 162 && runs.height <= 160)
      #expect(detail.width >= width - 162)
    }
  }

  @Test(arguments: [180.0, 320.0, 420.0], [0, 1, 3])
  func headerFitsWithoutClipping(width: Double, invocationCount: Int) async throws {
    NSApplication.shared.setActivationPolicy(.prohibited)
    let host = NSHostingView(rootView:
      ExecutionHistoryDetailHeader {
        if invocationCount == 0 {
          Label("呼び出しの記録を待っています", systemImage: "clock").lineLimit(1)
        } else {
          ExecutionHistoryInvocationPicker(invocations: Array(Self.invocations.prefix(invocationCount)), selection: .constant(1))
        }
      } trailing: {
        Button("ログ") {}
      }.frame(width: width)
    )
    host.layoutSubtreeIfNeeded()
    #expect(host.fittingSize.width <= width + 1)
    #expect(host.fittingSize.height <= 64)
  }

  private static var run: Tsugumi_RPC_RunSummaryT {
    let run = Tsugumi_RPC_RunSummaryT()
    run.status.state = .running
    run.status.acceptedAt = 1794755053000
    return run
  }

  private static var invocations: [Tsugumi_RPC_InvocationSummaryT] {
    (1...3).map { number in
      let item = Tsugumi_RPC_InvocationSummaryT()
      item.invocation = UInt64(number); item.state = .succeeded; item.durationMs = 12345
      item.batchIndex = UInt32(number); item.batchSize = 3
      return item
    }
  }
}

private struct HistoryLayoutProbe: NSViewRepresentable {
  let capture: (NSView) -> Void
  func makeNSView(context: Context) -> NSView {
    let view = NSView()
    capture(view)
    return view
  }
  func updateNSView(_ view: NSView, context: Context) {}
}

private struct HistoryMeasuredBounds: PreferenceKey {
  static var defaultValue: [String: CGRect] { [:] }
  static func reduce(value: inout [String: CGRect], nextValue: () -> [String: CGRect]) {
    value.merge(nextValue(), uniquingKeysWith: { _, next in next })
  }
}
