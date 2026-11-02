import AppKit
import QuartzCore
import SwiftUI
import TsugumiRendererC

public struct ChartSeries: Equatable {
  public var id: String
  public var label: String
  public var x: [Double]
  public var y: [Double]
  public init(id: String, label: String, x: [Double], y: [Double]) {
    self.id = id
    self.label = label
    self.x = x
    self.y = y
  }
}

/// Live line chart drawn by Dawn into a CAMetalLayer. Data updates only mark
/// the view dirty; drawing happens at most once per display refresh.
public struct DawnLineChart: NSViewRepresentable {
  public var series: [ChartSeries]
  public init(series: [ChartSeries]) { self.series = series }

  public func makeNSView(context: Context) -> ChartNSView { ChartNSView() }

  public func updateNSView(_ view: ChartNSView, context: Context) {
    view.update(series)
  }

  public static func dismantleNSView(_ view: ChartNSView, coordinator: ()) {
    view.teardown()
  }
}

public final class ChartNSView: NSView, CALayerDelegate {
  private var renderer: OpaquePointer?
  private var link: CADisplayLink?
  private var series: [ChartSeries] = []
  public private(set) var lastError: String?
  private static let palette: [(Float, Float, Float)] = [(0.36, 0.67, 1.0), (1.0, 0.62, 0.26), (0.47, 0.86, 0.55), (0.94, 0.44, 0.62)]

  override public init(frame: NSRect) {
    super.init(frame: frame)
    wantsLayer = true
  }

  required init?(coder: NSCoder) { fatalError() }

  override public func makeBackingLayer() -> CALayer {
    let l = CAMetalLayer()
    l.pixelFormat = .bgra8Unorm
    l.isOpaque = true
    return l
  }

  override public func viewDidMoveToWindow() {
    super.viewDidMoveToWindow()
    guard window != nil, renderer == nil, let layer = layer as? CAMetalLayer else { return }
    var err = [CChar](repeating: 0, count: 256)
    renderer = tsg_renderer_create(Unmanaged.passUnretained(layer).toOpaque(), &err, err.count)
    if renderer == nil { lastError = String(cString: err) }
    resize()
    push()
    let l = displayLink(target: self, selector: #selector(tick))
    l.add(to: .main, forMode: .common)
    link = l
  }

  override public func viewDidChangeBackingProperties() {
    super.viewDidChangeBackingProperties()
    resize()
  }

  override public func setFrameSize(_ newSize: NSSize) {
    super.setFrameSize(newSize)
    resize()
  }

  private func resize() {
    guard let r = renderer, let layer = layer as? CAMetalLayer else { return }
    let scale = window?.backingScaleFactor ?? 2
    layer.contentsScale = scale
    let w = UInt32(max(1, bounds.width * scale)), h = UInt32(max(1, bounds.height * scale))
    layer.drawableSize = CGSize(width: Int(w), height: Int(h))
    tsg_renderer_resize(r, w, h, Float(scale))
  }

  func update(_ s: [ChartSeries]) {
    guard s != series else { return }
    series = s
    push()
  }

  private func push() {
    guard let r = renderer else { return }
    // Arrays must stay alive for the call; the renderer copies them.
    let xs = series.map { $0.x }, ys = series.map { $0.y }
    var raw: [tsg_series] = []
    var pointers: [(UnsafeMutableBufferPointer<Double>, UnsafeMutableBufferPointer<Double>)] = []
    for (i, s) in series.enumerated() {
      let n = min(xs[i].count, ys[i].count)
      let px = UnsafeMutableBufferPointer<Double>.allocate(capacity: max(n, 1))
      let py = UnsafeMutableBufferPointer<Double>.allocate(capacity: max(n, 1))
      _ = px.initialize(from: xs[i].prefix(n))
      _ = py.initialize(from: ys[i].prefix(n))
      pointers.append((px, py))
      let c = Self.palette[i % Self.palette.count]
      raw.append(tsg_series(x: px.baseAddress, y: py.baseAddress, count: n, r: c.0, g: c.1, b: c.2, a: 1))
      _ = s
    }
    raw.withUnsafeBufferPointer { tsg_renderer_set_line_chart(r, $0.baseAddress, $0.count) }
    for (px, py) in pointers {
      px.deallocate()
      py.deallocate()
    }
  }

  @objc private func tick() {
    guard let r = renderer else { return }
    _ = tsg_renderer_render(r)
  }

  public func stats() -> tsg_renderer_stats {
    var s = tsg_renderer_stats()
    if let r = renderer { tsg_renderer_stats_get(r, &s) }
    return s
  }

  /// Releases the display link and every GPU resource.
  func teardown() {
    link?.invalidate()
    link = nil
    if let r = renderer { tsg_renderer_destroy(r) }
    renderer = nil
  }

  deinit {
    link?.invalidate()
    if let r = renderer { tsg_renderer_destroy(r) }
  }
}
