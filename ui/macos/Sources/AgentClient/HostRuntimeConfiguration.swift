import Foundation
@_exported import TsugumiVMTypes

/// Installation details passed to hostd; this never launches a helper or VM.
public enum HostRuntimeConfiguration {
  public static func arguments(support: URL, image: VMImage? = VMImage.find()) -> [String] {
    let environment = ProcessInfo.processInfo.environment
    let sibling = URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent().appendingPathComponent("tsugumi-vmhost")
    let helper = environment["TSUGUMI_VMHOST"].map { URL(fileURLWithPath: $0) }
      ?? Bundle.main.url(forAuxiliaryExecutable: "tsugumi-vmhost")
      ?? (FileManager.default.isExecutableFile(atPath: sibling.path) ? sibling : nil)
    guard let helper else { return [] }
    #if DEBUG
    var repo = URL(fileURLWithPath: #filePath)
    for _ in 0..<5 { repo.deleteLastPathComponent() }
    #else
    let repo = Bundle.main.bundleURL
    #endif
    let directory = image?.directory
      ?? environment["TSUGUMI_VM_IMAGE"].map { URL(fileURLWithPath: $0) }
      ?? Bundle.main.url(forResource: "tsugumi-vm", withExtension: nil) ?? repo.appendingPathComponent("dist/tsugumi-vm")
    return ["--vmhost", helper.path, "--vm-support", support.path, "--vm-image", directory.path]
  }
}
