import Foundation
import PackagePlugin

/// Generates Swift bindings for protocol/*.fbs with the standard flatc found on
/// PATH (provided by `nix develop`) or in $TSUGUMI_FLATC.
@main
struct FlatcPlugin: BuildToolPlugin {
  func createBuildCommands(context: PluginContext, target: Target) throws -> [Command] {
    let protocolDir = context.package.directoryURL.appending(path: "../protocol").standardizedFileURL
    let flatc = try locateFlatc()
    let out = context.pluginWorkDirectoryURL.appending(path: "generated")
    let schemas = ["tsugumi_enums", "data", "worker", "tsugumi"]
    // The binary schema (with doc comments) drives the Agent's tool definitions.
    let bfbs = Command.buildCommand(
      displayName: "flatc tsugumi.bfbs",
      executable: flatc,
      arguments: ["-b", "--schema", "--bfbs-comments", "--bfbs-builtins", "-I", protocolDir.path(), "-o", out.path(),
                  protocolDir.appending(path: "tsugumi.fbs").path()],
      inputFiles: schemas.map { protocolDir.appending(path: "\($0).fbs") },
      outputFiles: [out.appending(path: "tsugumi.bfbs")])
    return [bfbs] + schemas.map { name in
      let input = protocolDir.appending(path: "\(name).fbs")
      return .buildCommand(
        displayName: "flatc \(name).fbs",
        executable: flatc,
        arguments: ["--swift", "--gen-object-api", "-I", protocolDir.path(), "-o", out.path(), input.path()],
        inputFiles: schemas.map { protocolDir.appending(path: "\($0).fbs") },
        outputFiles: [out.appending(path: "\(name)_generated.swift")])
    }
  }

  private func locateFlatc() throws -> URL {
    let env = ProcessInfo.processInfo.environment
    if let explicit = env["TSUGUMI_FLATC"] { return URL(fileURLWithPath: explicit) }
    for dir in (env["PATH"] ?? "").split(separator: ":") {
      let candidate = URL(fileURLWithPath: String(dir)).appending(path: "flatc")
      if FileManager.default.isExecutableFile(atPath: candidate.path()) { return candidate }
    }
    throw PluginError.flatcMissing
  }

  enum PluginError: Error, CustomStringConvertible {
    case flatcMissing
    var description: String { "flatc not found: run inside `nix develop` or set TSUGUMI_FLATC" }
  }
}
