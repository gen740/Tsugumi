import Foundation
import Observation
import TsugumiAgentClient

/// The latest Agent activity on the canvas. One save is one short, simultaneous
/// cue; newer work supersedes it immediately. Nothing waits behind playback.
@MainActor
@Observable
final class AgentPresence {
  /// Presentation only; the saved project is never changed by these scenes.
  private let animate: @MainActor () -> Bool
  var animateWork: Bool { animate() }

  init(animateWork: @escaping @MainActor () -> Bool = { @MainActor in CorePreferences.shared.value("agent.animateWork", fallback: true) }) {
    animate = animateWork
  }

  struct Wire: Hashable, Sendable {
    let from: CardKey
    let fromPort: String
    let to: CardKey
    let toPort: String
  }

  struct Written: Equatable {
    let card: CardKey
    let reveal: Bool
  }

  enum Beat: Equatable {
    case visit(CardKey)  // reads it
    case compose(CardKey)  // writes a file of it, not saved yet
    case save([Written], [Wire])
    case run(CardKey)  // runs the workflow from this trigger Card

    var card: CardKey {
      switch self {
      case .visit(let k), .compose(let k), .run(let k): k
      case .save(let cards, let wires): cards.last?.card ?? wires.last!.to
      }
    }
  }

  struct Scheduled: Equatable {
    let beat: Beat
    let start: Date
    let received: Date
    var length: Double
    /// The plain line for the bubble (without the thought summary).
    let line: String
    /// The file a compose scene shows (`codes`); nil: none, or it has been cut.
    var code: Int?
    var end: Date { start.addingTimeInterval(length) }
  }

  enum Timing {
    static let visit = 0.24
    static let save = 0.24
    static let run = 0.24
    static let flight = 0.24
    static let codeHold = 0.8
    static let codeStalled = 5.0
    static let settle = save
    /// The bird stays this long after the turn ended, the last part of it
    /// fading away (and the "done" bubble before it).
    static let linger = 8.0
    static let fadeOut = 1.6, fadeIn = 0.5, doneLine = 3.0
  }

  private(set) var beats: [Scheduled] = []
  /// When the bird came onto the canvas (it fades in from there).
  private(set) var appearedAt: Date?
  private(set) var working = false
  private(set) var endedAt: Date?
  /// The latest thought summary (from a small model), with its time.
  private(set) var thought: (text: String, at: Date)?
  /// What the Agent does between scenes (looking things up, checking,
  /// waiting for a run), with its time.
  private(set) var activity: (text: String, at: Date)?
  /// How the last turn ended (the bubble's last words).
  private(set) var outcome: Outcome = .done

  enum Outcome: Equatable {
    case done, failed, stopped

    var line: String {
      switch self {
      case .done: String(localized: "できました")
      case .failed: String(localized: "うまくいきませんでした")
      case .stopped: String(localized: "止めました")
      }
    }
  }
  /// A file as the model writes it: only what has arrived.
  struct LiveCode: Equatable {
    let card: CardKey
    let file: String
    var text: String
    /// The save has gone through while it still plays.
    var saved = false
    var patch = false

  }

  /// The files of the compose scenes (apart from `beats`: they change many
  /// times a second, and only the panel looks at them).
  private var codes: [Int: LiveCode] = [:]
  private var nextCode = 0
  /// The file arriving now, by its path; its scene's code (nil: not shown).
  private var liveSequence: Int?
  private var liveID: Int?
  private var liveScanner = SourceWritingScanner()
  private var liveIndex: Int?

  /// Bumped when an effect ends, so views that animate it can stop.
  private(set) var revision = 0
  private var sweeper: Task<Void, Never>?
  /// A helper's name (shown under its bird; nil: the main Agent) and its
  /// colour (0: the main Agent's, then one per helper).
  var label: String?
  var tint = 0
  /// Display name of a Card (for the plain lines).
  var name: (CardKey) -> String = { path($0).last ?? $0 }

  // MARK: - Intake

  func turnStarted(now: Date = Date()) {
    working = true
    endedAt = nil
    thought = nil
    activity = nil
    outcome = .done
    beats.removeAll()
    codes.removeAll()
  }

  func turnEnded(_ outcome: Outcome = .done, now: Date = Date()) {
    self.outcome = outcome
    working = false
    toolInputEnded(ok: outcome == .done, now: now)
    if outcome != .done { finishStep(now: now) }
    endedAt = now
    sweep()
  }

  /// A thought summary; one that comes after the turn ended is dropped.
  func think(_ text: String, now: Date = Date()) {
    guard working else { return }
    thought = (text, now)
  }

  /// What the Agent started doing (nil: nothing to say beyond the scenes).
  func doing(_ text: String?, now: Date = Date()) {
    guard working, let text else { return }
    // The same thing still going on (text streams in many pieces) keeps its time.
    if let a = activity, a.text == text, a.at >= (current(at: now)?.beat.start ?? .distantPast) { return }
    finishStep(now: now)
    activity = (text, now)
  }

