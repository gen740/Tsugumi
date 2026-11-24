import AppKit
import Carbon.HIToolbox
import IOKit.hidsystem
import Observation
import QuartzCore
import SwiftUI
import TsugumiViewHostC

struct DawnCardView: View {
  let session: ProjectSession
  let cardKey: CardKey
  let index: Int
  let view: CardViewDeclaration
  @State private var host: DawnViewHost?
  @State private var error: String?

  private struct Selection: Hashable {
    let card: CardKey
    let index: Int
    let view: CardViewDeclaration
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      if let host {
        DawnViewRepresentable(host: host).id(ObjectIdentifier(host))
          .frame(maxWidth: .infinity, maxHeight: .infinity)
      } else if error == nil {
        ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
      }
      if let message = error ?? host?.error {
        Text(verbatim: message).font(.caption).foregroundStyle(.red).textSelection(.enabled)
      }
    }
    .task(id: Selection(card: cardKey, index: index, view: view)) { await show() }
  }

  private func show() async {
    host = nil
    error = nil
    do {
      let wasm = try await session.viewFiles.read(card: path(cardKey), index: index, name: view.name)
      try Task.checkCancellation()
      let created = DawnViewHost(session: session, cardKey: cardKey, name: view.name, wasm: wasm)
      host = created
      defer { created.close() }
      while !Task.isCancelled { try await Task.sleep(for: .seconds(3600)) }
    } catch {
      if !Task.isCancelled { self.error = error.localizedDescription }
    }
  }
}

private struct DawnViewRepresentable: NSViewRepresentable {
  let host: DawnViewHost

  func makeNSView(context: Context) -> DawnLayerView { DawnLayerView(host: host) }
  func updateNSView(_ view: DawnLayerView, context: Context) {}
  static func dismantleNSView(_ view: DawnLayerView, coordinator: ()) { view.stopDrawing() }
}

/// Owns the Wasm instance and its relay attachment. Every native call, including
/// the synchronous reverse message callback, runs on the main actor.
@MainActor @Observable private final class DawnViewHost: NSObject {
  private let session: ProjectSession
  private let cardKey: CardKey
  private let name: String
  @ObservationIgnored private var wasm: Data?
  @ObservationIgnored private var attachment: UUID?
  @ObservationIgnored private var native: OpaquePointer?
  @ObservationIgnored private var accepts: UInt32 = 0
  @ObservationIgnored private var link: CADisplayLink?
  @ObservationIgnored private var reverse: [Data] = []
  @ObservationIgnored private var reverseBytes = 0
  @ObservationIgnored private var reverseTask: Task<Void, Never>?
  private(set) var error: String?

  init(session: ProjectSession, cardKey: CardKey, name: String, wasm: Data) {
    self.session = session
    self.cardKey = cardKey
    self.name = name
    self.wasm = wasm
    super.init()
  }

  private static let send: tsugumi_view_host_send_fn = { context, bytes, size in
    guard let context else { return -1 }
    return MainActor.assumeIsolated {
      Unmanaged<DawnViewHost>.fromOpaque(context).takeUnretainedValue().enqueue(bytes, size: Int(size))
    }
  }

  private func create(in layer: CAMetalLayer, view: DawnLayerView, width: UInt32, height: UInt32, scale: Float) {
    guard let wasm else { return }
    self.wasm = nil
    let (created, diagnostic) = dawnDiagnostic { error in
      wasm.withUnsafeBytes { bytes in
        tsugumi_view_host_create(bytes.bindMemory(to: UInt8.self).baseAddress, bytes.count,
          Unmanaged.passUnretained(layer).toOpaque(), width, height, scale,
          Self.send, Unmanaged.passUnretained(self).toOpaque(), error.baseAddress, error.count)
      }
    }
    guard let created else {
      fail(diagnostic)
      return
    }
    native = created
    accepts = tsugumi_view_host_accepts(created)
    installDisplayLink(for: view)
    drawFrame()
    guard native != nil else { return }
    attachment = session.cardViews.attach(cardKey, view: name) { [weak self] message, complete in
      if let self { receive(message, complete: complete) }
      else { complete() }
    }
  }

