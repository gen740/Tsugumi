import Foundation
import PackagePlugin

/// Generates Swift bindings for protocol/*.fbs with the standard flatc found on
/// PATH (provided by `nix develop`) or in $TSUGUMI_FLATC.
@main
struct FlatcPlugin: BuildToolPlugin {
  func createBuildCommands(context: PluginContext, target: Target) throws -> [Command] {
    let protocolDir = context.package.directoryURL.appending(path: "../../protocol").standardizedFileURL
    let flatc = try locateFlatc()
    let out = context.pluginWorkDirectoryURL.appending(path: "generated")
    let schemas = ["card_enums", "tsugumi_enums", "rpc_trace", "worker_types", "data", "card", "worker", "local_worker", "remote_worker", "tsugumi", "service_common", "agent", "host", "vm_guest", "vm_host", "runtime_state"]
    let inputs = schemas.map { protocolDir.appending(path: "\($0).fbs") }
    // The layout a project keeps (layout.bin); its fields keep the documents'
    // camelCase names, which flatc warns about.
    let layout = protocolDir.appending(path: "layout_document.fbs")
    return [.buildCommand(
      displayName: "flatc Swift protocol bindings",
      executable: flatc,
      arguments: ["--swift", "--gen-object-api", "-I", protocolDir.path(), "-o", out.path()] + inputs.map { $0.path() },
      inputFiles: inputs,
      outputFiles: schemas.map { out.appending(path: "\($0)_generated.swift") }),
    .buildCommand(
      displayName: "flatc Swift layout bindings",
      executable: flatc,
      arguments: ["--no-warnings", "--swift", "--gen-object-api", "-o", out.path(), layout.path()],
      inputFiles: [layout],
      outputFiles: [out.appending(path: "layout_document_generated.swift")])]
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
