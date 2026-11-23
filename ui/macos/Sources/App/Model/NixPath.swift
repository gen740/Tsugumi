import Foundation
import TsugumiRPC

enum NixPathLockStatus: Equatable {
  case unlocked
  case urlChanged
  case locked(revision: String, modified: Date?)

  static func read(_ document: JSONValue?, name: String, url: String) -> Self {
    guard let entry = document?["nixPath"]?[name]?.string,
          let pin = URLComponents(string: entry) else {
      return .unlocked
    }
    guard let intent = URLComponents(string: url),
          pin.scheme == intent.scheme,
          pin.path.split(separator: "/").prefix(2).elementsEqual(intent.path.split(separator: "/").prefix(2)) else {
      return .urlChanged
    }
    guard let rev = pin.path.split(separator: "/").dropFirst(2).first else { return .unlocked }
    let date = pin.queryItems?.first { $0.name == "lastModified" }?.value.flatMap(Double.init)
      .flatMap { $0.isFinite ? Date(timeIntervalSince1970: $0) : nil }
    return .locked(revision: String(rev.prefix(7)), modified: date)
  }
}
