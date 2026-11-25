import AppKit
import Carbon.HIToolbox
import GameController
import IOKit.hidsystem
import Observation
import QuartzCore
import SwiftUI
import TsugumiViewHostC

struct DawnViewRepresentable: NSViewRepresentable {
  let host: DawnViewHost

  func makeNSView(context: Context) -> DawnLayerView { host.view }
  func updateNSView(_ view: DawnLayerView, context: Context) {}
  static func dismantleNSView(_ view: DawnLayerView, coordinator: ()) { view.stopDrawing() }
}

/// Owns the AppKit side of a Dawn view. The renderer and its synchronous
/// callbacks live on the view thread, while this object handles presentation.
@MainActor @Observable final class DawnViewHost: NSObject {
  private let sendToCard: (String, Data) async throws -> Void
  let flags: UInt32
  @ObservationIgnored lazy var view = DawnLayerView(host: self)
  @ObservationIgnored private var outbox = CardViewOutbox()
  @ObservationIgnored private var wasm: Data?
  @ObservationIgnored private var worker: DawnViewWorker?
  @ObservationIgnored private var recipients: Set<String>
  @ObservationIgnored private var accepts: UInt32 = 0
  @ObservationIgnored private let testClock: DawnTestClock?
  @ObservationIgnored private var link: CADisplayLink?
  @ObservationIgnored private var drawing = false
  @ObservationIgnored private var drawRequested = false
  @ObservationIgnored private var reverse: [(String, Data, () -> Void)] = []
  @ObservationIgnored private var reverseTask: Task<Void, Never>?
  @ObservationIgnored private var audio: DawnAudio?
  @ObservationIgnored private var gamepadTimer: Timer?
  @ObservationIgnored private var pads: [ObjectIdentifier: (UInt32, DawnGamepadState)] = [:]
  @ObservationIgnored private var closed = false
  private(set) var textArea: CGRect?
  private(set) var waiting = false
  private(set) var error: String?

  init(wasm: Data, flags: UInt32, recipients: Set<String>, testClock: DawnTestClock?,
       sendToCard: @escaping (String, Data) async throws -> Void) {
    self.flags = flags
    self.recipients = recipients
    self.sendToCard = sendToCard
    self.wasm = wasm
    self.testClock = testClock
    super.init()
  }

  func setRecipient(_ card: String, registered: Bool) {
    if registered { recipients.insert(card) }
    else { recipients.remove(card) }
    worker?.setRecipient(card, registered: registered)
  }

  private func create(in layer: CAMetalLayer, width: UInt32, height: UInt32, scale: Float) {
    guard let wasm else { return }
    self.wasm = nil
    guard let worker = DawnViewWorker(recipients: recipients, onWaiting: { [weak self] waiting in
      guard let self, !closed else { return }
      self.waiting = waiting
      if waiting { view.releasePointer() }
    }, onSend: { [weak self] to, data, complete in
      guard let self, !closed else { complete(); return }
      enqueue(to, bytes: data, complete: complete)
    }, onAudioFailure: { [weak self] diagnostic in
      self?.fail(diagnostic)
    }) else {
      fail(nil)
      return
    }
    self.worker = worker
    let timeOrigin = testClock?.seconds ?? -1
    worker.submit { [weak self] worker in
      let (accepts, diagnostic) = worker.create(wasm: wasm, layer: layer, width: width, height: height,
                                                scale: scale, timeOrigin: timeOrigin)
      DispatchQueue.main.async {
        guard let self, !self.closed else { return }
        guard let accepts else { self.fail(diagnostic); return }
        self.accepts = accepts
        if let testClock = self.testClock {
          self.drawTestFrame(at: testClock.seconds)
          return
        }
        self.requestDraw()
        // A background window may not receive display link callbacks yet.
        let refresh = max(1, self.view.window?.screen?.maximumFramesPerSecond ?? 60)
        self.enqueueDraw(present: CACurrentMediaTime() + 1.0 / Double(refresh))
        if accepts & UInt32(TSUGUMI_HOST_GAMEPAD) != 0 { self.watchGamepads() }
        if self.audioRequested { self.startAudio() }
      }
    }
    while let (message, complete) = outbox.take() { send(message, complete: complete) }
  }

