import Foundation
import Observation
import TsugumiAgent

/// What the Agent is doing, for the canvas: a queue of short scenes (beats)
/// that the tsugumi plays one after another. It looks into a Card the Agent
/// reads, writes out a Card the Agent saved (a Draft becoming code is written
/// left to right), threads the wires of what it wrote, and flies to the
/// trigger Card when the Agent runs the workflow. Beats never overlap: one
/// that comes while others still play waits for them.
@MainActor
@Observable
final class AgentPresence {
  struct Wire: Hashable, Sendable {
    let from: CardKey
    let fromPort: String
    let to: CardKey
    let toPort: String
  }

  enum Beat: Equatable {
    case visit(CardKey)  // reads it
    case compose(CardKey)  // writes a file of it, not saved yet
    case write(CardKey, reveal: Bool)  // saved it; `reveal`: a Draft (or a new Card) became code
    case thread(Wire)  // threads a wire of what it wrote
    case run(CardKey)  // runs the workflow from this trigger Card

    var card: CardKey {
      switch self {
      case .visit(let k), .compose(let k), .write(let k, _), .run(let k): k
      case .thread(let w): w.to
      }
    }
  }

  struct Scheduled: Equatable {
    let beat: Beat
    let start: Date
    var length: Double
    /// The plain line for the bubble (without the thought summary).
    let line: String
    /// The file a compose scene shows (`codes`); nil: none, or it has been cut.
    var code: Int?
    var end: Date { start.addingTimeInterval(length) }
  }

  /// Lengths of the scenes (seconds): unhurried, so the bird glides.
  enum Timing {
    static let visit = 1.6
    static let reveal = 1.7  // approach 0.4, written over 1.1, a moment after
    static let revealStart = 0.4, revealLength = 1.1
    static let update = 1.3
    static let thread = 1.5
    static let threadStart = 0.35, threadLength = 1.1
    static let run = 1.4
    /// The API sends a file in lumps of about 200 characters, about every 0.43 s
    /// (measured with claude-sonnet-5-5), which at once would replace the panel's lines
    /// each time. What arrives is written out over this long, up to the next lump.
    static let codeEase = 0.45
    /// A short file is seen this long at the least.
    static let codeShortest = 1.5
    /// Its last lines stay this long when no other file comes after it.
    static let codeHold = 0.8
    /// A pause in what arrives may last this long before its scene ends by itself.
    static let codeStalled = 5.0
    /// How long the colour of a written Card takes to settle and fade.
    static let settle = 4.0
    /// Beats that would start further ahead than this are dropped if they
    /// only look (visits): the bird does not lag far behind the Agent.
    static let maxBacklog = 5.0
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
    case done, failed, stopped, limit

    var line: String {
      switch self {
      case .done: String(localized: "できました")
      case .failed: String(localized: "うまくいきませんでした")
      case .stopped: String(localized: "止めました")
      case .limit: String(localized: "途中で止まっています")
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
    /// How far it was shown when more of it last arrived, and when.
    var shownFrom = 0
    var grewAt = Date.distantPast

    /// How many characters are shown at `now`.
    func shown(at now: Date) -> Int {
      let all = text.unicodeScalars.count
      let u = now.timeIntervalSince(grewAt) / Timing.codeEase
      return u >= 1 ? all : min(all, shownFrom + Int(Double(all - shownFrom) * max(0, u)))
    }

    mutating func grow(to text: String, at now: Date) {
      shownFrom = shown(at: now)
      grewAt = now
      self.text = text
    }
  }

  /// The files of the compose scenes (apart from `beats`: they change many
  /// times a second, and only the panel looks at them).
  private var codes: [Int: LiveCode] = [:]
  private var nextCode = 0
  /// The file arriving now, by its path; its scene's code (nil: not shown).
  private var livePath: String?
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
    beats.removeAll { $0.end < now }
  }

  func turnEnded(_ outcome: Outcome = .done, now: Date = Date()) {
    self.outcome = outcome
    working = false
    toolInputEnded(ok: outcome == .done, now: now)
    endedAt = max(now, beats.last?.end ?? now)
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
    activity = (text, now)
  }

