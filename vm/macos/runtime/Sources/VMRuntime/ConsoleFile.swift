import Foundation

/// Follow writes to the current boot's serial file. The attachment opens it
/// in append mode and the next boot rotates it after the reader has stopped.
final class ConsoleFile: @unchecked Sendable {
  private let source: DispatchSourceFileSystemObject
  private let file: FileHandle
  private let receive: @Sendable (Result<Data, Error>) -> Void

  init(_ url: URL, queue: DispatchQueue, receive: @escaping @Sendable (Result<Data, Error>) -> Void) throws {
    let file = try FileHandle(forReadingFrom: url)
    self.file = file
    self.receive = receive
    source = DispatchSource.makeFileSystemObjectSource(
      fileDescriptor: file.fileDescriptor, eventMask: [.write, .extend, .rename, .delete], queue: queue)
    source.setRegistrationHandler { [weak self] in self?.drain() }
    source.setEventHandler { [weak self] in
      guard let self else { return }
      if !source.data.intersection([.rename, .delete]).isEmpty {
        receive(.failure(CocoaError(.fileReadNoSuchFile)))
        cancel()
      } else {
        drain()
      }
    }
    source.setCancelHandler { _ = try? file.close() }
    source.activate()
  }

  private func drain() {
    do {
      if let data = try file.readToEnd(), !data.isEmpty { receive(.success(data)) }
    } catch {
      receive(.failure(error))
      cancel()
    }
  }

  func cancel() { source.cancel() }

  deinit { cancel() }
}