  /// A piece of a tool call's arguments as the model writes them. Each file is a
  /// scene of its own, showing what has arrived.
  func toolInput(index: Int, tool: String, delta: String, now: Date = Date()) {
    guard working, PresenceEvents.writesSources(tool: tool) else { return }
    if liveIndex != index {
      arrived(now: now)
      liveSequence = nil
      liveIndex = index
      liveScanner = SourceWritingScanner()
    }
    liveScanner.feed(delta)
    guard let w = liveScanner.current, w.ready, let path = w.path, let (id, file) = PresenceEvents.fileOfCard(path) else {
      if liveScanner.current != nil { arrived(now: now) }
      return
    }
    if liveSequence != w.sequence {
      arrived(now: now)
      liveSequence = w.sequence
      startCode(LiveCode(card: key([id]), file: file, text: w.text, patch: w.patch), now: now)
    } else if let liveID {
      codes[liveID]?.text = w.text
      // It goes on arriving: its scene goes on.
      if let i = beats.indices.last, beats[i].code == liveID, beats[i].end < now.addingTimeInterval(Timing.codeStalled / 2) {
        beats[i].length = now.timeIntervalSince(beats[i].start) + Timing.codeStalled
      }
    } else if !w.done {
      // The previous file ended; show what is arriving now.
      startCode(LiveCode(card: key([id]), file: file, text: w.text, patch: w.patch), now: now)
    }
    if w.done { arrived(now: now) }
  }

  private func startCode(_ code: LiveCode, now: Date) {
    enqueue(.compose(code.card), now: now)
    guard let i = beats.indices.last, beats[i].beat == .compose(code.card), beats[i].code == nil else { return }
    let id = nextCode
    nextCode += 1
    codes[id] = code
    beats[i].code = id
    liveID = id
    codes = codes.filter { c in beats.contains { $0.code == c.key } }
  }

  /// The file has all arrived: its scene holds its end.
  private func arrived(now: Date) {
    defer { liveID = nil }
    guard let liveID, let i = beats.indices.last, beats[i].code == liveID else { return }
    beats[i].length = max(0, now.timeIntervalSince(beats[i].start)) + Timing.codeHold
  }

  /// The call ended, was asked again or was given up. What a call that went
  /// through wrote holds its last lines; `ok` false: it is not on the screen any
  /// more (a save that went through is written out by its own scenes).
  func toolInputEnded(ok: Bool = true, now: Date = Date()) {
    arrived(now: now)
    liveSequence = nil
    liveIndex = nil
    if !ok { stopShowingCode(now: now) }
  }

  /// No file is on the screen from `now` on: a call failed, or the workflow runs.
  func stopShowingCode(now: Date = Date()) {
    if beats.last?.code != nil { finishStep(now: now) }
  }

  /// The file to show at `now`, as far as it has been written out, never past
  /// what has arrived. nil when no file's scene is playing.
  func liveCode(at now: Date) -> LiveCode? {
    guard let cur = current(at: now)?.beat, cur.end > now, let id = cur.code, let code = codes[id] else { return nil }
    return code
  }

  func running(now: Date = Date()) {
    finishStep(now: now)
    thought = nil
    activity = (String(localized: "実行しています"), now)
  }

  /// Complete the old cue at once. Retain just one predecessor for flight
  /// geometry, never a playback queue or old saved-state effects.
  func finishStep(now: Date = Date()) {
    if let i = beats.indices.last, beats[i].end > now {
      beats[i].length = max(0, now.timeIntervalSince(beats[i].start))
    }
    codes.removeAll()
    liveID = nil
    revision += 1
  }

  func enqueue(_ beat: Beat, now: Date = Date()) {
    finishStep(now: now)
    if !visible(at: now) || appearedAt == nil { appearedAt = now }
    beats = Array(beats.suffix(1))
    beats.append(Scheduled(beat: beat, start: now, received: now, length: length(of: beat), line: line(for: beat)))
    sweep()
  }

  /// Every change of a landed save shares one clock and one bird position.
  func saved(written: [(CardKey, reveal: Bool)], wires: [Wire], now: Date = Date()) {
    guard !written.isEmpty || !wires.isEmpty else { return }
    var code = liveCode(at: now)
    code?.saved = true
    enqueue(.save(written.map { Written(card: $0.0, reveal: $0.reveal) }, wires), now: now)
    if let code, let i = beats.indices.last {
      let id = nextCode
      nextCode += 1
      codes[id] = code
      beats[i].code = id
    }
  }

  // MARK: - Reading the state at a time

  /// The bird is on the canvas: during a turn, while scenes still play, and
  /// a little while after.
  func visible(at now: Date) -> Bool {
    if working { return true }
    if let last = beats.last, (animateWork ? last.end : last.received.addingTimeInterval(last.length)) > now { return true }
    if let e = endedAt { return now.timeIntervalSince(e) < Timing.linger }
    return false
  }

