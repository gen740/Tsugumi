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
}

/// Messages waiting for a view that is still busy with an earlier one.
struct CardViewOutbox {
  private var deliveries: [(CardViewMessage, () -> Void)] = []
  var pending: [CardViewMessage] { deliveries.map { $0.0 } }

  mutating func put(_ message: CardViewMessage, complete: @escaping () -> Void = {}) {
    deliveries.append((message, complete))
  }

  mutating func take() -> (CardViewMessage, () -> Void)? {
    deliveries.isEmpty ? nil : deliveries.removeFirst()
  }

  mutating func discard() {
    let pending = deliveries
    deliveries.removeAll()
    for (_, complete) in pending { complete() }
  }

}

/// Live view traffic for the Cards whose view windows are open. It lives in
/// memory only while a window shows the Card. Nothing of it is recorded.
@MainActor @Observable final class CardViewRelay {
  private struct Host {
    let card: CardKey
    let view: String
    let deliver: (CardViewMessage, @escaping () -> Void) -> Void
  }

  /// The declarations of each Card, absent until the core has sent them.
  private(set) var declarations: [CardKey: [CardViewDeclaration]] = [:]
  /// Why evaluating a Card's declarations failed. Absent when they were evaluated, even to an empty list.
  private(set) var failures: [CardKey: String] = [:]
  /// The view names data arrived for, per Card.
  private(set) var received: [CardKey: Set<String>] = [:]
  @ObservationIgnored private var hosts: [UUID: Host] = [:]

  /// Names the Card sent data to without declaring them. Empty until the declarations are known.
  func undeclared(_ card: CardKey) -> [String] {
    guard failures[card] == nil, let declared = declarations[card] else { return [] }
    return received[card, default: []].subtracting(declared.map(\.name)).sorted()
  }

  /// Names more than one declaration has. Data sent to such a name reaches no view.
  func ambiguous(_ card: CardKey) -> [String] {
    let names = declarations[card, default: []].map(\.name)
    return Set(names.filter { name in names.filter { $0 == name }.count > 1 }).sorted()
  }

  /// The full list for a Card; an empty list clears what it declared before.
  /// A failure says the declarations could not be evaluated.
  func declare(_ card: CardKey, _ views: [CardViewDeclaration], failure: String? = nil) {
    if declarations[card] != views { declarations[card] = views }
    if failures[card] != failure { failures[card] = failure }
  }

  func receive(_ card: CardKey, view: String, bytes: Data, complete: @escaping () -> Void = {}) {
    if received[card]?.contains(view) != true { received[card, default: []].insert(view) }
    guard declarations[card]?.filter({ $0.name == view }).count == 1 else { complete(); return }
    let listeners = hosts.values.filter { $0.card == card && $0.view == view }
    var remaining = listeners.count
    if remaining == 0 { complete() }
    for host in listeners {
      host.deliver(.data(bytes)) {
        remaining -= 1
        if remaining == 0 { complete() }
      }
    }
  }

  func attach(_ card: CardKey, view: String,
              deliver: @escaping (CardViewMessage, @escaping () -> Void) -> Void) -> UUID {
    let id = UUID()
    hosts[id] = Host(card: card, view: view, deliver: deliver)
    return id
  }

  func detach(_ id: UUID) { hosts[id] = nil }

  /// The number of attached hosts, for tests of teardown.
  var attachedCount: Int { hosts.count }

  /// Drops everything about a Card whose windows all closed or that left the document.
  func forget(_ card: CardKey) {
    declarations[card] = nil
    failures[card] = nil
    received[card] = nil
  }

  var cards: Set<CardKey> {
    Set(declarations.keys).union(received.keys)
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

  /// `CardViewReadRequest{card, index, name, offset, limit_bytes}` -> `FileContent`.
  /// `index` is the declaration's place in the views list; `name` guards against a changed list.
  let page: @MainActor (_ card: [String], _ index: Int, _ name: String, _ offset: UInt64) async throws -> CardViewFilePage

  @MainActor func read(card: [String], index: Int, name: String) async throws -> Data {
    var data = Data(), offset: UInt64 = 0
    while true {
      let page = try await page(card, index, name, offset)
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
  static let unavailable = CardViewFiles { _, _, _, _ in throw CardViewFileError.unavailable }

  /// Pages served by the connected project core.
  static func rpc(_ client: @escaping @MainActor () -> RPCClient?) -> CardViewFiles {
    CardViewFiles { card, index, name, offset in
      guard let c = client() else { throw RPCError.disconnected }
      let page = try await c.readCardView(card: card, index: index, name: name, offset: offset)
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
