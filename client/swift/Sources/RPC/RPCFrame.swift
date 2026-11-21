import Foundation
import TsugumiClientC

/// One length-prefixed frame (a 4-byte little-endian length, then the bytes)
/// on a blocking descriptor, for peers that are not `Connection` clients
/// (rpc/include/tsugumi/rpc/frame.h). After a thrown error the caller closes
/// the connection: a length over `maxBytes` is rejected before anything is
/// allocated for it, and a stream that ends inside a frame is an error.
public enum RPCFrame {
  public struct Failure: Error, CustomStringConvertible, Sendable {
    public let description: String
  }

  /// The next frame, or nil when the stream ends before a frame begins.
  public static func read(_ fd: Int32, maxBytes: UInt32) throws -> Data? {
    var bytes: UnsafeMutablePointer<UInt8>?
    var size: UInt32 = 0
    let status = tsugumi_frame_read(fd, maxBytes, &bytes, &size)
    if status == TSUGUMI_FRAME_END { return nil }
    guard status == TSUGUMI_FRAME_OK, let bytes else { throw failure(status, maxBytes) }
    return Data(bytesNoCopy: bytes, count: Int(size), deallocator: .custom { pointer, _ in
      tsugumi_frame_free(pointer.assumingMemoryBound(to: UInt8.self))
    })
  }

  public static func write(_ data: Data, to fd: Int32, maxBytes: UInt32) throws {
    guard data.count <= Int(maxBytes) else { throw failure(TSUGUMI_FRAME_SIZE, maxBytes) }
    let status = data.withUnsafeBytes { buffer in
      tsugumi_frame_write(fd, buffer.bindMemory(to: UInt8.self).baseAddress, UInt32(buffer.count), maxBytes)
    }
    guard status == TSUGUMI_FRAME_OK else { throw failure(status, maxBytes) }
  }

  private static func failure(_ status: tsugumi_frame_status, _ maxBytes: UInt32) -> Failure {
    let error = errno
    switch status {
    case TSUGUMI_FRAME_SIZE: return Failure(description: "frame is empty or exceeds the limit of \(maxBytes) bytes")
    case TSUGUMI_FRAME_TRUNCATED: return Failure(description: "stream ended inside a frame")
    default: return Failure(description: "frame transfer failed: \(String(cString: strerror(error)))")
    }
  }
}