  /// A piece of a tool call's arguments as the model writes them. Each file is a
  /// scene of its own, showing what has arrived.
  func toolInput(index: Int, tool: String, delta: String, now: Date = Date()) {
    guard working, PresenceEvents.writesSources(tool: tool) else { return }
    if liveIndex != index {
      arrived(now: now)
      livePath = nil
      liveIndex = index
      liveScanner = SourceWritingScanner()
    }
    liveScanner.feed(delta)
    guard let w = liveScanner.current, let path = w.path, let (id, file) = PresenceEvents.fileOfCard(path) else {
      if liveScanner.current != nil { arrived(now: now) }
      return
    }
    if livePath != path {
      arrived(now: now)
      livePath = path
      startCode(LiveCode(card: key([id]), file: file, text: w.text, grewAt: now), now: now)
    } else if let liveID {
      codes[liveID]?.grow(to: w.text, at: now)
      // It goes on arriving: its scene goes on.
      if let i = beats.indices.last, beats[i].code == liveID, beats[i].end < now.addingTimeInterval(Timing.codeStalled / 2) {
        beats[i].length = now.timeIntervalSince(beats[i].start) + Timing.codeStalled
      }
    } else if !w.done {
      // It waited behind the bird's other scenes: it may have room now.
      startCode(LiveCode(card: key([id]), file: file, text: w.text, grewAt: now), now: now)
    }
    if w.done { arrived(now: now) }
  }

  private func startCode(_ code: LiveCode, now: Date) {
    // The file being written goes first: one that has all arrived and still
    // waits is not shown, and the one on the screen gives up its hold. So the
    // window is never further behind than `Timing.codeShortest`.
    beats.removeAll { $0.code != nil && $0.start > now }
    if let i = beats.indices.last, beats[i].code != nil {
      beats[i].length = max(now.timeIntervalSince(beats[i].start), Timing.codeShortest)
    }
    enqueue(.compose(code.card), now: now)
    // Not there (too far behind): not shown. Not by the count: enqueue also drops old scenes.
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
    beats[i].length = max(now.timeIntervalSince(beats[i].start), Timing.codeShortest) + Timing.codeHold
  }

  /// The call ended, was asked again or was given up. What a call that went
  /// through wrote holds its last lines; `ok` false: it is not on the screen any
  /// more (a save that went through is written out by its own scenes).
  func toolInputEnded(ok: Bool = true, now: Date = Date()) {
    arrived(now: now)
    livePath = nil
    liveIndex = nil
    if !ok { stopShowingCode(now: now) }
  }

  /// No file is on the screen from `now` on: a call failed, or the workflow runs.
  func stopShowingCode(now: Date = Date()) {
    beats.removeAll { $0.code != nil && $0.start > now }
    if let i = beats.lastIndex(where: { $0.code != nil && $0.start <= now && $0.end > now }) {
      beats[i].length = now.timeIntervalSince(beats[i].start)
    }
    codes.removeAll()
  }

  /// The file to show at `now`, as far as it has been written out, never past
  /// what has arrived. nil when no file's scene is playing.
  func liveCode(at now: Date) -> LiveCode? {
    guard let cur = current(at: now)?.beat, cur.end > now, let id = cur.code, var code = codes[id] else { return nil }
    code.text = String(String.UnicodeScalarView(code.text.unicodeScalars.prefix(code.shown(at: now))))
    return code
  }

  func enqueue(_ beat: Beat, now: Date = Date()) {
    let start = max(now, beats.last?.end ?? now)
    if !visible(at: start) || appearedAt == nil { appearedAt = start }
    switch beat {
    case .visit(let k), .compose(let k):
      // Already there, or too far behind to be worth showing.
      if case .visit = beat, let last = beats.last, last.beat.card == k, last.end > now.addingTimeInterval(-0.5) { return }
      if start.timeIntervalSince(now) > Timing.maxBacklog { return }
    default: break
    }
    beats.append(Scheduled(beat: beat, start: start, length: length(of: beat), line: line(for: beat)))
    // Played and gone beats are kept only as long as their effects last.
    beats.removeAll { $0.end.addingTimeInterval(Timing.settle + 1) < now }
    sweep()
  }