  /// How present the bird is (0…1): it fades in when it comes and fades out
  /// at the end of its stay; `lift` is how far it rises as it goes (0…1).
  func fade(at now: Date) -> (opacity: Double, lift: Double) {
    let fadeIn = appearedAt.map { AgentPresence.rise(now.timeIntervalSince($0) / Timing.fadeIn) } ?? 1
    guard !working, let e = endedAt else { return (fadeIn, 0) }
    let left = e.addingTimeInterval(Timing.linger).timeIntervalSince(now)
    let out = AgentPresence.rise(left / Timing.fadeOut)
    return (min(fadeIn, out), 1 - out)
  }

  /// The scene playing at `now` (the last one that started), and the one before.
  func current(at now: Date) -> (beat: Scheduled, previous: Scheduled?)? {
    if !animateWork, let last = beats.last, last.code == nil {
      return (Scheduled(beat: .visit(last.beat.card), start: last.received, received: last.received, length: last.length, line: last.line), nil)
    }
    guard let i = beats.lastIndex(where: { $0.start <= now }) else { return nil }
    return (beats[i], i > 0 ? beats[i - 1] : nil)
  }

  /// What the bubble says at `now`: a fresh thought summary if there is one,
  /// otherwise the line of the scene playing, otherwise what the Agent does
  /// now (a scene that has ended is not said on: the bird would be reading
  /// a Card while the Agent looks something else up).
  func line(at now: Date) -> (text: String, since: Date)? {
    let scene = current(at: now)
    if !working, let e = endedAt {
      return now.timeIntervalSince(e) < Timing.doneLine ? (outcome.line, e) : nil
    }
    // A scene that started after the thought is newer than it (the bird
    // writes rgb while the thought still says it checks bands).
    if working, let t = thought, now.timeIntervalSince(t.at) < 8, t.at >= (scene?.beat.start ?? .distantPast) {
      return (t.text, t.at)
    }
    if let s = scene, s.beat.end > now { return (s.beat.line, s.beat.start) }
    if working {
      if let a = activity, a.at >= (scene?.beat.start ?? .distantPast) { return (a.text, a.at) }
      if let s = scene { return (s.beat.line, s.beat.start) }
      return (String(localized: "考えています"), appearedAt ?? now)
    }
    // Ended: said for a moment, then the bubble goes before the bird.
    guard let e = endedAt else { return nil }
    let doneAt = e
    return now.timeIntervalSince(doneAt) < Timing.doneLine ? (outcome.line, doneAt) : nil
  }

  /// All changed Cards and new wires of the latest save share this short cue.
  func writing(_ k: CardKey, at now: Date) -> (reveal: Double?, settle: Double)? {
    guard animateWork, let s = beats.last, s.start <= now, now < s.end,
          case .save(let cards, _) = s.beat, let card = cards.first(where: { $0.card == k }) else { return nil }
    let u = Self.ease(now.timeIntervalSince(s.start) / Timing.save)
    return (card.reveal ? u : nil, sin(.pi * u))
  }

  enum ThreadState: Equatable {
    case drawing(progress: Double, sag: Double)
    case hidden
    case plain
  }

  func thread(_ w: Wire, at now: Date) -> ThreadState {
    guard animateWork, let s = beats.last, s.start <= now, now < s.end,
          case .save(_, let wires) = s.beat, wires.contains(w) else { return .plain }
    return .drawing(progress: Self.ease(now.timeIntervalSince(s.start) / Timing.save), sag: 0)
  }

  /// Something is moving: views animate frame by frame only then.
  func animating(at now: Date) -> Bool {
    let _ = animateWork // Observe setting changes even while only wires are staged.
    return visible(at: now) || beats.contains { $0.end.addingTimeInterval(Timing.settle) > now }
  }

  // MARK: - Helpers

  nonisolated static func ease(_ x: Double) -> Double {
    let x = min(1, max(0, x))
    return x < 0.5 ? 2 * x * x : 1 - pow(-2 * x + 2, 2) / 2
  }

  /// Starts and stops without a jolt.
  nonisolated static func rise(_ x: Double) -> Double {
    let x = min(1, max(0, x))
    return x * x * x * (x * (6 * x - 15) + 10)
  }

  private func length(of beat: Beat) -> Double {
    switch beat {
    case .visit: Timing.visit
    case .compose: Timing.codeStalled
    case .save: Timing.save
    case .run: Timing.run
    }
  }

  private func line(for beat: Beat) -> String {
    switch beat {
    case .visit(let k): String(localized: "\(name(k)) を読んでいます")
    case .compose(let k): String(localized: "\(name(k)) を書いています")
    case .save: String(localized: "\(name(beat.card)) を直しています")
    case .run(let k): String(localized: "\(name(k)) から実行しています")
    }
  }

  /// Bumps `revision` as effects end, so animating views can pause.
  private func sweep() {
    sweeper?.cancel()
    sweeper = Task { [weak self] in
      while !Task.isCancelled {
        try? await Task.sleep(for: .milliseconds(500))
        guard let self else { return }
        let now = Date()
        self.revision += 1
        if !self.animating(at: now) { return }
      }
    }
  }
}
