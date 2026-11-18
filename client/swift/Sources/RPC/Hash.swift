import CryptoKit
import Foundation

/// Lowercase SHA-256 hex of raw bytes (source and artifact verification).
public func sha256Hex(_ data: Data) -> String {
  SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}
