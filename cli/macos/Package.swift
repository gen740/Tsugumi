// swift-tools-version:6.2
import PackageDescription
// This diagnostic command speaks Host RPC, like the UI; it never owns a VM.
let package = Package(name: "TsugumiVMCtl", platforms: [.macOS("26.0")],
  products: [.executable(name: "tsugumi-vmctl", targets: ["TsugumiVMCtl"])],
  dependencies: [.package(name: "TsugumiUI", path: "../../ui/macos")],
  targets: [.executableTarget(name: "TsugumiVMCtl", dependencies: [.product(name: "TsugumiLauncher", package: "TsugumiUI")], path: "Sources/VMCtl", swiftSettings: [.swiftLanguageMode(.v5), .treatAllWarnings(as: .error)])])
