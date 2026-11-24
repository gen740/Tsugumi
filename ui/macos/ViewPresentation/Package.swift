// swift-tools-version:6.2
import Foundation
import PackageDescription

let hostLib = ProcessInfo.processInfo.environment["TSUGUMI_CLIENT_LIB_DIR"]
  ?? URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    .appendingPathComponent("../../../build/client").standardizedFileURL.path

let package = Package(
  name: "TsugumiViewPresentation", platforms: [.macOS("14.0")],
  products: [.library(name: "TsugumiViewPresentation", targets: ["TsugumiViewPresentation"])],
  targets: [
    .systemLibrary(name: "TsugumiViewHostC", path: "Sources/ViewHostC"),
    .target(
      name: "TsugumiViewPresentation", dependencies: ["TsugumiViewHostC"], path: "Sources/ViewPresentation",
      swiftSettings: [.swiftLanguageMode(.v5), .treatAllWarnings(as: .error)],
      linkerSettings: [
        .unsafeFlags(["-L", hostLib]),
        .linkedFramework("Metal"), .linkedFramework("QuartzCore"), .linkedFramework("IOSurface"),
        .linkedFramework("IOKit"), .linkedFramework("CoreGraphics"), .linkedFramework("Foundation"),
        .linkedFramework("AVFAudio"), .linkedFramework("GameController"), .linkedFramework("ImageIO"),
      ]),
  ])
