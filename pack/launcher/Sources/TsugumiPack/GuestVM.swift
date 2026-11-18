import Foundation
import Virtualization

/// One boot of the pack guest: it does its one job (export or run) and powers off.
struct GuestBoot {
  enum Mode: String { case export, run }

  var mode: Mode
  var kernel: URL
  var rootfs: URL
  /// /dev/vdb: the exporter's store disk (writable) or the app's payload (read only).
  var disk: URL
  var diskReadOnly: Bool
  /// /dev/vdc, writable: the run's scratch disk.
  var scratch: URL? = nil
  /// virtiofs "pack": what the guest reads and where it writes its results.
  var share: URL
  var network: Bool
  var cpus: Int
  var memoryGiB: Int
  /// The whole console, kept for a failure report.
  var consoleLog: URL

  static func defaultCPUs() -> Int {
    max(1, min(ProcessInfo.processInfo.activeProcessorCount, VZVirtualMachineConfiguration.maximumAllowedCPUCount))
  }

  static func defaultMemoryGiB(_ wanted: Int) -> Int {
    let physical = Int(ProcessInfo.processInfo.physicalMemory >> 30)
    return max(2, min(wanted, physical / 2))
  }

  func configuration() throws -> VZVirtualMachineConfiguration {
    let c = VZVirtualMachineConfiguration()
    let boot = VZLinuxBootLoader(kernelURL: kernel)
    boot.commandLine = "console=hvc0 root=/dev/vda rootfstype=erofs ro init=/init loglevel=4 panic=-1 tsugumi.pack=\(mode.rawValue)"
    c.bootLoader = boot
    c.platform = VZGenericPlatformConfiguration()
    c.cpuCount = cpus
    c.memorySize = UInt64(memoryGiB) << 30
    let root = try VZDiskImageStorageDeviceAttachment(url: rootfs, readOnly: true)
    let second = try VZDiskImageStorageDeviceAttachment(url: disk, readOnly: diskReadOnly,
                                                        cachingMode: .automatic, synchronizationMode: .full)
    var disks = [root, second]
    if let scratch {
      disks.append(try VZDiskImageStorageDeviceAttachment(url: scratch, readOnly: false,
                                                         cachingMode: .automatic, synchronizationMode: .none))
    }
    c.storageDevices = disks.map { VZVirtioBlockDeviceConfiguration(attachment: $0) }
    if network {
      let net = VZVirtioNetworkDeviceConfiguration()
      net.attachment = VZNATNetworkDeviceAttachment()
      c.networkDevices = [net]
    }
    let fs = VZVirtioFileSystemDeviceConfiguration(tag: "pack")
    fs.share = VZSingleDirectoryShare(directory: VZSharedDirectory(url: share, readOnly: false))
    c.directorySharingDevices = [fs]
    c.entropyDevices = [VZVirtioEntropyDeviceConfiguration()]
    let serial = VZVirtioConsoleDeviceSerialPortConfiguration()
    serial.attachment = try VZFileSerialPortAttachment(url: consoleLog, append: false)
    c.serialPorts = [serial]
    try c.validate()
    return c
  }

  /// Boots, hands each console line to `line` and returns when the guest stopped.
  func run(line: @escaping (String) -> Void) throws {
    FileManager.default.createFile(atPath: consoleLog.path, contents: nil)
    let config = try configuration()
    let vm = VZVirtualMachine(configuration: config)
    let watcher = StopWatcher()
    vm.delegate = watcher
    let reader = ConsoleReader(url: consoleLog, line: line)
    var startError: Error?
    vm.start { result in
      if case .failure(let e) = result { startError = e; watcher.done = true }
    }
    // Ctrl-C or a terminate: stop the guest, then return so the callers'
    // cleanup (temporary folders, scratch disk) runs.
    var interrupted = false
    let signals = [SIGINT, SIGTERM].map { number -> DispatchSourceSignal in
      signal(number, SIG_IGN)
      let source = DispatchSource.makeSignalSource(signal: number, queue: .main)
      source.setEventHandler { interrupted = true }
      source.resume()
      return source
    }
    defer {
      for source in signals { source.cancel() }
      signal(SIGINT, SIG_DFL)
      signal(SIGTERM, SIG_DFL)
    }
    var stopping = false
    while !watcher.done {
      _ = RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.1))
      reader.poll()
      if interrupted, !stopping, vm.canStop {
        stopping = true
        vm.stop { _ in watcher.done = true }
      }
    }
    reader.poll()
    reader.finish()
    if interrupted { throw PackError("interrupted; the guest was stopped") }
    if let startError { throw PackError("the VM did not start: \(startError.localizedDescription)") }
    if let e = watcher.error { throw PackError("the VM stopped with an error: \(e.localizedDescription)") }
  }
}

private final class StopWatcher: NSObject, VZVirtualMachineDelegate {
  var done = false
  var error: Error?
  func guestDidStop(_ virtualMachine: VZVirtualMachine) { done = true }
  func virtualMachine(_ virtualMachine: VZVirtualMachine, didStopWithError error: Error) {
    self.error = error
    done = true
  }
}

/// Follows the console file the VM writes and splits it into lines.
private final class ConsoleReader {
  let handle: FileHandle?
  let line: (String) -> Void
  var pending = Data()

  init(url: URL, line: @escaping (String) -> Void) {
    handle = try? FileHandle(forReadingFrom: url)
    self.line = line
  }

  func poll() {
    guard let data = try? handle?.readToEnd(), !data.isEmpty else { return }
    pending.append(data)
    while let newline = pending.firstIndex(of: 0x0a) {
      let bytes = pending[pending.startIndex..<newline]
      pending.removeSubrange(pending.startIndex...newline)
      line(String(decoding: bytes, as: UTF8.self).trimmingCharacters(in: CharacterSet(charactersIn: "\r")))
    }
  }

  func finish() {
    if !pending.isEmpty { line(String(decoding: pending, as: UTF8.self)) }
    try? handle?.close()
  }
}

/// A sparse disk image of `gib` GiB; takes no space until written.
func createSparseDisk(_ url: URL, gib: Int) throws {
  FileManager.default.createFile(atPath: url.path, contents: nil)
  let h = try FileHandle(forWritingTo: url)
  try h.truncate(atOffset: UInt64(gib) << 30)
  try h.close()
}

struct PackError: Error, CustomStringConvertible {
  let description: String
  init(_ description: String) { self.description = description }
}
