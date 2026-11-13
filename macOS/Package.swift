// swift-tools-version:6.0
// Tsugumi macOS application (SwiftUI UI, RPC client, container launcher).
import Foundation
import PackageDescription

let clientLib = ProcessInfo.processInfo.environment["TSUGUMI_CLIENT_LIB_DIR"] ?? URL(fileURLWithPath: #filePath).deletingLastPathComponent()
  .appendingPathComponent("../build/client").standardizedFileURL.path

let package = Package(
  name: "Tsugumi",
  platforms: [.macOS("26.0")],
  products: [
    .library(name: "TsugumiAgentClient", targets: ["TsugumiAgentClient"]),
    .executable(name: "Tsugumi", targets: ["TsugumiApp"]),
    .executable(name: "tsugumi-vmctl", targets: ["TsugumiVMCtl"]),
    .executable(name: "tsugumi-vmhost", targets: ["TsugumiVMHost"]),
    .library(name: "TsugumiRPC", targets: ["TsugumiRPC"]),
    // For the core tests (CoreTests/, a package of their own without the app).
    .library(name: "TsugumiLauncher", targets: ["TsugumiLauncher"]),
  ],
  dependencies: [
    .package(url: "https://github.com/google/flatbuffers.git", exact: "25.12.19"),
    // Editor syntax colors (UI only; Cards and SDKs never use these).
    .package(url: "https://github.com/ChimeHQ/SwiftTreeSitter", exact: "0.25.0"),
    .package(url: "https://github.com/tree-sitter/tree-sitter-c", exact: "0.23.6"),
    .package(url: "https://github.com/tree-sitter/tree-sitter-cpp", exact: "0.23.4"),
    .package(url: "https://github.com/tree-sitter/tree-sitter-json", exact: "0.24.8"),
    .package(url: "https://github.com/tree-sitter/tree-sitter-bash", exact: "0.25.1"),
    .package(url: "https://github.com/tree-sitter/tree-sitter-go", exact: "0.25.0"),
    .package(url: "https://github.com/tree-sitter/tree-sitter-rust", exact: "0.24.2"),
    .package(url: "https://github.com/tree-sitter/tree-sitter-julia", exact: "0.25.0"),
    .package(url: "https://github.com/r-lib/tree-sitter-r", exact: "1.3.0"),
    // Terminal emulator of the project terminal (UI only). 1.11.2: later
    // versions need the Metal Toolchain (an Xcode download) for their shaders.
    .package(url: "https://github.com/migueldeicaza/SwiftTerm", exact: "1.11.2"),
    // Markdown and TeX math in chat replies, notes and Cards (UI only, native drawing).
    .package(url: "https://github.com/swiftlang/swift-markdown.git", exact: "0.7.3"),
    .package(url: "https://github.com/mgriebling/SwiftMath.git", exact: "1.7.3"),
  ],
  targets: [
    // macOS adapters for the core-owned Agent service.
    .target(
      name: "TsugumiAgentClient",
      dependencies: ["TsugumiRPC", .product(name: "FlatBuffers", package: "flatbuffers")],
      path: "Sources/AgentClient",
      swiftSettings: [.swiftLanguageMode(.v5)]),
    // Runs the standard flatc on protocol/*.fbs; output stays in .build.
    .plugin(name: "FlatcPlugin", capability: .buildTool(), path: "Plugins/FlatcPlugin"),
    .systemLibrary(name: "TsugumiClientC", path: "Sources/ClientC"),
    .target(
      name: "TsugumiRPC",
      dependencies: ["TsugumiClientC", .product(name: "FlatBuffers", package: "flatbuffers")],
      path: "Sources/RPC",
      swiftSettings: [.swiftLanguageMode(.v5)],
      linkerSettings: [.unsafeFlags(["-L", clientLib]), .linkedLibrary("curl"), .linkedLibrary("c++")],
      plugins: ["FlatcPlugin"]),
    .target(
      name: "TsugumiLauncher",
      dependencies: ["TsugumiRPC", .product(name: "FlatBuffers", package: "flatbuffers")],
      path: "Sources/Launcher",
      swiftSettings: [.swiftLanguageMode(.v5)]),
    // Drives the shared VM without the app (tests); needs the virtualization entitlement.
    .executableTarget(
      name: "TsugumiVMCtl",
      dependencies: ["TsugumiLauncher"],
      path: "Sources/VMCtl",
      swiftSettings: [.swiftLanguageMode(.v5)]),
    // The shared VM for the web server of the `tsugumi` command (cli/, web/server/vm.ts);
    // needs the virtualization entitlement.
    .executableTarget(
      name: "TsugumiVMHost",
      dependencies: ["TsugumiLauncher"],
      path: "Sources/VMHost",
      swiftSettings: [.swiftLanguageMode(.v5)]),
    // Grammars with no Swift package of their own (Sources/Grammars/README.md).
    .target(
      name: "TreeSitterNix", path: "Sources/Grammars/TreeSitterNix",
      exclude: ["LICENSE"], sources: ["src/parser.c", "src/scanner.c"],
      publicHeadersPath: "include", cSettings: [.headerSearchPath("src")]),
    // tree-sitter-python's manifest leaves out its scanner when used as a dependency.
    .target(
      name: "TreeSitterPython", path: "Sources/Grammars/TreeSitterPython",
      exclude: ["LICENSE"], sources: ["src/parser.c", "src/scanner.c"],
      // (the grammar's scanner as upstream wrote it: its size_t to uint32_t is intended)
      publicHeadersPath: "include", cSettings: [.headerSearchPath("src"), .unsafeFlags(["-Wno-shorten-64-to-32"])]),
    // tree-sitter-julia's manifest looks for src/scanner.c in the working directory, which
    // is not its package, so the parser it builds lacks the scanner. Only the scanner is
    // copied here (the 23 MB parser.c stays in the dependency).
    .target(
      name: "TreeSitterJuliaScanner", path: "Sources/Grammars/TreeSitterJuliaScanner",
      exclude: ["LICENSE"], sources: ["src/scanner.c"],
      publicHeadersPath: "include", cSettings: [.headerSearchPath("src")]),
    .target(
      name: "TreeSitterFBS", path: "Sources/Grammars/TreeSitterFBS",
      exclude: ["grammar.js", "src/grammar.json", "src/node-types.json"], sources: ["src/parser.c"],
      publicHeadersPath: "include", cSettings: [.headerSearchPath("src")]),
    .executableTarget(
      name: "TsugumiApp",
      dependencies: [
        "TsugumiRPC", "TsugumiLauncher",
        "TsugumiAgentClient",
        .product(name: "SwiftTreeSitter", package: "SwiftTreeSitter"),
        .product(name: "TreeSitterC", package: "tree-sitter-c"),
        .product(name: "TreeSitterCPP", package: "tree-sitter-cpp"),
        .product(name: "TreeSitterJSON", package: "tree-sitter-json"),
        .product(name: "TreeSitterBash", package: "tree-sitter-bash"),
        .product(name: "TreeSitterGo", package: "tree-sitter-go"),
        .product(name: "TreeSitterRust", package: "tree-sitter-rust"),
        .product(name: "TreeSitterJulia", package: "tree-sitter-julia"),
        .product(name: "TreeSitterR", package: "tree-sitter-r"),
        "TreeSitterNix", "TreeSitterFBS", "TreeSitterPython", "TreeSitterJuliaScanner",
        .product(name: "SwiftTerm", package: "SwiftTerm"),
        .product(name: "Markdown", package: "swift-markdown"),
        .product(name: "SwiftMath", package: "SwiftMath"),
      ],
      path: "Sources/App",
      exclude: ["Resources/TreeSitter/SOURCES.md"],
      resources: [.copy("Resources/TreeSitter")],
      swiftSettings: [.swiftLanguageMode(.v5)]),
    // The app's own logic. The model, RPC, Agent and launcher tests are the
    // package CoreTests/ (they build without the app).
    .testTarget(
      name: "TsugumiAppTests",
      dependencies: ["TsugumiRPC", "TsugumiLauncher", "TsugumiApp", "TsugumiAgentClient"],
      path: "Tests/TsugumiAppTests",
      swiftSettings: [.swiftLanguageMode(.v5)]),
    // Drawing benchmarks: TSUGUMI_BENCH=1 swift test --filter TsugumiBenchmarks.
    .testTarget(
      name: "TsugumiBenchmarks",
      dependencies: ["TsugumiApp"],
      path: "Tests/TsugumiBenchmarks",
      swiftSettings: [.swiftLanguageMode(.v5)]),
  ]
)
