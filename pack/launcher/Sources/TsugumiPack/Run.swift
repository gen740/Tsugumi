import Foundation

/// The exported app: runs the Workflow once on the recipient's values and
/// writes its results to a folder. No network device is attached.
struct AppRun {
  var resources: URL
  var manifest: [String: Any]
  var output: URL
  var params: [String: [String: Any]]
  /// Input Card id to the recipient's file that replaces its own.
  var files: [String: URL] = [:]
  var start: String?
  var cpus: Int
  var memoryGiB: Int
  var verbose: Bool
  /// Started from the Finder rather than a shell: the results folder is shown at the end.
  var opened = false

  var name: String { manifest["name"] as? String ?? "App" }

  func usage() -> String {
    var text = """
      usage: \(name) [--set CARD.PARAM=VALUE]... [--params FILE] [--file CARD=PATH]... [--start TRIGGER]
                     [--output DIR] [--cpus N] [--memory GiB] [--verbose]
             \(name) --describe

      Runs the Workflow "\(manifest["project"] as? String ?? name)" offline and writes each
      Card's outputs, display and logs under DIR (default ./\(name) results <time>).
      VALUE is JSON (a bare word is a string). Parameters:

      """
    let cards = manifest["cards"] as? [[String: Any]] ?? []
    let inputs = cards.filter { $0["file"] as? Bool == true }
    for card in cards {
      for p in card["params"] as? [[String: Any]] ?? [] {
        let fixed = p["fixed"] as? Bool == true ? "  (fixed at export)" : ""
        let value = (try? JSONSerialization.data(withJSONObject: p["default"] ?? NSNull(), options: [.fragmentsAllowed]))
          .map { String(decoding: $0, as: UTF8.self) } ?? ""
        text += "  \(card["id"] ?? "").\(p["name"] ?? "") : \(p["type"] ?? "") = \(value)\(fixed)\n"
      }
    }
    if !inputs.isEmpty {
      text += "\nFiles (--file CARD=PATH replaces the input Card's file):\n"
      for card in inputs {
        let current = (card["params"] as? [[String: Any]])?.first { $0["name"] as? String == "value" }?["default"] ?? ""
        text += "  \(card["id"] ?? "")  (exported with \(current))\n"
      }
    }
    return text
  }

  static func parse(_ arguments: [String], resources: URL) throws -> AppRun? {
    let manifest = try JSONSerialization.jsonObject(with: Data(contentsOf: resources.appendingPathComponent("pack.json"))) as? [String: Any] ?? [:]
    var run = AppRun(resources: resources, manifest: manifest, output: URL(fileURLWithPath: ""), params: [:], start: nil,
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
      case "--set":
        let assignment = try value()
        guard let eq = assignment.firstIndex(of: "="), let dot = assignment[..<eq].lastIndex(of: ".") else {
          throw PackError("--set needs CARD.PARAM=VALUE")
        }
        let card = String(assignment[..<dot])
        let param = String(assignment[assignment.index(after: dot)..<eq])
        let text = String(assignment[assignment.index(after: eq)...])
        let parsed = (try? JSONSerialization.jsonObject(with: Data(text.utf8), options: [.fragmentsAllowed])) ?? text
        run.params[card, default: [:]][param] = parsed
      case "--params":
        let file = try value()
        let loaded = try JSONSerialization.jsonObject(with: Data(contentsOf: URL(fileURLWithPath: file))) as? [String: [String: Any]]
        for (card, values) in loaded ?? [:] { run.params[card, default: [:]].merge(values) { $1 } }
      case "--file":
        let assignment = try value()
        guard let eq = assignment.firstIndex(of: "=") else { throw PackError("--file needs CARD=PATH") }
        let card = String(assignment[..<eq])
        let file = URL(fileURLWithPath: String(assignment[assignment.index(after: eq)...])).standardized
        let cards = manifest["cards"] as? [[String: Any]] ?? []
        guard cards.contains(where: { $0["id"] as? String == card && $0["file"] as? Bool == true }) else {
          throw PackError("\(card) is not an input Card with a file\n\n\(run.usage())")
        }
        var directory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: file.path, isDirectory: &directory), !directory.boolValue else {
          throw PackError("\(file.path) is not a file")
        }
        run.files[card] = file
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
    var request: [String: Any] = ["params": params]
    if let start { request["start"] = start }
    var named: [String: String] = [:]
    for (card, file) in files {
      let folder = share.appendingPathComponent("inputs/\(card)")
      try fm.createDirectory(at: folder, withIntermediateDirectories: true)
      try fm.copyItem(at: file, to: folder.appendingPathComponent(file.lastPathComponent))
      named[card] = file.lastPathComponent
    }
    request["files"] = named
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
    let code = Set((manifest["cards"] as? [[String: Any]] ?? []).filter { $0["template"] as? String == "code" }
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
