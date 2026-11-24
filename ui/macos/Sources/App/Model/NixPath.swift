import TsugumiRPC

enum NixPathLockStatus: Equatable {
  case unlocked
  case urlChanged
  case locked(pin: String)

  static func read(_ document: JSONValue?, name: String, url: String) -> Self {
    guard let entry = document?["entries"]?.array?.first(where: { $0["name"]?.string == name }),
          let hash = entry["narHash"]?.string else {
      return .unlocked
    }
    guard entry["url"]?.string == url else { return .urlChanged }
    let pin = entry["rev"]?.string.map { String($0.prefix(7)) } ?? String(hash.prefix(14))
    return .locked(pin: pin)
  }
}
