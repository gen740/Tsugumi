import Foundation
import TsugumiViewPresentation

/// Maps the Host ViewWindows and ViewAction RPCs to the packed app's presentation.
@MainActor struct DebugViews {
  let presentation: CardViewPresentation

  func handle(_ method: String, _ body: [String: Any]) async throws -> [String: Any]? {
    switch method {
    case "ViewWindowsRequest":
      let openings = presentation.openings.sorted { $0.id.uuidString < $1.id.uuidString }
      let windows: [[String: Any]] = openings.map { view in
        ["session": view.id.uuidString.lowercased(), "card": ["segments": view.card],
         "name": view.name, "type": view.type, "mode": view.mode.rawValue,
         "width": view.width, "height": view.height, "flags": view.flags]
      }
      return ["windows": windows]
    case "ViewActionRequest":
      let decoder = JSONDecoder()
      decoder.keyDecodingStrategy = .convertFromSnakeCase
      let action = try decoder.decode(Action.self, from: JSONSerialization.data(withJSONObject: body))
      if action.type == .snapshot {
        let image = try await presentation.snapshot(session: action.session)
        return ["png": Array(image)]
      }
      try await presentation.sendInput(try action.fields.input(for: action.type), session: action.session)
      return ["png": [UInt8]()]
    default:
      return nil
    }
  }

  private struct Action: Decodable {
    enum Kind: String, Decodable {
      case pointer = "ViewPointer"
      case key = "ViewKey"
      case text = "ViewText"
      case gamepad = "ViewGamepad"
      case snapshot = "ViewSnapshot"
    }

    struct Fields: Decodable {
      var kind: UInt32?
      var modifiers: UInt32?
      var buttons: UInt32?
      var x: Float?
      var y: Float?
      var wheelX: Float?
      var wheelY: Float?
      var code: UInt32?
      var text: String?
      var pad: UInt32?
      var lx: Float?
      var ly: Float?
      var rx: Float?
      var ry: Float?
      var lt: Float?
      var rt: Float?

      func input(for action: Kind) throws -> CardViewInput {
        switch action {
        case .pointer:
          return .pointer(kind: kind ?? 0, x: x ?? 0, y: y ?? 0, buttons: buttons ?? 0,
                          wheelX: wheelX ?? 0, wheelY: wheelY ?? 0, modifiers: modifiers ?? 0)
        case .key:
          return .key(kind: kind ?? 0, code: code ?? 0, modifiers: modifiers ?? 0)
        case .text:
          guard let text else { throw DebugSocket.SocketError("ViewText requires text") }
          return .text(text)
        case .gamepad:
          return .gamepad(pad: pad ?? 0, kind: kind ?? 0, buttons: buttons ?? 0,
                          lx: lx ?? 0, ly: ly ?? 0, rx: rx ?? 0, ry: ry ?? 0, lt: lt ?? 0, rt: rt ?? 0)
        case .snapshot:
          throw DebugSocket.SocketError("ViewSnapshot is a capture request")
        }
      }
    }

    let session: UUID
    let type: Kind
    let fields: Fields

    private enum CodingKeys: String, CodingKey {
      case session
      case type = "actionType"
      case fields = "action"
    }
  }
}
