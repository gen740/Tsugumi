import Foundation
import SwiftUI
import Testing
@testable import TsugumiApp

/// One frame of the point cloud (what a drag redraws), offscreen: the
/// bitmap against one SwiftUI path per point. Measured 2026-11-03 (M3 Max,
/// release, 100 000 points): 12 ms → 1.0 ms.
@MainActor @Test(.enabled(if: ProcessInfo.processInfo.environment["TSUGUMI_BENCH"] == "1"))
func pointCloudFrameTime() {
  let n = 100_000
  var x: [Float] = [], y: [Float] = [], z: [Float] = [], c: [UInt32] = []
  for i in 0..<n {
    let t = Float(i) * 0.001
    x.append(sin(t) * 3); y.append(cos(t * 1.3) * 2); z.append(sin(t * 0.7)); c.append(UInt32(i % 5) * 0x203040)
  }
  func time(_ f: () -> Void) -> Double {
    let t0 = Date(); for _ in 0..<5 { f() }; return Date().timeIntervalSince(t0) / 5 * 1000
  }
  // The earlier way: one SwiftUI path per point.
  func old(_ ctx: inout GraphicsContext, _ size: CGSize) {
    for i in 0..<n {
      let sx = size.width / 2 + Double(x[i]) * 50, sy = size.height / 2 - Double(y[i]) * 50
      let cc = c[i]
      let color = Color(red: Double((cc >> 16) & 0xFF) / 255, green: Double((cc >> 8) & 0xFF) / 255, blue: Double(cc & 0xFF) / 255)
      ctx.fill(Path(ellipseIn: CGRect(x: sx - 1.5, y: sy - 1.5, width: 3, height: 3)), with: .color(color))
    }
  }
  let baseline = time {
    let r = ImageRenderer(content: Canvas { _, _ in }.frame(width: 600, height: 400)); r.scale = 2; _ = r.cgImage
  }
  let earlier = time {
    let r = ImageRenderer(content: Canvas { ctx, size in old(&ctx, size) }.frame(width: 600, height: 400)); r.scale = 2; _ = r.cgImage
  }
  let cloud = PreparedCloud(x: x, y: y, z: z, colors: c)
  let now = time { _ = cloud.render(yaw: 0.6, pitch: 0.4, zoom: 1, size: CGSize(width: 600, height: 400), pixelScale: 2) }
  print("POINTCLOUD 100k: earlier \(Int(earlier - baseline)) ms, bitmap \(now) ms")
  #if !DEBUG
  // Optimized builds (the app's): the bitmap is far faster. Debug builds
  // check every array access and say little about either.
  #expect(now * 4 < earlier - baseline)
  #endif
}