  private func installDisplayLink(for view: DawnLayerView) {
    guard link == nil else { return }
    let link = view.displayLink(target: self, selector: #selector(drawFrame))
    link.isPaused = true
    link.add(to: .main, forMode: .common)
    self.link = link
  }

  func resize(in layer: CAMetalLayer, view: DawnLayerView, width: UInt32, height: UInt32, scale: Float) {
    if let native {
      installDisplayLink(for: view)
      finish(tsugumi_view_host_resize(native, width, height, scale), native: native)
    } else {
      create(in: layer, view: view, width: width, height: height, scale: scale)
    }
  }

  func stopDrawing() {
    link?.invalidate()
    link = nil
  }

  @objc private func drawFrame() {
    guard let native else { return }
    let result = tsugumi_view_host_draw(native)
    finish(result, native: native, schedule: result & 1 != 0)
  }

  private func finish(_ result: Int32, native: OpaquePointer, schedule: Bool = true) {
    let diagnostic = tsugumi_view_host_error(native).map { String(cString: $0) }
    if result < 0 {
      fail(diagnostic ?? "")
    } else {
      if let diagnostic, diagnostic != error { error = diagnostic }
      link?.isPaused = !schedule
    }
  }

  private func fail(_ diagnostic: String) {
    error = diagnostic.isEmpty ? String(localized: "Dawn ビューを実行できませんでした") : diagnostic
    close()
  }

  func close() {
    stopDrawing()
    if let attachment {
      session.cardViews.detach(attachment)
      self.attachment = nil
    }
    if let native {
      tsugumi_view_host_destroy(native)
      self.native = nil
    }
    accepts = 0
    wasm = nil
    reverseTask?.cancel()
    reverseTask = nil
    reverse.removeAll()
    reverseBytes = 0
  }

  private func receive(_ message: CardViewMessage, complete: () -> Void) {
    guard let native, accepts & 1 != 0 else { complete(); return }
    let result: Int32 = switch message {
    case .data(let data):
      data.withUnsafeBytes {
        tsugumi_view_host_message(native, $0.bindMemory(to: UInt8.self).baseAddress, UInt32($0.count))
      }
    }
    complete()
    if self.native == native { finish(result, native: native) }
  }

  func pointer(_ event: NSEvent, kind: UInt32, at point: CGPoint) {
    guard let native, accepts & 2 != 0 else { return }
    // AppKit reports content displacement. DOM wheel deltas have the opposite sign.
    let x = kind == 3 ? -Float(event.scrollingDeltaX) : 0
    let y = kind == 3 ? -Float(event.scrollingDeltaY) : 0
    let result = tsugumi_view_host_pointer(native, kind, Float(point.x), Float(point.y),
      UInt32(NSEvent.pressedMouseButtons & 7), x, y, dawnModifiers(event.modifierFlags))
    finish(result, native: native)
  }

  var acceptsKeys: Bool { accepts & 4 != 0 }

  func key(_ event: NSEvent, kind: UInt32) {
    guard let native, acceptsKeys, let code = DawnKeys.usage(event.keyCode) else { return }
    finish(tsugumi_view_host_key(native, kind, code, dawnModifiers(event.modifierFlags)), native: native)
  }

  private func enqueue(_ bytes: UnsafePointer<UInt8>?, size: Int) -> Int32 {
    let limit = 1 << 24
    guard size <= limit, reverseBytes <= limit, size == 0 || bytes != nil else { return -1 }
    reverse.append(bytes.map { Data(bytes: $0, count: size) } ?? Data())
    reverseBytes += max(1, size)
    if reverseTask == nil {
      reverseTask = Task { await flushReverse() }
    }
    return 0
  }

  private func flushReverse() async {
    var batch: [Data] = []
    while !Task.isCancelled {
      // Drain each batch in arrival order without shifting the queue per send.
      if batch.isEmpty {
        batch = Array(reverse.reversed())
        reverse.removeAll()
      }
      guard let bytes = batch.popLast() else { break }
      do {
        guard let rpc = session.rpc else { throw CardViewFileError.unavailable }
        try await rpc.sendView(card: path(cardKey), view: name, bytes: bytes)
      } catch {
        if !Task.isCancelled { self.error = error.localizedDescription }
      }
      if Task.isCancelled { return }
      reverseBytes -= max(1, bytes.count)
    }
    reverseTask = nil
  }

  // TODO(open): Support Agent PNG snapshots of Dawn views.
}

@MainActor final class DawnLayerView: NSView {
  private weak var host: DawnViewHost?
  private var dimensions: (width: UInt32, height: UInt32, scale: Float)?
  private var pointerTracking: NSTrackingArea?

