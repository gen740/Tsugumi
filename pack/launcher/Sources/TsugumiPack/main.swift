import Foundation

// Inside an exported app (Contents/Resources/pack.json) this is the app's
// launcher; anywhere else it is the exporter.
setvbuf(stdout, nil, _IOLBF, 0)
let arguments = Array(CommandLine.arguments.dropFirst())
let resources = Bundle.main.resourceURL ?? URL(fileURLWithPath: "/nonexistent")
MainActor.assumeIsolated {
  do {
    if FileManager.default.fileExists(atPath: resources.appendingPathComponent("pack.json").path) {
      guard let run = try AppRun.parse(arguments, resources: resources) else { exit(0) }
      exit(try run.run())
    }
    if arguments.isEmpty || arguments.contains("--help") || arguments.contains("-h") {
      print(Export.usage)
      exit(arguments.isEmpty ? 2 : 0)
    }
    try Export.parse(arguments).run()
  } catch {
    FileHandle.standardError.write(Data("\(CommandLine.arguments[0].split(separator: "/").last ?? ""): \(error)\n".utf8))
    exit(1)
  }
}