  private func startAudio() {
    do {
      audio = try DawnAudio()
      if let ring = audio?.ring { worker?.submit { $0.audio = ring } }
    } catch {
      // The view clock and drawing continue even when an output device fails.
      self.error = error.localizedDescription
    }
  }

  private func installDisplayLink(for view: DawnLayerView) {
    guard link == nil, testClock == nil else { return }
    let link = view.displayLink(target: self, selector: #selector(drawFrame(_:)))
    link.isPaused = true
    link.add(to: .main, forMode: .common)
    self.link = link
  }

  func resize(in layer: CAMetalLayer, view: DawnLayerView, width: UInt32, height: UInt32, scale: Float) {
    guard !closed else { return }
    installDisplayLink(for: view)
    if worker != nil {
      let at = currentTime
      perform { tsugumi_view_host_resize($0, width, height, scale, at) }
    } else {
      create(in: layer, width: width, height: height, scale: scale)
    }
  }

  func stopDrawing() {
    link?.invalidate()
    link = nil
  }

  private func requestDraw() {
    guard testClock == nil else { return }
    drawRequested = true
    if !drawing { link?.isPaused = false }
  }

  @objc private func drawFrame(_ link: CADisplayLink) {
    enqueueDraw(present: link.targetTimestamp)
  }

  private func enqueueDraw(present: Double) {
    guard !drawing, drawRequested else { return }
    drawing = true
    drawRequested = false
    link?.isPaused = true
    submitDraw(present: present) { [weak self] result in
      guard let self else { return }
      self.drawing = false
      if let result, result & 1 != 0 || self.drawRequested { self.requestDraw() }
    }
  }

  func drawTestFrame(at time: Double) {
    submitDraw(present: time)
  }

  private func submitDraw(present: Double,
                          complete: @escaping @MainActor (Int32?) -> Void = { _ in }) {
    guard let worker, !closed else { complete(nil); return }
    worker.submit { [weak self] worker in
      let result = worker.native.map { tsugumi_view_host_draw($0, present) } ?? -1
      let area = result < 0 ? nil : worker.textArea()
      let diagnostic = worker.diagnostic
      DispatchQueue.main.async {
        guard let self, self.finish(result, diagnostic: diagnostic) else { complete(nil); return }
        if area != self.textArea {
          self.textArea = area
          self.view.updateIME()
        }
        complete(result)
      }
    } cancel: {
      DispatchQueue.main.async { complete(nil) }
    }
  }

  private func finish(_ result: Int32, diagnostic: String?) -> Bool {
    guard !closed else { return false }
    if result < 0 { fail(diagnostic); return false }
    if let diagnostic, diagnostic != error { error = diagnostic }
    return true
  }

  private func perform(_ operation: @escaping (OpaquePointer) -> Int32,
                       complete: @escaping @MainActor (Bool) -> Void = { _ in }) {
    guard let worker, !closed else { complete(false); return }
    worker.submit { [weak self] worker in
      let result = worker.native.map(operation) ?? -1
      let diagnostic = worker.diagnostic
      DispatchQueue.main.async {
        guard let self, self.finish(result, diagnostic: diagnostic) else { complete(false); return }
        self.requestDraw()
        complete(true)
      }
    } cancel: {
      DispatchQueue.main.async { complete(false) }
    }
  }

  private func fail(_ diagnostic: String?) {
    guard !closed else { return }
    error = diagnostic.flatMap { $0.isEmpty ? nil : $0 }
      ?? String(localized: "Dawn ビューを実行できませんでした")
    close()
  }

  func stop() {
    error = DawnViewError.stopped.localizedDescription
    close()
  }

  func close() {
    guard !closed else { return }
    closed = true
    waiting = false
    textArea = nil
    view.releasePointer()
    view.updateIME()
    stopDrawing()
    gamepadTimer?.invalidate()
    gamepadTimer = nil
    worker?.close()
    worker = nil
    audio?.stop()
    audio = nil
    outbox.discard()
    accepts = 0
    wasm = nil
    reverseTask?.cancel()
    reverseTask = nil
    let pending = reverse
    reverse.removeAll()
    for (_, _, complete) in pending { complete() }
  }

  func send(_ message: CardViewMessage, complete: @escaping () -> Void) {
    if wasm != nil {
      outbox.put(message, complete: complete)
      return
    }
    let at = currentTime
    perform({ native in
      switch message {
      case .data(let data):
        data.withUnsafeBytes {
          tsugumi_view_host_message(native, $0.bindMemory(to: UInt8.self).baseAddress, UInt32($0.count), at)
        }
      }
    }, complete: { _ in complete() })
  }

  var acceptsKeys: Bool { accepts & UInt32(TSUGUMI_HOST_KEY | TSUGUMI_HOST_TEXT) != 0 }
  var acceptsComposition: Bool { accepts & UInt32(TSUGUMI_HOST_COMPOSITION) != 0 }
  var audioRequested: Bool { accepts & UInt32(TSUGUMI_HOST_AUDIO) != 0 }
  var acceptsPointerLock: Bool { !closed && !waiting && flags & 2 != 0 }
  var hidesCursor: Bool { !closed && !waiting && flags & 1 != 0 }

  func pointer(_ event: NSEvent, kind: UInt32, buttons: UInt32, at point: CGPoint) {
    guard !waiting, accepts & UInt32(TSUGUMI_HOST_POINTER) != 0 else { return }
    // AppKit reports content displacement. DOM wheel deltas have the opposite sign.
    let x = kind == 3 ? -Float(event.scrollingDeltaX) : 0
    let y = kind == 3 ? -Float(event.scrollingDeltaY) : 0
    dispatch(.pointer(kind: kind, x: Float(point.x), y: Float(point.y), buttons: buttons,
      wheelX: x, wheelY: y, modifiers: dawnModifiers(event.modifierFlags)), at: event.timestamp)
  }

  func key(_ event: NSEvent, kind: UInt32) {
    guard !waiting, acceptsKeys, let code = DawnKeys.usage(event.keyCode) else { return }
    dispatch(.key(kind: kind, code: code, modifiers: dawnModifiers(event.modifierFlags)), at: event.timestamp)
  }

  private var currentTime: Double { testClock?.seconds ?? CACurrentMediaTime() }

  func dispatch(_ input: CardViewInput, at inputTime: Double,
                complete: @escaping @MainActor (Bool) -> Void = { _ in }) {
    let at = testClock?.seconds ?? inputTime
    perform({ native in
      switch input {
      case let .pointer(kind, x, y, buttons, wheelX, wheelY, modifiers):
        tsugumi_view_host_pointer(native, kind, x, y, buttons, wheelX, wheelY, modifiers, at)
      case let .key(kind, code, modifiers):
        tsugumi_view_host_key(native, kind, code, modifiers, at)
      case .text(let string):
        Data(string.utf8).withUnsafeBytes {
          tsugumi_view_host_text(native, $0.bindMemory(to: UInt8.self).baseAddress, UInt32($0.count), at)
        }
      case let .gamepad(pad, kind, buttons, lx, ly, rx, ry, lt, rt):
        tsugumi_view_host_gamepad(native, pad, kind, buttons, lx, ly, rx, ry, lt, rt, at)
      }
    }, complete: complete)
  }

  func composition(_ string: String, cursor: UInt32, at inputTime: Double) {
    guard acceptsComposition else { return }
    let at = testClock?.seconds ?? inputTime
    let data = Data(string.utf8)
    perform { native in
      data.withUnsafeBytes {
        tsugumi_view_host_composition(native, $0.bindMemory(to: UInt8.self).baseAddress,
          UInt32($0.count), cursor, at)
      }
    }
  }

  func sendInput(_ input: CardViewInput) async throws {
    try await withCheckedThrowingContinuation { (reply: CheckedContinuation<Void, Error>) in
      dispatch(input, at: currentTime) { success in
        if success { reply.resume() } else { reply.resume(throwing: DawnViewError.unavailable) }
      }
    }
  }

  func snapshot() async throws -> Data {
    guard let worker, !closed else { throw DawnViewError.unavailable }
    let testTime = testClock?.seconds
    return try await withCheckedThrowingContinuation { reply in
      worker.submit { [weak self] worker in
        let (png, diagnostic) = worker.snapshot(testTime: testTime)
        DispatchQueue.main.async {
          guard let self, !self.closed, let png else {
            reply.resume(throwing: DawnViewError.snapshotFailed)
            if let diagnostic { self?.fail(diagnostic) }
            return
          }
          reply.resume(returning: png)
        }
      } cancel: {
        reply.resume(throwing: DawnViewError.unavailable)
      }
    }
  }

  private func enqueue(_ to: String, bytes: Data, complete: @escaping () -> Void) {
    reverse.append((to, bytes, complete))
    if reverseTask == nil { reverseTask = Task { await flushReverse() } }
  }

  private func flushReverse() async {
    while !Task.isCancelled, !reverse.isEmpty {
      let (to, bytes, complete) = reverse.removeFirst()
      do {
        try await sendToCard(to, bytes)
      } catch {
        if !Task.isCancelled { self.error = error.localizedDescription }
      }
      complete()
    }
    reverseTask = nil
  }

  private func watchGamepads() {
    let timer = Timer(timeInterval: 1 / 60, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.pollGamepads() }
    }
    RunLoop.main.add(timer, forMode: .common)
    gamepadTimer = timer
    pollGamepads()
  }

