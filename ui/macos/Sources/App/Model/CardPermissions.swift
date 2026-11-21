import Foundation
import Observation
import TsugumiAgentClient
import TsugumiRPC

/// Presentation catalog supplied by the host, read when the host connects.
/// Missing entries use their id.
@MainActor @Observable final class PermissionCatalog {
  static let shared = PermissionCatalog()
  private(set) var entries: [Tsugumi_Service_PermissionKindT] = []
  private init() {
    Task {
      for await session in await CoreHostClient.shared.hostConnectionChanges() where session != nil && entries.isEmpty {
        entries = (try? await CoreHostClient.shared.permissionKinds(language: AppPreferences.Language.running.rawValue)) ?? []
      }
    }
  }
}

struct CardPermission: RawRepresentable, Hashable {
  let rawValue: String
  init?(rawValue: String) {
    guard !rawValue.isEmpty else { return nil }
    self.rawValue = rawValue
  }
  @MainActor var known: Bool { PermissionCatalog.shared.entries.contains { $0.id == rawValue } }
  @MainActor var label: String { PermissionCatalog.shared.entries.first { $0.id == rawValue }?.label ?? rawValue }
  @MainActor var symbol: String { PermissionCatalog.shared.entries.first { $0.id == rawValue }?.icon ?? "hand.raised" }

  // These are only the properties shown as badges on a Card, not a catalog.
  static let network = CardPermission(rawValue: "network")!
  static let sandboxOff = CardPermission(rawValue: "sandbox_off")!
  static let hostFolderRead = CardPermission(rawValue: "host_folder_read")!
  static let hostFolderWrite = CardPermission(rawValue: "host_folder_write")!

  struct Request: Identifiable, Hashable {
    let raw: String
    let kind: CardPermission
    let card: String
    let folder: String?
    var id: String { raw }

    init?(_ raw: String) {
      guard !raw.hasPrefix("project_access "), !raw.contains("operation_approval\n") else { return nil }
      let parts = raw.split(separator: " ", maxSplits: 2).map(String.init)
      guard let first = parts.first, let kind = CardPermission(rawValue: first) else { return nil }
      self.raw = raw
      self.kind = kind
      card = parts.count > 1 ? parts[1] : ""
      folder = parts.count > 2 ? parts[2] : nil
    }
  }

  /// What a Card's settings give it, for its badges.
  static func of(_ card: JSONValue?) -> [CardPermission] {
    guard let card else { return [] }
    var out: [CardPermission] = []
    let grants = card["grants"]
    let sandbox = card["sandbox"]
    if grants?["network"]?.bool == true { out.append(.network) }
    if grants?["gui"]?.bool == true { out.append(CardPermission(rawValue: "gui")!) }
    if ["pid", "ipc", "identity"].contains(where: { sandbox?[$0]?.bool == false }) {
      out.append(CardPermission(rawValue: "sandbox_options")!)
    }
    if sandbox?["filesystem"]?.bool == false { out.append(.sandboxOff) }
    let folders = grants?["hostPaths"]?.array ?? []
    if folders.contains(where: { $0["write"]?.bool == true }) { out.append(.hostFolderWrite) }
    else if !folders.isEmpty { out.append(.hostFolderRead) }
    return out
  }
}
