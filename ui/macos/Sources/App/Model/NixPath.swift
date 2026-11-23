import Foundation
import TsugumiRPC

enum NixPathLockStatus: Equatable {
  case unlocked
  case urlChanged
  case locked(revision: String, modified: Date?)

  static func read(_ document: JSONValue?, name: String, url: String) -> Self {
    guard let entry = document?["nixPath"]?.array?.first(where: { $0["name"]?.string == name }) else {
      return .unlocked
    }
    guard entry["url"]?.string == url else { return .urlChanged }
    guard let rev = entry["locked"]?["rev"]?.string else { return .unlocked }
    let date = entry["locked"]?["lastModified"]?.double
      .flatMap { $0.isFinite ? Date(timeIntervalSince1970: $0) : nil }
    return .locked(revision: String(rev.prefix(7)), modified: date)
  }
}
