import Foundation
import Observation
import TsugumiRPC

/// One view a Card's definition declares (`views = [ { name; type; blob; } ]`).
/// The core relays the declaration unchanged; the UI decides how to host it.
struct CardViewDeclaration: Equatable, Hashable {
  let name: String
  let type: String
  /// The immutable store path of the view's single file.
  let blob: String
}

extension CardViewDeclaration {
  init(_ wire: Tsugumi_Data_CardViewT) {
    self.init(name: wire.name, type: wire.type, blob: wire.blob)
  }
}

/// What a view host passes on to its view, in the order the core sent it.
enum CardViewMessage: Equatable {
  /// Opaque bytes the Card sent to this view.
  case data(Data)
  /// A run of the Card started (true) or ended (false).
  case run(started: Bool)
}

/// Messages waiting for a view that is still busy with an earlier one. Only
/// pending data is replaced by newer data; run state changes keep their order.
struct CardViewOutbox {
  private(set) var pending: [CardViewMessage] = []

  mutating func put(_ message: CardViewMessage) {
    if case .data = message, case .data? = pending.last {
      pending[pending.count - 1] = message
    } else {
      pending.append(message)
    }
  }

  mutating func take() -> CardViewMessage? {
    pending.isEmpty ? nil : pending.removeFirst()
  }
}

/// Live view traffic for the Cards whose view windows are open. It lives in
/// memory only while a window shows the Card. Nothing of it is recorded.
@MainActor @Observable final class CardViewRelay {
  struct RunMark: Equatable {
    let runId: String
    let started: Bool
  }

  private struct Host {
    let card: CardKey
    let view: String
    let deliver: (CardViewMessage) -> Void
  }

  /// The declarations of each Card, absent until the core has sent them.
  private(set) var declarations: [CardKey: [CardViewDeclaration]] = [:]
  /// The view names data arrived for, per Card.
  private(set) var received: [CardKey: Set<String>] = [:]
  private(set) var runs: [CardKey: RunMark] = [:]
  @ObservationIgnored private var latest: [CardKey: [String: Data]] = [:]
  @ObservationIgnored private var hosts: [UUID: Host] = [:]

  /// Names the Card sent data to without declaring them. Empty until the declarations are known.
  func undeclared(_ card: CardKey) -> [String] {
    guard let declared = declarations[card] else { return [] }
    return received[card, default: []].subtracting(declared.map(\.name)).sorted()
  }

  /// The full list for a Card; an empty list clears what it declared before.
  func declare(_ card: CardKey, _ views: [CardViewDeclaration]) {
    if declarations[card] != views { declarations[card] = views }
  }

  func receive(_ card: CardKey, view: String, bytes: Data) {
    latest[card, default: [:]][view] = bytes
    if received[card]?.contains(view) != true { received[card, default: []].insert(view) }
    for host in hosts.values where host.card == card && host.view == view { host.deliver(.data(bytes)) }
  }

  func runState(_ card: CardKey, runId: String, started: Bool) {
    runs[card] = RunMark(runId: runId, started: started)
    for host in hosts.values where host.card == card { host.deliver(.run(started: started)) }
  }

  /// A new host starts from the state so far: the run that started, its latest data, and whether the run ended.
  func attach(_ card: CardKey, view: String, deliver: @escaping (CardViewMessage) -> Void) -> UUID {
    let id = UUID()
    hosts[id] = Host(card: card, view: view, deliver: deliver)
    let run = runs[card]
    if run != nil { deliver(.run(started: true)) }
    if let bytes = latest[card]?[view] { deliver(.data(bytes)) }
    if run?.started == false { deliver(.run(started: false)) }
    return id
  }

  func detach(_ id: UUID) { hosts[id] = nil }

  /// The number of attached hosts, for tests of teardown.
  var attachedCount: Int { hosts.count }

  /// Drops everything about a Card whose windows all closed or that left the document.
  func forget(_ card: CardKey) {
    declarations[card] = nil
    received[card] = nil
    runs[card] = nil
    latest[card] = nil
  }

  var cards: Set<CardKey> {
    Set(declarations.keys).union(received.keys).union(runs.keys).union(latest.keys)
  }
}

/// One page of a declared view file, as the project core serves it.
struct CardViewFilePage {
  let bytes: Data
  let eof: Bool
  let nextOffset: UInt64?
}

/// Reads the file a Card declared for a view. The project core serves the
/// bytes of the declared store path opaquely, page by page; the UI never
/// reads store paths itself, so this works the same with the shared VM.
struct CardViewFiles {
  static let limit = 32 * 1024 * 1024

  /// `CardViewReadRequest{card, name, offset, limit_bytes}` -> `FileContent`.
  let page: @MainActor (_ card: [String], _ name: String, _ offset: UInt64) async throws -> CardViewFilePage

  @MainActor func read(card: [String], name: String) async throws -> Data {
    var data = Data(), offset: UInt64 = 0
    while true {
      let page = try await page(card, name, offset)
      guard data.count + page.bytes.count <= Self.limit else {
        throw CardViewFileError.tooLarge
      }
      data.append(page.bytes)
      if page.eof { return data }
      guard let next = page.nextOffset, next > offset else { throw CardViewFileError.malformed }
      offset = next
    }
  }

  /// Before the session is connected.
  static let unavailable = CardViewFiles { _, _, _ in throw CardViewFileError.unavailable }

  /// Pages served by the connected project core.
  static func rpc(_ client: @escaping @MainActor () -> RPCClient?) -> CardViewFiles {
    CardViewFiles { card, name, offset in
      guard let c = client() else { throw RPCError.disconnected }
      let page = try await c.readCardView(card: card, name: name, offset: offset)
      return CardViewFilePage(bytes: Data(page.bytes), eof: page.eof == true, nextOffset: page.nextOffset)
    }
  }
}

enum CardViewFileError: LocalizedError {
  case unavailable, tooLarge, malformed, notText

  var errorDescription: String? {
    switch self {
    case .unavailable: String(localized: "この接続では表示のファイルを読めません")
    case .tooLarge: String(localized: "表示のファイルが 32 MiB を超えています")
    case .malformed: String(localized: "表示のファイルを読めません")
    case .notText: String(localized: "HTML 表示のファイルが UTF-8 ではありません")
    }
  }
}
