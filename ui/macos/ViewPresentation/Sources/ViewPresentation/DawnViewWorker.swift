import AVFAudio
import Foundation
import ImageIO
import QuartzCore
import TsugumiViewHostC
import UniformTypeIdentifiers

enum DawnViewError: LocalizedError {
  case unavailable, snapshotFailed, stopped

  var errorDescription: String? {
    switch self {
    case .unavailable: String(localized: "ビューを操作できません")
    case .snapshotFailed: String(localized: "ビューのスナップショットを作成できませんでした")
    case .stopped: String(localized: "ビューを停止しました")
    }
  }
}

/// The queue, route set and Stop token are shared. The host, GPU and audio
/// producer are owned exclusively by the one thread running the queue.
final class DawnViewWorker: @unchecked Sendable {
  private struct Work {
    let run: (DawnViewWorker) -> Void
    let cancel: () -> Void
  }

  private let condition = NSCondition()
  private var pending: [Work] = []
  private var closed = false
  private var routes: Set<UUID>
  private var reverseBytes = 0
  private var reverseCount = 0
  private let stop: OpaquePointer
  private let onWaiting: @MainActor (Bool) -> Void
  private let onSend: @MainActor (UUID, Data, @escaping () -> Void) -> Void
  private let onAudioFailure: @MainActor (String?) -> Void
  private(set) var native: OpaquePointer?
  var audio: DawnAudioRing?

  init?(routes: Set<UUID>, onWaiting: @escaping @MainActor (Bool) -> Void,
        onSend: @escaping @MainActor (UUID, Data, @escaping () -> Void) -> Void,
        onAudioFailure: @escaping @MainActor (String?) -> Void) {
    guard let stop = tsugumi_view_stop_create() else { return nil }
    self.stop = stop
    self.routes = routes
    self.onWaiting = onWaiting
    self.onSend = onSend
    self.onAudioFailure = onAudioFailure
    let thread = Thread { [self] in run() }
    thread.name = "Tsugumi Dawn view"
    thread.qualityOfService = .userInteractive
    thread.start()
  }

  deinit { tsugumi_view_stop_destroy(stop) }

  func submit(_ run: @escaping (DawnViewWorker) -> Void, cancel: @escaping () -> Void = {}) {
    condition.lock()
    guard !closed else { condition.unlock(); cancel(); return }
    pending.append(Work(run: run, cancel: cancel))
    condition.signal()
    condition.unlock()
  }

  func registerRoute(_ route: UUID) {
    condition.lock()
    routes.insert(route)
    condition.unlock()
  }

  func close() {
    tsugumi_view_stop_request(stop)
    condition.lock()
    closed = true
    let queued = pending
    pending.removeAll()
    condition.signal()
    condition.unlock()
    for work in queued { work.cancel() }
  }

  private func run() {
    defer { if let native { tsugumi_view_host_destroy(native) } }
    while true {
      condition.lock()
      while pending.isEmpty && !closed {
        if audio != nil {
          _ = condition.wait(until: Date(timeIntervalSinceNow: 0.004))
          break
        }
        condition.wait()
      }
      let stop = closed
      let work = pending.isEmpty ? nil : pending.removeFirst()
      condition.unlock()
      if stop { return }
      autoreleasepool {
        work?.run(self)
        if let audio, let native, tsugumi_view_audio_ring_fill(audio.pointer, native) < 0 {
          self.audio = nil
          let error = diagnostic
          DispatchQueue.main.async { self.onAudioFailure(error) }
        }
      }
    }
  }

  private static let send: tsugumi_view_host_send_fn = { context, route, bytes, size in
    guard let context, let route, size == 0 || bytes != nil else { return -1 }
    return Unmanaged<DawnViewWorker>.fromOpaque(context).takeUnretainedValue()
      .send(viewRoute(route), bytes: bytes, size: Int(size))
  }

  private static let waiting: tsugumi_view_host_waiting_fn = { context, waiting in
    guard let context else { return }
    let worker = Unmanaged<DawnViewWorker>.fromOpaque(context).takeUnretainedValue()
    DispatchQueue.main.async { worker.onWaiting(waiting != 0) }
  }

  func create(wasm: Data, layer: CAMetalLayer, width: UInt32, height: UInt32, scale: Float)
    -> (UInt32?, String) {
    let (created, diagnostic) = dawnDiagnostic { error in
      wasm.withUnsafeBytes { bytes in
        tsugumi_view_host_create(bytes.bindMemory(to: UInt8.self).baseAddress, bytes.count,
          Unmanaged.passUnretained(layer).toOpaque(), width, height, scale, stop,
          Self.send, Self.waiting, Unmanaged.passUnretained(self).toOpaque(), error.baseAddress, error.count)
      }
    }
    native = created
    return (created.map(tsugumi_view_host_accepts), diagnostic)
  }

