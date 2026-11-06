import AppKit
import Foundation
import SwiftUI
import Testing
@testable import TsugumiApp

/// What Home's demo list costs, in a real view hierarchy in a window placed off
/// the screen (TSUGUMI_BENCH=1). TSUGUMI_THUMB_OUT=<folder> also writes every
/// demo's thumbnail where its run ended, to look at. An optimized build:
/// swift test -c release -Xswiftc -enable-testing -Xswiftc -DDEBUG --filter demoThumbnailCost
@MainActor @Test(.enabled(if: ProcessInfo.processInfo.environment["TSUGUMI_BENCH"] == "1"))
func demoThumbnailCost() async throws {
  var repo = URL(fileURLWithPath: #filePath)
  for _ in 0..<4 { repo.deleteLastPathComponent() }
  let index = try JSONSerialization.jsonObject(with: Data(contentsOf: repo.appendingPathComponent("demos/index.json"))) as? [String: Any]
  let ids = (index?["demos"] as? [[String: Any]] ?? []).compactMap { $0["id"] as? String }

  func memory() -> Double {
    var info = task_vm_info_data_t()
    var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
    _ = withUnsafeMutablePointer(to: &info) { $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count) } }
    return Double(info.phys_footprint) / 1_048_576
  }
  func cpu() -> Double { Double(clock_gettime_nsec_np(CLOCK_PROCESS_CPUTIME_ID)) / 1e9 }

  _ = NSApplication.shared
  let window = NSWindow(contentRect: NSRect(x: -6000, y: -6000, width: 1000, height: 800), styleMask: [.borderless], backing: .buffered, defer: false)
  window.isReleasedWhenClosed = false

  // The frameworks' own first use is not the thumbnails' cost: the app has paid it by Home.
  let warm = try #require(await DemoScenes.shared.scene("card-guide"))
  window.contentView = NSHostingView(rootView: DemoThumbnailCanvas(scene: warm, frame: .done, size: CGSize(width: 300, height: 160), animate: false))
  window.orderBack(nil)
  window.contentView?.layoutSubtreeIfNeeded()
  try await Task.sleep(for: .seconds(1))
  window.orderOut(nil)
  window.contentView = nil
  try await Task.sleep(for: .seconds(3))
  let m0 = memory()

  // The list as Home makes it: lazily, in a scroll view.
  let host = NSHostingView(rootView: ScrollView {
    LazyVGrid(columns: [GridItem(.adaptive(minimum: 260), spacing: 14)], spacing: 14) {
      ForEach(ids, id: \.self) { DemoThumbnail(id: $0).frame(height: 190) }
    }
  }.frame(width: 1000, height: 800))
  host.frame = NSRect(x: 0, y: 0, width: 1000, height: 800)
  window.contentView = host
  window.orderBack(nil)

  let t0 = Date(), c0 = cpu()
  host.layoutSubtreeIfNeeded()
  host.displayIfNeeded()
  let firstLayout = Date().timeIntervalSince(t0) * 1000
  while DemoScenes.shared.played.isEmpty, Date().timeIntervalSince(t0) < 30 { try await Task.sleep(for: .milliseconds(20)) }
  let firstPlaying = Date().timeIntervalSince(t0) * 1000

  // How long the main thread takes to come back, while the first ones arrive.
  var worstMount = 0.0
  while Date().timeIntervalSince(t0) * 1000 < firstPlaying + 1000 {
    let b = Date()
    try await Task.sleep(for: .milliseconds(8))
    host.displayIfNeeded()
    worstMount = max(worstMount, Date().timeIntervalSince(b) * 1000 - 8)
  }

  // And while they play, with the CPU.
  var worst = 0.0, beats = 0
  let c1 = cpu(), t1 = Date()
  while Date().timeIntervalSince(t1) < 4 {
    let b = Date()
    try await Task.sleep(for: .milliseconds(8))
    host.displayIfNeeded()
    worst = max(worst, Date().timeIntervalSince(b) * 1000 - 8)
    beats += 1
  }
  let playing = (cpu() - c1) / 4 * 100

  // At rest, when the replays have ended.
  try await Task.sleep(for: .seconds(14))
  let c2 = cpu()
  try await Task.sleep(for: .seconds(3))
  let rest = (cpu() - c2) / 3 * 100
  let on = DemoScenes.shared.played.count
  #expect(on > 0, "nothing was drawn: an optimized build needs -Xswiftc -DDEBUG to find the data in the repository")
  print("THUMBS \(ids.count) demos: first layout \(Int(firstLayout)) ms, first playing \(Int(firstPlaying)) ms, \(on) came on screen, " +
        "main thread worst \(Int(worstMount)) ms arriving, \(Int(worst)) ms playing (\(beats) beats in 4 s), CPU playing \(Int(playing))%, at rest \(Int(rest))%, " +
        "footprint \(Int(m0)) → \(Int(memory())) MB (CPU to first playing \(String(format: "%.2f", c1 - c0)) s)")
  window.orderOut(nil)

  // Reading every demo once: what a list scrolled to its end has read.
  let t3 = Date(), m3 = memory()
  var scenes: [(String, DemoScene)] = []
  for id in ids { if let s = await DemoScenes.shared.scene(id) { scenes.append((id, s)) } }
  print("THUMBS reading all: \(Int(Date().timeIntervalSince(t3) * 1000)) ms, footprint \(Int(m3)) → \(Int(memory())) MB")

  guard let out = ProcessInfo.processInfo.environment["TSUGUMI_THUMB_OUT"] else { return }
  try FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)
  for (id, s) in scenes {
    let size = CGSize(width: 360, height: 200)
    let view = NSHostingView(rootView: ZStack(alignment: .topLeading) {
      Color(nsColor: .textBackgroundColor)
      DemoThumbnailCanvas(scene: s, frame: .done, size: size, animate: false)
    }.frame(width: size.width, height: size.height).clipped().environment(\.colorScheme, .dark))
    view.frame = CGRect(origin: .zero, size: size)
    window.contentView = view
    window.orderBack(nil)
    view.layoutSubtreeIfNeeded()
    try await Task.sleep(for: .milliseconds(120))
    guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { continue }
    view.cacheDisplay(in: view.bounds, to: rep)
    try rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: "\(out)/\(id).png"))
    window.orderOut(nil)
  }
}