  fileprivate init(host: DawnViewHost) {
    self.host = host
    super.init(frame: .zero)
    wantsLayer = true
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

  override func makeBackingLayer() -> CALayer { CAMetalLayer() }
  override var isFlipped: Bool { true }
  override var acceptsFirstResponder: Bool { host?.acceptsKeys == true }

  override func viewDidMoveToWindow() {
    super.viewDidMoveToWindow()
    updateSurface()
  }

  override func viewDidChangeBackingProperties() {
    super.viewDidChangeBackingProperties()
    updateSurface()
  }

  override func setFrameSize(_ newSize: NSSize) {
    super.setFrameSize(newSize)
    updateSurface()
  }

  override func setBoundsSize(_ newSize: NSSize) {
    super.setBoundsSize(newSize)
    updateSurface()
  }

  override func layout() {
    super.layout()
    updateSurface()
  }

  fileprivate func stopDrawing() {
    dimensions = nil
    host?.stopDrawing()
  }

  private func updateSurface() {
    guard let window, let metal = layer as? CAMetalLayer, bounds.width > 0, bounds.height > 0 else {
      stopDrawing()
      return
    }
    let scale = window.backingScaleFactor
    let width = UInt32(max(1, (bounds.width * scale).rounded()))
    let height = UInt32(max(1, (bounds.height * scale).rounded()))
    let size = (width, height, Float(scale))
    guard dimensions.map({ $0 != size }) ?? true else { return }
    dimensions = size
    metal.contentsScale = scale
    metal.drawableSize = CGSize(width: Int(width), height: Int(height))
    host?.resize(in: metal, view: self, width: width, height: height, scale: Float(scale))
  }

  override func updateTrackingAreas() {
    super.updateTrackingAreas()
    if let pointerTracking { removeTrackingArea(pointerTracking) }
    let area = NSTrackingArea(rect: .zero, options: [.inVisibleRect, .activeInKeyWindow,
      .mouseMoved, .mouseEnteredAndExited], owner: self, userInfo: nil)
    addTrackingArea(area)
    pointerTracking = area
  }

  private func pointer(_ event: NSEvent, kind: UInt32) {
    if kind == 1, acceptsFirstResponder { window?.makeFirstResponder(self) }
    let point = convert(event.locationInWindow, from: nil)
    host?.pointer(event, kind: kind, at: CGPoint(x: point.x - bounds.minX, y: point.y - bounds.minY))
  }

  override func mouseMoved(with event: NSEvent) { pointer(event, kind: 0) }
  override func mouseDragged(with event: NSEvent) { pointer(event, kind: 0) }
  override func rightMouseDragged(with event: NSEvent) { pointer(event, kind: 0) }
  override func otherMouseDragged(with event: NSEvent) { pointer(event, kind: 0) }
  override func mouseDown(with event: NSEvent) { pointer(event, kind: 1) }
  override func rightMouseDown(with event: NSEvent) { pointer(event, kind: 1) }
  override func otherMouseDown(with event: NSEvent) { pointer(event, kind: 1) }
  override func mouseUp(with event: NSEvent) { pointer(event, kind: 2) }
  override func rightMouseUp(with event: NSEvent) { pointer(event, kind: 2) }
  override func otherMouseUp(with event: NSEvent) { pointer(event, kind: 2) }
  override func scrollWheel(with event: NSEvent) { pointer(event, kind: 3) }
  override func mouseExited(with event: NSEvent) { pointer(event, kind: 4) }
  // Key repeats use the same down event as the initial press.
  override func keyDown(with event: NSEvent) { host?.key(event, kind: 0) }
  override func keyUp(with event: NSEvent) { host?.key(event, kind: 1) }

  override func flagsChanged(with event: NSEvent) {
    guard let mask = DawnKeys.modifierMask(event.keyCode) else { return }
    host?.key(event, kind: event.modifierFlags.rawValue & mask != 0 ? 0 : 1)
  }
}

private func dawnModifiers(_ flags: NSEvent.ModifierFlags) -> UInt32 {
  var bits: UInt32 = 0
  if flags.contains(.shift) { bits |= 1 }
  if flags.contains(.control) { bits |= 2 }
  if flags.contains(.option) { bits |= 4 }
  if flags.contains(.command) { bits |= 8 }
  return bits
}

enum DawnKeys {
  static func usage(_ keyCode: UInt16) -> UInt32? { usages[Int(keyCode)] }

