// swift-tools-version:6.2
import PackageDescription

// tsugumi-pack: exports a Project into an app, and is that app's launcher.
let package = Package(
  name: "TsugumiPack", platforms: [.macOS("14.0")],
  products: [.executable(name: "tsugumi-pack", targets: ["TsugumiPack"])],
  targets: [
    .executableTarget(
      name: "TsugumiPack", path: "Sources/TsugumiPack",
      swiftSettings: [.swiftLanguageMode(.v5), .treatAllWarnings(as: .error)])
  ])