  var diagnostic: String? {
    native.flatMap(tsugumi_view_host_error).map { String(cString: $0) }
  }

  func textArea() -> CGRect? {
    guard let native else { return nil }
    var area = tsugumi_view_host_text_area()
    guard tsugumi_view_host_text_input(native, &area) != 0 else { return nil }
    return CGRect(x: CGFloat(area.x), y: CGFloat(area.y),
                  width: CGFloat(area.width), height: CGFloat(area.height))
  }

  func snapshot() -> (Data?, String?) {
    guard let native else { return (nil, nil) }
    let image = DawnSnapshot()
    let result = tsugumi_view_host_snapshot(native, CACurrentMediaTime(), DawnSnapshot.copy,
      Unmanaged.passUnretained(image).toOpaque())
    return (image.png, result == -1 ? diagnostic : nil)
  }

  private func send(_ route: UUID, bytes: UnsafePointer<UInt8>?, size: Int) -> Int32 {
    condition.lock()
    guard !closed, route == openerViewRoute || routes.contains(route),
          size <= 1 << 24, reverseBytes <= 1 << 24, reverseCount < 4096 else {
      condition.unlock()
      return -1
    }
    reverseBytes += max(1, size)
    reverseCount += 1
    condition.unlock()
    let data = bytes.map { Data(bytes: $0, count: size) } ?? Data()
    DispatchQueue.main.async {
      self.onSend(route, data) {
        self.condition.lock()
        self.reverseBytes -= max(1, size)
        self.reverseCount -= 1
        self.condition.unlock()
      }
    }
    return 0
  }
}

private final class DawnSnapshot {
  var png: Data?

  static let copy: tsugumi_view_host_pixels_fn = { context, bgra, width, height, stride in
    guard let context, let bgra else { return }
    let snapshot = Unmanaged<DawnSnapshot>.fromOpaque(context).takeUnretainedValue()
    let data = Data(bytes: bgra, count: Int(stride) * Int(height))
    let info = CGBitmapInfo.byteOrder32Little.union(
      CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue))
    guard let provider = CGDataProvider(data: data as CFData),
          let color = CGColorSpace(name: CGColorSpace.sRGB),
          let image = CGImage(width: Int(width), height: Int(height), bitsPerComponent: 8,
            bitsPerPixel: 32, bytesPerRow: Int(stride), space: color, bitmapInfo: info,
            provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent) else { return }
    let png = NSMutableData()
    guard let destination = CGImageDestinationCreateWithData(png, UTType.png.identifier as CFString, 1, nil) else { return }
    CGImageDestinationAddImage(destination, image, nil)
    if CGImageDestinationFinalize(destination) { snapshot.png = png as Data }
  }
}

final class DawnAudioRing: @unchecked Sendable {
  let pointer: OpaquePointer
  init?() {
    guard let pointer = tsugumi_view_audio_ring_create() else { return nil }
    self.pointer = pointer
  }
  deinit { tsugumi_view_audio_ring_destroy(pointer) }
}

@MainActor final class DawnAudio {
  let ring: DawnAudioRing
  private let engine = AVAudioEngine()

  init?() throws {
    guard engine.outputNode.outputFormat(forBus: 0).channelCount > 0,
          let ring = DawnAudioRing(),
          let format = AVAudioFormat(standardFormatWithSampleRate: 48_000, channels: 2) else { return nil }
    self.ring = ring
    let source = AVAudioSourceNode(format: format) { _, timestamp, frames, buffers in
      let channels = UnsafeMutableAudioBufferListPointer(buffers)
      guard channels.count == 2, let left = channels[0].mData, let right = channels[1].mData else { return -1 }
      let time = timestamp.pointee.mHostTime
      let now = time == 0 ? CACurrentMediaTime() : AVAudioTime.seconds(forHostTime: time)
      tsugumi_view_audio_ring_read(ring.pointer, left.assumingMemoryBound(to: Float.self),
        right.assumingMemoryBound(to: Float.self), frames, now)
      return 0
    }
    engine.attach(source)
    engine.connect(source, to: engine.mainMixerNode, format: format)
    engine.prepare()
    try engine.start()
    tsugumi_view_audio_ring_set_latency(ring.pointer, source.outputPresentationLatency)
  }

  func stop() { engine.stop() }
}