  // USB HID Keyboard/Keypad usage page (0x07), by physical macOS key.
  private static let usages: [Int: UInt32] = [
    kVK_ANSI_A: 0x04, kVK_ANSI_B: 0x05, kVK_ANSI_C: 0x06, kVK_ANSI_D: 0x07,
    kVK_ANSI_E: 0x08, kVK_ANSI_F: 0x09, kVK_ANSI_G: 0x0a, kVK_ANSI_H: 0x0b,
    kVK_ANSI_I: 0x0c, kVK_ANSI_J: 0x0d, kVK_ANSI_K: 0x0e, kVK_ANSI_L: 0x0f,
    kVK_ANSI_M: 0x10, kVK_ANSI_N: 0x11, kVK_ANSI_O: 0x12, kVK_ANSI_P: 0x13,
    kVK_ANSI_Q: 0x14, kVK_ANSI_R: 0x15, kVK_ANSI_S: 0x16, kVK_ANSI_T: 0x17,
    kVK_ANSI_U: 0x18, kVK_ANSI_V: 0x19, kVK_ANSI_W: 0x1a, kVK_ANSI_X: 0x1b,
    kVK_ANSI_Y: 0x1c, kVK_ANSI_Z: 0x1d,
    kVK_ANSI_1: 0x1e, kVK_ANSI_2: 0x1f, kVK_ANSI_3: 0x20, kVK_ANSI_4: 0x21,
    kVK_ANSI_5: 0x22, kVK_ANSI_6: 0x23, kVK_ANSI_7: 0x24, kVK_ANSI_8: 0x25,
    kVK_ANSI_9: 0x26, kVK_ANSI_0: 0x27,
    kVK_Return: 0x28, kVK_Escape: 0x29, kVK_Delete: 0x2a, kVK_Tab: 0x2b,
    kVK_Space: 0x2c, kVK_ANSI_Minus: 0x2d, kVK_ANSI_Equal: 0x2e, kVK_ANSI_LeftBracket: 0x2f,
    kVK_ANSI_RightBracket: 0x30, kVK_ANSI_Backslash: 0x31, kVK_ANSI_Semicolon: 0x33,
    kVK_ANSI_Quote: 0x34, kVK_ANSI_Grave: 0x35, kVK_ANSI_Comma: 0x36,
    kVK_ANSI_Period: 0x37, kVK_ANSI_Slash: 0x38, kVK_CapsLock: 0x39,
    kVK_F1: 0x3a, kVK_F2: 0x3b, kVK_F3: 0x3c, kVK_F4: 0x3d,
    kVK_F5: 0x3e, kVK_F6: 0x3f, kVK_F7: 0x40, kVK_F8: 0x41,
    kVK_F9: 0x42, kVK_F10: 0x43, kVK_F11: 0x44, kVK_F12: 0x45,
    kVK_Help: 0x49, kVK_Home: 0x4a, kVK_PageUp: 0x4b, kVK_ForwardDelete: 0x4c,
    kVK_End: 0x4d, kVK_PageDown: 0x4e, kVK_RightArrow: 0x4f, kVK_LeftArrow: 0x50,
    kVK_DownArrow: 0x51, kVK_UpArrow: 0x52,
    kVK_ANSI_KeypadClear: 0x53, kVK_ANSI_KeypadDivide: 0x54, kVK_ANSI_KeypadMultiply: 0x55,
    kVK_ANSI_KeypadMinus: 0x56, kVK_ANSI_KeypadPlus: 0x57, kVK_ANSI_KeypadEnter: 0x58,
    kVK_ANSI_Keypad1: 0x59, kVK_ANSI_Keypad2: 0x5a, kVK_ANSI_Keypad3: 0x5b,
    kVK_ANSI_Keypad4: 0x5c, kVK_ANSI_Keypad5: 0x5d, kVK_ANSI_Keypad6: 0x5e,
    kVK_ANSI_Keypad7: 0x5f, kVK_ANSI_Keypad8: 0x60, kVK_ANSI_Keypad9: 0x61,
    kVK_ANSI_Keypad0: 0x62, kVK_ANSI_KeypadDecimal: 0x63, kVK_ISO_Section: 0x64,
    kVK_ANSI_KeypadEquals: 0x67, kVK_JIS_KeypadComma: 0x85, kVK_JIS_Underscore: 0x87,
    kVK_JIS_Yen: 0x89, kVK_JIS_Kana: 0x90, kVK_JIS_Eisu: 0x91,
    kVK_Control: 0xe0, kVK_Shift: 0xe1, kVK_Option: 0xe2, kVK_Command: 0xe3,
    kVK_RightControl: 0xe4, kVK_RightShift: 0xe5, kVK_RightOption: 0xe6, kVK_RightCommand: 0xe7,
  ]