  private func pollGamepads() {
    guard !waiting else { return }
    let controllers = GCController.controllers().compactMap { controller in
      controller.extendedGamepad.map { (ObjectIdentifier(controller), $0, controller.physicalInputProfile.lastEventTimestamp) }
    }
    let current = Set(controllers.map { $0.0 })
    for id in pads.keys.filter({ !current.contains($0) }) {
      if let (pad, _) = pads.removeValue(forKey: id) {
        dispatch(.gamepad(pad: pad, kind: 2, buttons: 0, lx: 0, ly: 0, rx: 0, ry: 0, lt: 0, rt: 0),
          at: CACurrentMediaTime())
      }
    }
    for (id, controller, timestamp) in controllers {
      let state = DawnGamepadState(controller)
      let pad: UInt32
      let kind: UInt32
      if let (index, previous) = pads[id] {
        guard state != previous else { continue }
        pad = index
        kind = 1
      } else {
        let occupied = Set(pads.values.map { $0.0 })
        pad = (0...UInt32(occupied.count)).first { !occupied.contains($0) }!
        kind = 0
      }
      pads[id] = (pad, state)
      dispatch(state.event(pad: pad, kind: kind), at: kind == 1 && timestamp > 0 ? timestamp : CACurrentMediaTime())
    }
  }
}

