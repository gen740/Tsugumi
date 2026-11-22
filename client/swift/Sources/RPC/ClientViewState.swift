import FlatBuffers

public struct ClientViewState {
  public struct Camera {
    public var scope: [String]
    public var x: Double
    public var y: Double
    public var zoom: Double
    public init(scope: [String], x: Double, y: Double, zoom: Double) {
      self.scope = scope; self.x = x; self.y = y; self.zoom = zoom
    }
    public var valid: Bool { x.isFinite && y.isFinite && zoom.isFinite && zoom > 0 }
  }

  public var scope: [String]
  public var cameras: [Camera]
  public var selected_cards: [[String]]

  public init(scope: [String], cameras: [Camera], selectedCards: [[String]]) {
    self.scope = scope; self.cameras = cameras.filter(\.valid); self.selected_cards = selectedCards
  }

  public init?(bytes: [UInt8]) {
    guard !bytes.isEmpty else { return nil }
    var buffer = ByteBuffer(bytes: bytes)
    guard let view: Tsugumi_RPC_ClientView = try? getCheckedRoot(byteBuffer: &buffer) else { return nil }
    self.init(scope: view.scope.compactMap { $0 },
              cameras: view.cameras.map { Camera(scope: $0.scope.compactMap { $0 }, x: $0.x, y: $0.y, zoom: $0.zoom) },
              selectedCards: view.selectedCards.map { $0.segments.compactMap { $0 } })
  }

  public func bytes() -> [UInt8] {
    var view = Tsugumi_RPC_ClientViewT()
    view.scope = scope
    view.cameras = cameras.filter(\.valid).map {
      let camera = Tsugumi_RPC_ViewCameraT()
      camera.scope = $0.scope; camera.x = $0.x; camera.y = $0.y; camera.zoom = $0.zoom
      return camera
    }
    view.selectedCards = selected_cards.map { cardPath($0) }
    var builder = FlatBufferBuilder(initialSize: 256)
    let root = Tsugumi_RPC_ClientView.pack(&builder, obj: &view)
    builder.finish(offset: root)
    return builder.sizedByteArray
  }
}
