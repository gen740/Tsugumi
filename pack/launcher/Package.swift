// swift-tools-version:6.2
import PackageDescription

// tsugumi-pack: exports a Project into an app, and is that app's launcher.
let package = Package(
  name: "TsugumiPack", platforms: [.macOS("26.0")],
  products: [.executable(name: "tsugumi-pack", targets: ["TsugumiPack"])],
  dependencies: [.package(path: "../../ui/macos/ViewPresentation")],
  targets: [
    .executableTarget(
      name: "TsugumiPack", dependencies: [.product(name: "TsugumiViewPresentation", package: "ViewPresentation")],
      path: "Sources/TsugumiPack",
      swiftSettings: [.swiftLanguageMode(.v5), .treatAllWarnings(as: .error)])
  ])