private struct DawnGamepadState: Equatable {
  let buttons: UInt32
  let lx, ly, rx, ry, lt, rt: Float

  init(_ pad: GCExtendedGamepad) {
    let standard: [GCControllerButtonInput?] = [
      pad.buttonA, pad.buttonB, pad.buttonX, pad.buttonY, pad.leftShoulder, pad.rightShoulder,
      pad.leftTrigger, pad.rightTrigger, pad.buttonOptions, pad.buttonMenu, pad.leftThumbstickButton,
      pad.rightThumbstickButton, pad.dpad.up, pad.dpad.down, pad.dpad.left, pad.dpad.right, pad.buttonHome,
    ]
    buttons = standard.enumerated().reduce(0) { bits, entry in
      entry.element?.isPressed == true ? bits | (1 << entry.offset) : bits
    }
    lx = pad.leftThumbstick.xAxis.value
    ly = -pad.leftThumbstick.yAxis.value
    rx = pad.rightThumbstick.xAxis.value
    ry = -pad.rightThumbstick.yAxis.value
    lt = pad.leftTrigger.value
    rt = pad.rightTrigger.value
  }

  func event(pad: UInt32, kind: UInt32) -> CardViewInput {
    .gamepad(pad: pad, kind: kind, buttons: buttons, lx: lx, ly: ly, rx: rx, ry: ry, lt: lt, rt: rt)
  }
}

@MainActor final class DawnLayerView: NSView, @preconcurrency NSTextInputClient {
  private weak var host: DawnViewHost?
  private var dimensions: (width: UInt32, height: UInt32, scale: Float)?
  private var pointerTracking: NSTrackingArea?
  private var pointerLocked = false
  private var releasingEscape = false
  private var cursorHidden = false
  private var pointerInside = false
  private var marked = NSAttributedString(string: "")
  private var selection = NSRange(location: 0, length: 0)
  private var inputTimestamp: Double?
  private var markedLabel: NSTextField?
  /// The buttons held, as the events this view received say (bit 0 primary, 1 secondary, 2 middle).
  private var buttons: UInt32 = 0

