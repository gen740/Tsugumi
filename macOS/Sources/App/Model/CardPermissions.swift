import Foundation
import TsugumiRPC

/// What a Card may do beyond computing (core/src/permissions.hh): reach the
/// network, run without the sandbox, see folders of the Mac. The Agent's
/// saves that widen these wait for the user (ChatController.approve), except
/// for the kinds the user allows without asking (AppPreferences).
enum CardPermission: String, CaseIterable, Identifiable {
  case network
  case sandboxOff = "sandbox_off"
  case hostFolderRead = "host_folder_read"
  case hostFolderWrite = "host_folder_write"

  var id: String { rawValue }

  var label: String {
    switch self {
    case .network: return String(localized: "ネットワークへのアクセス")
    case .sandboxOff: return String(localized: "隔離（sandbox）なしで実行")
    case .hostFolderRead: return String(localized: "Mac のフォルダを読む")
    case .hostFolderWrite: return String(localized: "Mac のフォルダに書く")
    }
  }

  var symbol: String {
    switch self {
    case .network: return "network"
    case .sandboxOff: return "shield.slash"
    case .hostFolderRead: return "folder"
    case .hostFolderWrite: return "folder.badge.plus"
    }
  }

  /// One widening a save asks for: "<kind> <card>[ <folder>]".
  struct Request: Identifiable, Hashable {
    let raw: String
    let kind: CardPermission
    let card: String
    let folder: String?
    var id: String { raw }

    init?(_ raw: String) {
      let parts = raw.split(separator: " ", maxSplits: 2).map(String.init)
      guard parts.count >= 2, let kind = CardPermission(rawValue: parts[0]) else { return nil }
      self.raw = raw
      self.kind = kind
      card = parts[1]
      folder = parts.count > 2 ? parts[2] : nil
    }
  }

  /// What a Card's settings (_card.json) give it, for its icons.
  static func of(_ card: JSONValue?) -> [CardPermission] {
    guard let card else { return [] }
    var out: [CardPermission] = []
    if card["network"]?.bool == true { out.append(.network) }
    if card["sandbox"]?.bool == false { out.append(.sandboxOff) }
    let folders = card["hostFolders"]?.array ?? []
    if folders.contains(where: { $0["write"]?.bool == true }) { out.append(.hostFolderWrite) }
    else if !folders.isEmpty { out.append(.hostFolderRead) }
    return out
  }
}