  /// A save by the Agent: the Cards it wrote (in flow order) and the wires
  /// to thread after them.
  func saved(written: [(CardKey, reveal: Bool)], wires: [Wire], now: Date = Date()) {
    // The save has gone through: a file that still waits is not shown, and the
    // one on the screen says so for what is left of its hold.
    beats.removeAll { $0.code != nil && $0.start > now }
    for (id, code) in codes where written.contains(where: { $0.0 == code.card }) { codes[id]?.saved = true }
    for (k, reveal) in written { enqueue(.write(k, reveal: reveal), now: now) }
    for w in wires { enqueue(.thread(w), now: now) }
  }

  // MARK: - Reading the state at a time

  /// The bird is on the canvas: during a turn, while scenes still play, and
  /// a little while after.
  func visible(at now: Date) -> Bool {
    if working { return true }
    if let last = beats.last, last.end > now { return true }
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
    guard let i = beats.lastIndex(where: { $0.start <= now }) else { return nil }
    return (beats[i], i > 0 ? beats[i - 1] : nil)
  }

  /// What the bubble says at `now`: a fresh thought summary if there is one,
  /// otherwise the line of the scene playing, otherwise what the Agent does
  /// now (a scene that has ended is not said on: the bird would be reading
  /// a Card while the Agent looks something else up).
  func line(at now: Date) -> (text: String, since: Date)? {
    let scene = current(at: now)
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
    let doneAt = max(scene?.beat.end ?? e, e)
    return now.timeIntervalSince(doneAt) < Timing.doneLine ? (outcome.line, doneAt) : nil
  }

  /// How far a written Card is (reveal 0…1 while it is written out; nil when
  /// it is not being written) and how strongly its colour settles (0…1).
  func writing(_ k: CardKey, at now: Date) -> (reveal: Double?, settle: Double)? {
    guard let s = beats.last(where: { if case .write(let c, _) = $0.beat { return c == k } else { return false } }) else { return nil }
    guard case .write(_, let revealing) = s.beat else { return nil }
    let t = now.timeIntervalSince(s.start)
    guard t >= 0 else { return revealing ? (0, 0) : nil }  // queued: the new face waits hidden
    let settleFrom = revealing ? Timing.revealStart + Timing.revealLength - 0.3 : 0.4
    let settleT = (t - settleFrom) / Timing.settle
    let settle = settleT <= 0 ? 0 : settleT >= 1 ? 0 : AgentPresence.rise(settleT / 0.35) * (1 - AgentPresence.rise((settleT - 0.45) / 0.55))
    let reveal: Double? = revealing && t < Timing.revealStart + Timing.revealLength
      ? AgentPresence.ease((t - Timing.revealStart) / Timing.revealLength) : nil
    if reveal == nil && settleT >= 1 { return nil }
    return (reveal, settle)
  }

  enum ThreadState: Equatable {
    case drawing(progress: Double, sag: Double)
    case hidden  // waits for its scene
    case plain
  }

  /// Where a wire is in its threading at `now`.
  func thread(_ w: Wire, at now: Date) -> ThreadState {
    guard let s = beats.last(where: { $0.beat == .thread(w) }) else { return .plain }
    let t = now.timeIntervalSince(s.start)
    if t < Timing.threadStart { return .hidden }
    let u = (t - Timing.threadStart) / Timing.threadLength
    if u >= 1.6 { return .plain }
    // Hanging while it is drawn, pulled taut as it arrives (a small bounce).
    let x = u / 1.5
    let sag = x < 0.6 ? 1 : exp(-5 * (x - 0.6)) * cos(8 * (x - 0.6))
    return .drawing(progress: AgentPresence.ease(min(1, u)), sag: sag)
  }

  /// Something is moving: views animate frame by frame only then.
  func animating(at now: Date) -> Bool {
    visible(at: now) || beats.contains { $0.end.addingTimeInterval(Timing.settle) > now }
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
    case .write(_, let reveal): reveal ? Timing.reveal : Timing.update
    case .thread: Timing.thread
    case .run: Timing.run
    }
  }

  private func line(for beat: Beat) -> String {
    switch beat {
    case .visit(let k): String(localized: "\(name(k)) を読んでいます")
    case .compose(let k): String(localized: "\(name(k)) を書いています")
    case .write(let k, let reveal): reveal ? String(localized: "\(name(k)) を書いています") : String(localized: "\(name(k)) を直しています")
    case .thread(let w): String(localized: "\(name(w.from)) から \(name(w.to)) へつないでいます")
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