  fileprivate init(host: DawnViewHost) {
    self.host = host
    super.init(frame: .zero)
    wantsLayer = true
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

  override func makeBackingLayer() -> CALayer { CAMetalLayer() }
  override var isFlipped: Bool { true }
  override var acceptsFirstResponder: Bool {
    host?.acceptsKeys == true || host?.textArea != nil || host?.acceptsPointerLock == true
  }
  // The click that brings the window forward is also the view's, as a press and release.
  override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

  override func viewDidMoveToWindow() {
    super.viewDidMoveToWindow()
    let center = NotificationCenter.default
    center.removeObserver(self)
    if let window {
      center.addObserver(self, selector: #selector(windowLostFocus), name: NSWindow.didResignKeyNotification, object: window)
      center.addObserver(self, selector: #selector(windowLostFocus), name: NSApplication.didResignActiveNotification, object: nil)
    } else {
      windowLostFocus()
    }
    updateSurface()
  }

  @objc private func windowLostFocus() {
    pointerInside = false
    releasingEscape = false
    buttons = 0
    releasePointer()
    inputContext?.discardMarkedText()
  }

  override func resignFirstResponder() -> Bool {
    releasePointer()
    inputContext?.discardMarkedText()
    return super.resignFirstResponder()
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
    releasePointer()
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

  func releasePointer() {
    if pointerLocked {
      _ = CGAssociateMouseAndMouseCursorPosition(1)
      pointerLocked = false
    }
    if cursorHidden {
      NSCursor.unhide()
      cursorHidden = false
    }
  }

  private func updateCursor() {
    let shouldHide = pointerLocked || (pointerInside && host?.hidesCursor == true && window?.isKeyWindow == true)
    guard shouldHide != cursorHidden else { return }
    cursorHidden = shouldHide
    if shouldHide { NSCursor.hide() } else { NSCursor.unhide() }
  }

  private func pointer(_ event: NSEvent, kind: UInt32) {
    if kind == 1, acceptsFirstResponder { window?.makeFirstResponder(self) }
    if kind == 1, !pointerLocked, host?.acceptsPointerLock == true,
       window?.isKeyWindow == true, window?.firstResponder === self {
      pointerLocked = CGAssociateMouseAndMouseCursorPosition(0) == .success
      updateCursor()
    }
    buttons = DawnPointer.buttons(after: event.type, buttonNumber: event.buttonNumber, held: buttons)
    let location = convert(event.locationInWindow, from: nil)
    let point = pointerLocked
      ? CGPoint(x: kind == 0 ? event.deltaX : 0, y: kind == 0 ? event.deltaY : 0)
      : CGPoint(x: location.x - bounds.minX, y: location.y - bounds.minY)
    host?.pointer(event, kind: kind, buttons: buttons, at: point)
  }

  override func mouseEntered(with event: NSEvent) {
    pointerInside = true
    updateCursor()
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
  override func mouseExited(with event: NSEvent) {
    pointerInside = false
    updateCursor()
    if !pointerLocked { pointer(event, kind: 4) }
  }

  override func keyDown(with event: NSEvent) {
    if event.keyCode == UInt16(kVK_Escape), pointerLocked || releasingEscape {
      releasingEscape = true
      releasePointer()
      return
    }
    host?.key(event, kind: 0)
    guard let host, !host.waiting, host.textArea != nil else { return }
    inputTimestamp = event.timestamp
    defer { inputTimestamp = nil }
    interpretKeyEvents([event])
  }

  override func keyUp(with event: NSEvent) {
    if event.keyCode == UInt16(kVK_Escape), releasingEscape {
      releasingEscape = false
      return
    }
    host?.key(event, kind: 1)
  }

  private var compositionTime: Double { inputTimestamp ?? CACurrentMediaTime() }

  func updateIME() {
    if host?.textArea == nil {
      inputContext?.discardMarkedText()
      unmarkText()
    }
    updateMarkedLabel()
    inputContext?.invalidateCharacterCoordinates()
  }

  func insertText(_ string: Any, replacementRange: NSRange) {
    guard host?.textArea != nil else { return }
    let text = attributed(string).string
    unmarkText()
    host?.dispatch(.text(text), at: compositionTime)
  }

  func setMarkedText(_ string: Any, selectedRange: NSRange, replacementRange: NSRange) {
    guard host?.textArea != nil else { return }
    marked = attributed(string)
    let length = marked.length
    let location = min(selectedRange.location, length)
    selection = NSRange(location: location, length: min(selectedRange.length, length - location))
    // AppKit's cursor uses UTF-16 offsets. The view receives a UTF-8 byte
    // offset, so avoid cutting a surrogate pair in half.
    let units = Array(marked.string.utf16)
    var boundary = location
    if boundary > 0, boundary < units.count, (0xDC00...0xDFFF).contains(units[boundary]) {
      boundary -= 1
    }
    let cursor = String(decoding: units.prefix(boundary), as: UTF16.self).utf8.count
    host?.composition(marked.string, cursor: UInt32(cursor), at: compositionTime)
    updateMarkedLabel()
  }

  func unmarkText() {
    guard hasMarkedText() else { return }
    marked = NSAttributedString(string: "")
    selection = NSRange(location: 0, length: 0)
    host?.composition("", cursor: 0, at: compositionTime)
    updateMarkedLabel()
  }

  func selectedRange() -> NSRange { selection }
  func markedRange() -> NSRange { NSRange(location: hasMarkedText() ? 0 : NSNotFound, length: marked.length) }
  func hasMarkedText() -> Bool { marked.length != 0 }

  func attributedSubstring(forProposedRange range: NSRange, actualRange: NSRangePointer?) -> NSAttributedString? {
    guard range.location < marked.length else { return nil }
    let available = NSRange(location: range.location, length: min(range.length, marked.length - range.location))
    actualRange?.pointee = available
    return marked.attributedSubstring(from: available)
  }

  func validAttributesForMarkedText() -> [NSAttributedString.Key] {
    [.underlineStyle, .underlineColor, .backgroundColor, .foregroundColor]
  }

  func firstRect(forCharacterRange range: NSRange, actualRange: NSRangePointer?) -> NSRect {
    guard let area = host?.textArea, let window else { return .zero }
    actualRange?.pointee = NSRange(location: min(range.location, marked.length), length: 0)
    var rect = area.offsetBy(dx: bounds.minX, dy: bounds.minY)
    rect.size.width = max(1, rect.width)
    rect.size.height = max(1, rect.height)
    return window.convertToScreen(convert(rect, to: nil))
  }

  func characterIndex(for point: NSPoint) -> Int { selection.location }

  // Physical key commands were already delivered by keyDown.
  override func doCommand(by selector: Selector) {}

  private func attributed(_ value: Any) -> NSAttributedString {
    value as? NSAttributedString ?? NSAttributedString(string: value as? String ?? "")
  }

  private func updateMarkedLabel() {
    guard let host, let area = host.textArea, !host.acceptsComposition, hasMarkedText() else {
      markedLabel?.removeFromSuperview()
      markedLabel = nil
      return
    }
    let label = markedLabel ?? NSTextField(labelWithString: "")
    if markedLabel == nil {
      label.drawsBackground = true
      label.backgroundColor = .textBackgroundColor
      addSubview(label)
      markedLabel = label
    }
    label.attributedStringValue = marked
    label.sizeToFit()
    label.frame.origin = CGPoint(x: area.minX + bounds.minX, y: area.minY + bounds.minY)
  }

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

enum DawnPointer {
  /// The buttons held after an event, from the event itself rather than the global mouse
  /// state: a press adds its button, a release removes it, a drag holds its button and a
  /// plain move holds none. Bits as the view ABI has them: 0 primary, 1 secondary, 2 middle.
  static func buttons(after type: NSEvent.EventType, buttonNumber: Int, held: UInt32) -> UInt32 {
    let bit: UInt32 = switch type {
    case .leftMouseDown, .leftMouseUp, .leftMouseDragged: 1
    case .rightMouseDown, .rightMouseUp, .rightMouseDragged: 2
    case .otherMouseDown, .otherMouseUp, .otherMouseDragged: buttonNumber == 2 ? 4 : 0
    default: 0
    }
    switch type {
    case .leftMouseDown, .rightMouseDown, .otherMouseDown,
         .leftMouseDragged, .rightMouseDragged, .otherMouseDragged: return held | bit
    case .leftMouseUp, .rightMouseUp, .otherMouseUp: return held & ~bit
    case .mouseMoved: return 0
    default: return held
    }
  }
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

func dawnDiagnostic<T>(_ call: (UnsafeMutableBufferPointer<CChar>) -> T) -> (T, String) {
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
