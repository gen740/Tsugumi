import Foundation
import TsugumiRPC

public enum HostViewInput: Sendable {
  case pointer(kind: UInt32, x: Float, y: Float, buttons: UInt32, wheelX: Float, wheelY: Float, modifiers: UInt32)
  case key(kind: UInt32, code: UInt32, modifiers: UInt32)
  case text(String)
  case gamepad(pad: UInt32, kind: UInt32, buttons: UInt32, lx: Float, ly: Float, rx: Float, ry: Float, lt: Float, rt: Float)

  init?(_ action: Tsugumi_Host_ViewActionUnion?) {
    switch action?.value {
    case let value as Tsugumi_Host_ViewPointerT:
      self = .pointer(kind: value.kind, x: value.x, y: value.y, buttons: value.buttons,
                      wheelX: value.wheelX, wheelY: value.wheelY, modifiers: value.modifiers)
    case let value as Tsugumi_Host_ViewKeyT:
      self = .key(kind: value.kind, code: value.code, modifiers: value.modifiers)
    case let value as Tsugumi_Host_ViewTextT:
      self = .text(value.text)
    case let value as Tsugumi_Host_ViewGamepadT:
      self = .gamepad(pad: value.pad, kind: value.kind, buttons: value.buttons,
                      lx: value.lx, ly: value.ly, rx: value.rx, ry: value.ry, lt: value.lt, rt: value.rt)
    default: return nil
    }
  }

  var wire: Tsugumi_Host_ViewActionUnion {
    switch self {
    case let .pointer(kind, x, y, buttons, wheelX, wheelY, modifiers):
      let value = Tsugumi_Host_ViewPointerT()
      value.kind = kind; value.x = x; value.y = y; value.buttons = buttons
      value.wheelX = wheelX; value.wheelY = wheelY; value.modifiers = modifiers
      return .init(value, type: .viewpointer)
    case let .key(kind, code, modifiers):
      let value = Tsugumi_Host_ViewKeyT()
      value.kind = kind; value.code = code; value.modifiers = modifiers
      return .init(value, type: .viewkey)
    case let .text(text):
      let value = Tsugumi_Host_ViewTextT()
      value.text = text
      return .init(value, type: .viewtext)
    case let .gamepad(pad, kind, buttons, lx, ly, rx, ry, lt, rt):
      let value = Tsugumi_Host_ViewGamepadT()
      value.pad = pad; value.kind = kind; value.buttons = buttons
      value.lx = lx; value.ly = ly; value.rx = rx; value.ry = ry; value.lt = lt; value.rt = rt
      return .init(value, type: .viewgamepad)
    }
  }
}

public struct HostViewCommand: Sendable {
  public enum Action: Sendable { case input(HostViewInput), snapshot }

  public let project: String
  public let session: UUID
  public let action: Action
  let request: String

  init?(_ value: Tsugumi_Host_ViewActionDispatchT) {
    guard let session = UUID(uuidString: value.session) else { return nil }
    project = value.projectId
    self.session = session
    request = value.request
    if value.action?.type == .viewsnapshot { action = .snapshot }
    else if let input = HostViewInput(value.action) { action = .input(input) }
    else { return nil }
  }
}

public enum HostViewEvent: Sendable {
  case connected
  case command(HostViewCommand)
}

public extension CoreHostClient {
  func listViews(project: String) async throws -> [Tsugumi_Host_ViewWindowT] {
    let request = Tsugumi_Host_ViewWindowsRequestT()
    request.projectId = project
    let result: Tsugumi_Host_ViewWindowsT = try await hostCall(.viewwindowsrequest, request)
    return result.windows.compactMap { $0 }
  }

  func sendViewInput(project: String, session: UUID, input: HostViewInput) async throws {
    _ = try await viewAction(project: project, session: session, action: input.wire)
  }

  func snapshotView(project: String, session: UUID) async throws -> Data {
    let result = try await viewAction(project: project, session: session,
                                      action: .init(Tsugumi_Host_ViewSnapshotT(), type: .viewsnapshot))
    return Data(result.png)
  }

  func viewEvents() -> AsyncStream<HostViewEvent> {
    let (events, continuation) = AsyncStream<HostViewEvent>.makeStream()
    let observer = observeHost { event in
      if let command = event["viewCommand"] as? HostViewCommand { continuation.yield(.command(command)) }
      else if event["type"] as? String == "Connected" { continuation.yield(.connected) }
    }
    continuation.onTermination = { [weak self] _ in
      Task { await self?.removeHostObserver(observer) }
    }
    return events
  }

  func reportView(project: String, window: Tsugumi_Host_ViewWindowT, closed: Bool) async throws {
    let request = Tsugumi_Host_ViewPresentationRequestT()
    request.projectId = project; request.window = window; request.closed = closed
    let _: Tsugumi_Service_AcknowledgementT =
      try await hostCall(.viewpresentationrequest, request, existingOnly: closed)
  }

  func completeViewCommand(_ command: HostViewCommand, png: Data? = nil, error: String? = nil) async throws {
    let request = Tsugumi_Host_ViewActionCompleteRequestT()
    request.request = command.request; request.session = command.session.uuidString.lowercased()
    request.png = png.map(Array.init) ?? []; request.error = error
    let _: Tsugumi_Service_AcknowledgementT =
      try await hostCall(.viewactioncompleterequest, request, existingOnly: true)
  }

  private func viewAction(project: String, session: UUID, action: Tsugumi_Host_ViewActionUnion) async throws
    -> Tsugumi_Host_ViewActionResultT {
    let request = Tsugumi_Host_ViewActionRequestT()
    request.projectId = project; request.session = session.uuidString.lowercased(); request.action = action
    return try await hostCall(.viewactionrequest, request)
  }
}
