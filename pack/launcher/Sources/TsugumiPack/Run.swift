import Foundation

/// The exported app: runs the Workflow once with the values fixed at export
/// time and writes its results to a folder. No network device is attached.
struct AppRun {
  var resources: URL
  var manifest: [String: Any]
  var output: URL
  var start: String?
  var cpus: Int
  var memoryGiB: Int
  var verbose: Bool
  /// Started from the Finder rather than a shell: the results folder is shown at the end.
  var opened = false

  var name: String { manifest["name"] as? String ?? "App" }

  func usage() -> String {
    """
      usage: \(name) [--start TRIGGER] [--output DIR] [--cpus N] [--memory GiB] [--verbose]
             \(name) --describe

      Runs the Workflow "\(manifest["project"] as? String ?? name)" offline with the values fixed
      at export time and writes each Card's outputs and logs under DIR
      (default ./\(name) results <time>).
      """
  }

  static func parse(_ arguments: [String], resources: URL) throws -> AppRun? {
    let manifest = try JSONSerialization.jsonObject(with: Data(contentsOf: resources.appendingPathComponent("pack.json"))) as? [String: Any] ?? [:]
    var run = AppRun(resources: resources, manifest: manifest, output: URL(fileURLWithPath: ""), start: nil,
                     cpus: GuestBoot.defaultCPUs(), memoryGiB: GuestBoot.defaultMemoryGiB(8), verbose: false)
    var output: String?
    run.opened = arguments.isEmpty && isatty(STDOUT_FILENO) == 0
    var i = 0
    func value() throws -> String {
      i += 1
      guard i < arguments.count else { throw PackError("\(arguments[i - 1]) needs a value") }
      return arguments[i]
    }
    while i < arguments.count {
      switch arguments[i] {
      case "--start": run.start = try value()
      case "--output", "-o": output = try value()
      case "--cpus": run.cpus = Int(try value()) ?? run.cpus
      case "--memory": run.memoryGiB = GuestBoot.defaultMemoryGiB(Int(try value()) ?? run.memoryGiB)
      case "--verbose", "-v": run.verbose = true
      case "--describe":
        print(String(decoding: try JSONSerialization.data(withJSONObject: manifest, options: [.prettyPrinted, .sortedKeys]), as: UTF8.self))
        return nil
      case "--help", "-h":
        print(run.usage())
        return nil
      default: throw PackError("unknown argument \(arguments[i])\n\n\(run.usage())")
      }
      i += 1
    }
    let stamp = DateFormatter()
    stamp.dateFormat = "yyyy-MM-dd HH.mm.ss"
    let folder = "\(run.name) results \(stamp.string(from: Date()))"
    if let output {
      run.output = URL(fileURLWithPath: output).standardized
    } else if run.opened {
      // Opened from the Finder: no working directory of the recipient's.
      run.output = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent(folder)
    } else {
      run.output = URL(fileURLWithPath: folder).standardized
    }
    return run
  }

  /// Runs once; returns the process exit status.
  func run() throws -> Int32 {
    let fm = FileManager.default
    let began = Date()
    guard !fm.fileExists(atPath: output.path) else { throw PackError("\(output.path) already exists") }
    let share = fm.temporaryDirectory.appendingPathComponent("tsugumi-pack-run-\(UUID().uuidString)")
    try fm.createDirectory(at: share, withIntermediateDirectories: true)
    defer { try? fm.removeItem(at: share) }
    var request: [String: Any] = [:]
    if let start { request["start"] = start }
    try JSONSerialization.data(withJSONObject: request, options: [.prettyPrinted]).write(to: share.appendingPathComponent("request.json"))
    let console = share.appendingPathComponent("console.log")
    // Outside the share: the guest formats it as its scratch disk.
    let scratch = fm.temporaryDirectory.appendingPathComponent("tsugumi-pack-scratch-\(UUID().uuidString).img")
    try createSparseDisk(scratch, gib: 64)
    defer { try? fm.removeItem(at: scratch) }
    var boot = GuestBoot(mode: .run, kernel: resources.appendingPathComponent("vmlinux"),
                         rootfs: resources.appendingPathComponent("rootfs.erofs"),
                         disk: resources.appendingPathComponent("payload.erofs"), diskReadOnly: true,
                         share: share, network: false, cpus: cpus, memoryGiB: memoryGiB, consoleLog: console)
    boot.scratch = scratch
    // The first result is the first Code Card's: built-in Cards supply and route values.
    let code = Set((manifest["cards"] as? [[String: Any]] ?? []).filter { $0["type"] as? String == "code" }
      .compactMap { $0["id"] as? String })
    var firstResult: TimeInterval?
    try boot.run { line in
      let elapsed = Date().timeIntervalSince(began)
      if line.hasPrefix("PACK-CARD ") {
        let words = line.split(separator: " ")
        if firstResult == nil, words.count == 3, words[2] == "succeeded", code.contains(String(words[1])) {
          firstResult = elapsed
        }
        print(String(format: "[%6.1f s] %@", elapsed, words.dropFirst().joined(separator: " ")))
      } else if line.hasPrefix("PACK-STAGE ") || line.hasPrefix("PACK-FAILED ") || line.hasPrefix("PACK-RESULT ") {
        print(String(format: "[%6.1f s] %@", elapsed, String(line.split(separator: " ", maxSplits: 1).last ?? "")))
      } else if verbose || line.hasPrefix("Traceback") || line.contains("Error") {
        print("  \(line)")
      }
    }
    let result = share.appendingPathComponent("result")
    let status = (try? String(contentsOf: share.appendingPathComponent("run.status"), encoding: .utf8))?
      .trimmingCharacters(in: .whitespacesAndNewlines)
    try fm.createDirectory(at: output.deletingLastPathComponent(), withIntermediateDirectories: true)
    if fm.fileExists(atPath: result.path) {
      try fm.moveItem(at: result, to: output)
    } else {
      try fm.createDirectory(at: output, withIntermediateDirectories: true)
    }
    try? fm.copyItem(at: console, to: output.appendingPathComponent("console.log"))
    let total = Date().timeIntervalSince(began)
    var timing: [String: Any] = ["total_seconds": total]
    if let firstResult { timing["first_result_seconds"] = firstResult }
    try? JSONSerialization.data(withJSONObject: timing, options: [.prettyPrinted]).write(to: output.appendingPathComponent("timing.json"))
    let summary = (try? JSONSerialization.jsonObject(with: Data(contentsOf: output.appendingPathComponent("summary.json")))) as? [String: Any]
    for item in summary?["results"] as? [[String: Any]] ?? [] {
      print("  result \(item["card"] ?? "").\(item["port"] ?? ""): \(item["file"] ?? item["json"] ?? "")")
    }
    print(String(format: "%@: %@ in %.1f s; results in %@", name, status == "0" ? "succeeded" : "failed", total, output.path))
    if opened { try? shell("/usr/bin/open", [output.path]) }
    return status == "0" ? 0 : 1
  }
}