  static func modifierMask(_ keyCode: UInt16) -> UInt? {
    switch Int(keyCode) {
    case kVK_Control: UInt(NX_DEVICELCTLKEYMASK)
    case kVK_RightControl: UInt(NX_DEVICERCTLKEYMASK)
    case kVK_Shift: UInt(NX_DEVICELSHIFTKEYMASK)
    case kVK_RightShift: UInt(NX_DEVICERSHIFTKEYMASK)
    case kVK_Option: UInt(NX_DEVICELALTKEYMASK)
    case kVK_RightOption: UInt(NX_DEVICERALTKEYMASK)
    case kVK_Command: UInt(NX_DEVICELCMDKEYMASK)
    case kVK_RightCommand: UInt(NX_DEVICERCMDKEYMASK)
    case kVK_CapsLock: NSEvent.ModifierFlags.capsLock.rawValue
    default: nil
    }
  }
}

private func dawnDiagnostic<T>(_ call: (UnsafeMutableBufferPointer<CChar>) -> T) -> (T, String) {
  var buffer = [CChar](repeating: 0, count: 4096)
  let result = buffer.withUnsafeMutableBufferPointer { call($0) }
  let text = String(decoding: buffer.prefix(while: { $0 != 0 }).map(UInt8.init(bitPattern:)), as: UTF8.self)
  return (result, text)
}

/// Checks the Wasm contract without creating a Metal layer or GPU device.
@MainActor func dawnModuleError(_ wasm: [UInt8]) -> String? {
  let (result, diagnostic) = dawnDiagnostic { error in
    wasm.withUnsafeBufferPointer {
      tsugumi_view_host_check($0.baseAddress, $0.count, error.baseAddress, error.count)
    }
  }
  return result < 0 ? diagnostic : nil
}
