import Foundation
import AppKit
import CoreGraphics
import Observation
import SwiftUI
import Testing
import TsugumiTestSupport
@testable import TsugumiLauncher
@testable import TsugumiApp
@testable import TsugumiRPC

@MainActor
@Test func mathRenderingUsesThePackagedFont() throws {
  let resources = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    .appendingPathComponent("../../Resources")
  let fontURL = resources.appendingPathComponent("Fonts/latin-modern-math/latinmodern-math.otf")
  let provider = try #require(CGDataProvider(url: fontURL as CFURL))
  let bundledFont = try #require(CGFont(provider))
  let bundledName = try #require(bundledFont.postScriptName as String?)

  let rendered = try #require(MathImages.image("x + y = 2", size: 18, display: true))
  let data = NSMutableData()
  let consumer = try #require(CGDataConsumer(data: data as CFMutableData))
  var bounds = CGRect(origin: .zero, size: rendered.image.size)
  let context = try #require(CGContext(consumer: consumer, mediaBox: &bounds, nil))
  context.beginPDFPage(nil)
  NSGraphicsContext.saveGraphicsState()
  NSGraphicsContext.current = NSGraphicsContext(cgContext: context, flipped: false)
  rendered.image.draw(in: bounds)
  NSGraphicsContext.restoreGraphicsState()
  context.endPDFPage()
  context.closePDF()

  let pdfData = try #require(CGDataProvider(data: data as CFData))
  let pdf = try #require(CGPDFDocument(pdfData))
  let page = try #require(pdf.page(at: 1)?.dictionary)
  func dictionary(_ parent: CGPDFDictionaryRef, _ key: String) -> CGPDFDictionaryRef? {
    var child: CGPDFDictionaryRef?
    return CGPDFDictionaryGetDictionary(parent, key, &child) ? child : nil
  }
  let pageResources = try #require(dictionary(page, "Resources"))
  var names = Set<String>()
  func readFonts(_ resources: CGPDFDictionaryRef) {
    if let fonts = dictionary(resources, "Font") {
      CGPDFDictionaryApplyBlock(fonts, { _, value, _ in
        var font: CGPDFDictionaryRef?
        var baseFont: UnsafePointer<CChar>?
        if CGPDFObjectGetValue(value, .dictionary, &font), let font,
           CGPDFDictionaryGetName(font, "BaseFont", &baseFont), let baseFont {
          // PDF font subsets may add a prefix to the PostScript name.
          let name = String(cString: baseFont)
          names.insert(String(name.split(separator: "+").last ?? Substring(name)))
        }
        return true
      }, nil)
    }
    if let objects = dictionary(resources, "XObject") {
      CGPDFDictionaryApplyBlock(objects, { _, value, _ in
        var stream: CGPDFStreamRef?
        if CGPDFObjectGetValue(value, .stream, &stream), let stream,
           let form = CGPDFStreamGetDictionary(stream), let nested = dictionary(form, "Resources") {
          readFonts(nested)
        }
        return true
      }, nil)
    }
  }
  readFonts(pageResources)
  #expect(names == [bundledName])
}

@Test func nixPathShowsTheLockForTheDeclaredURL() throws {
  let url = "github:owner/repo"
  let lock = try JSONValue.parse(#"{"entries":[{"name":"science","url":"github:owner/repo","rev":"abcdef1234567890abcdef1234567890abcdef12","narHash":"sha256-example"}]}"#)
  #expect(NixPathLockStatus.read(nil, name: "science", url: url) == .unlocked)
  #expect(NixPathLockStatus.read(lock, name: "other", url: url) == .unlocked)
  #expect(NixPathLockStatus.read(lock, name: "science", url: "github:owner/other") == .urlChanged)
  #expect(NixPathLockStatus.read(lock, name: "science", url: url + "/other") == .urlChanged)
  #expect(NixPathLockStatus.read(lock, name: "science", url: url) ==
          .locked(pin: "abcdef1"))
}

@Test func sdkBuildShowsOnlyTheApiDirectoryAndPreservesFailureLog() {
  let result = Tsugumi_RPC_EvaluateNixResultT()
  result.failure = nil
  func file(_ path: String) -> Tsugumi_RPC_NixOutputFileT {
    let file = Tsugumi_RPC_NixOutputFileT()
    file.path = path
    file.content = Array("api".utf8)
    return file
  }
  result.files = [file("share/tsugumi/api/README.md"), file("share/tsugumi/api-extra/x"), file("bin/tool")]
  result.truncated = true
  let sdk = SdkBuild(result, prefix: "sdk:")
  #expect(sdk.files?.map(\.id) == ["sdk:README.md"])
  #expect(sdk.files?.first?.text == "api" && sdk.truncated)
  let failure = Tsugumi_Data_FailureT()
  failure.diagnostic = "Nix failed"
  result.failure = failure
  result.log = "last line"
  let failed = SdkBuild(result, prefix: "sdk:")
  #expect(failed.files == nil && failed.failure == "Nix failed" && failed.log == "last line")
}

@Test func aVersionHoverFitsItsCards() {
  // Every Card stays inside the box, in the order it has on the canvas.
  let wide = VersionMiniMapLayout([.init(id: "a", x: 100, y: 0), .init(id: "b", x: 5000, y: 300), .init(id: "c", x: 2500, y: -400)],
                                  flow: .leftToRight, width: 284, height: 132)
  for (_, c) in wide.cards {
    let r = c.box
    #expect(r.minX >= 0 && r.maxX <= 284 && r.minY >= 0 && r.maxY <= wide.height && wide.height <= 132)
  }
  #expect(wide.cards["a"]!.box.minX < wide.cards["c"]!.box.minX && wide.cards["c"]!.box.minX < wide.cards["b"]!.box.minX)
  #expect(!wide.named)
  // A small graph is not blown up, and is only as tall as it is.
  let small = VersionMiniMapLayout([.init(id: "a", x: 0, y: 0)], flow: .leftToRight, width: 284, height: 132)
  #expect(small.cards["a"]!.box.width == 220 * 0.3 && small.height == small.cards["a"]!.box.height + 16 && small.named)
  #expect(VersionMiniMapLayout([], flow: .leftToRight, width: 284, height: 132).cards.isEmpty)
}

@Test func collectFunnelFollowsTheCanvasFlow() {
  let data = CardNodeData(title: "collect", kind: .collect, inputs: [PortData(name: "in")], outputs: [PortData(name: "out")])
  for flow in [FlowDirection.topToBottom, .leftToRight] {
    let geometry = CardGeometry(data, flow: flow)
    let input = geometry.port("in", output: false)
    let output = geometry.port("out", output: true)
    #expect(geometry.shape == .collect)
    if flow == .topToBottom {
      #expect(input.x == output.x && input.y < output.y)
    } else {
      #expect(input.y == output.y && input.x < output.x)
    }
  }
}

@Test func aVersionsMinimapDrawsWhatIsNotACardWithItsCanvasShape() {
  func canvas(_ kind: CardKind, fan: Int = 0, flow: FlowDirection = .leftToRight) -> CardGeometry {
    CardGeometry(VersionMiniMapLayout.Card(id: "c", x: 0, y: 0, kind: kind, fan: fan).data, flow: flow)
  }
  func outline(_ g: CardGeometry, in box: CGRect, _ k: CGFloat) -> Path {
    let o = g.outlineRect
    return g.outline(in: CGRect(x: box.minX + o.minX * k, y: box.minY + o.minY * k, width: o.width * k, height: o.height * k), cornerScale: k)
  }
  // A graph laid out on the canvas does not overlap here: start and two Cards feeding a Merge.
  let start = canvas(.trigger), merge = canvas(.merge, fan: 2)
  let placed = VersionMiniMapLayout([
    .init(id: "start", x: 0, y: 30, kind: .trigger),
    .init(id: "merge", x: start.size.width + 10, y: 20, kind: .merge, fan: 2),
    .init(id: "load", x: start.size.width + merge.size.width + 40, y: -30),
    .init(id: "draft", x: start.size.width + merge.size.width + 40, y: 60, kind: .draft),
  ], flow: .leftToRight, width: 284, height: 132)
  let at = placed.cards
  #expect(at["start"]!.shape == .start && at["merge"]!.shape == .merge && at["load"]!.shape == .card && at["draft"]!.shape == .card)
  #expect(at["draft"]!.draft && !at["load"]!.draft)
  let boxes = Array(at.values.map(\.box))
  for (i, a) in boxes.enumerated() {
    for b in boxes[(i + 1)...] { #expect(!a.intersects(b)) }
  }
  // The shapes are those of the canvas, scaled and at their place.
  let k = at["merge"]!.box.width / merge.size.width
  #expect(k > 0 && k <= 0.3)
  #expect(abs(at["merge"]!.box.height - merge.size.height * k) < 1e-9 && abs(at["start"]!.box.width - start.size.width * k) < 1e-9)
  #expect(at["merge"]!.outline == outline(merge, in: at["merge"]!.box, k))
  #expect(at["start"]!.outline == outline(start, in: at["start"]!.box, k))
  #expect(abs(at["merge"]!.outline.boundingRect.minX - (at["merge"]!.box.minX + merge.outlineRect.minX * k)) < 1)

  func single(_ kind: CardKind, _ fan: Int, _ flow: FlowDirection = .leftToRight) -> VersionMiniMapLayout.Placed {
    VersionMiniMapLayout([.init(id: "m", x: 0, y: 0, kind: kind, fan: fan)], flow: flow, width: 284, height: 132).cards["m"]!
  }
  #expect(single(.merge, 2).shape == .merge)
  #expect(single(.merge, 6).box.height > single(.merge, 2).box.height)
  #expect(single(.merge, 6, .topToBottom).box.width > single(.merge, 2, .topToBottom).box.width)
  #expect(single(.merge, 2).outline != single(.merge, 2, .topToBottom).outline)

  // A loop is a merge and a Collect: they keep their shapes and do not overlap.
  let loop = VersionMiniMapLayout([.init(id: "m", x: 0, y: 0, kind: .merge, fan: 2), .init(id: "step", x: 140, y: 0), .init(id: "b", x: 400, y: 0, kind: .collect)],
                                  flow: .leftToRight, width: 284, height: 132).cards
  #expect(loop["m"]!.shape == .merge && loop["step"]!.shape == .card && loop["b"]!.shape == .collect)
  #expect(!loop["m"]!.box.intersects(loop["step"]!.box) && !loop["step"]!.box.intersects(loop["b"]!.box))

  // A folded Card is its header.
  let folded = VersionMiniMapLayout([.init(id: "a", x: 0, y: 0, collapsed: true)], flow: .leftToRight, width: 284, height: 132).cards["a"]!
  #expect(abs(folded.box.height - CardGeometry.header * 0.3) < 1e-9)
}

@Test func stagesOfCodeAndBuiltinCards() {
  // Shown as build → run → validate (SDK and Nix inside build, saving inside validate).
  #expect(Stage.applicable(code: true) == [.build, .execute, .validate])
  #expect(Stage.applicable(code: false) == [.execute, .validate])
  #expect(Stage.build.parts == [.prepare, .evaluate, .build] && Stage.validate.parts == [.validate, .save])
  #expect(StageProgress.merge([.cached, .cached, .cached], ended: true) == .cached)
  #expect(StageProgress.merge([.done, .pending, .pending], ended: false) == .running)
  #expect(StageProgress.merge([.done, .failed, .notRun], ended: true) == .failed)
  #expect(StageProgress.merge([.reused, .reused, .reused], ended: true) == .reused)
  #expect(Stage(phase: .evaluate) == .evaluate && Stage(phase: .save) == .save && Stage(phase: .unspecified) == nil)
  #expect(Stage(phase: .prepare) == .prepare && Stage(phase: .execute) == .execute)
  // The visible stages remain distinguishable as progress moves between them.
  #expect(Set(Stage.allCases.map(\.symbol)).count == Stage.allCases.count)
  #expect(Set(Stage.allCases.map(\.activeSymbol)).count == Stage.allCases.count)
  #expect(Stage.build.logStream == .build && Stage.execute.logStream == .output && Stage.save.logStream == .journal)
}

@Test func remoteWaitingIsNotRunning() {
  var state = CardRunState(state: .running, invocation: 1, phase: .execute)
  #expect(ExecutionActivityText(state) == nil)
  state.executionActivity = .waitingStore
  #expect(ExecutionActivityText(state)?.waiting == true)
  let running: Tsugumi_Data_StageOutcome = .running
  #expect(SectionStatus(StagePipeline.status(running), waiting: true) == .waiting && SectionStatus(StagePipeline.status(running)) == .running)
  #expect(SectionStatus(StageProgress.Status.notRun) == .pending && SectionStatus(StageProgress.Status.cached) == .cached)
  state.executionActivity = .building
  state.executionActivityDetail = "nupipeline-1.0"
  let building = ExecutionActivityText(state)
  #expect(building?.waiting == false && building?.short.contains("nupipeline-1.0") == true)
  state.executionActivity = .running
  #expect(ExecutionActivityText(state) == nil)
  state.executionActivity = .unspecified
  state.executionState = .queued
  #expect(ExecutionActivityText(state)?.waiting == true)
  state.state = .succeeded
  #expect(ExecutionActivityText(state) == nil)
}


/// The log view's pipeline line draws the Card's stage circles and says in a
/// second line what the running stage does.
@Test func thePipelineLineDrawsEachStageAsTheCardDoes() throws {
  let order: [Stage] = [.prepare, .evaluate, .build, .execute, .validate, .save]
  func record(_ stage: Stage, _ outcome: Tsugumi_Data_StageOutcome, _ start: Int64, _ end: Int64? = nil) -> StageRecord {
    StageRecord(stage: stage, outcome: outcome, start: start, end: end)
  }
  func step(_ line: StagePipeline, _ stage: Stage) throws -> StagePipeline.Step { try #require(line.steps.first { $0.stage == stage }) }

  // Not run: every circle is an empty ring, with no time and no second line.
  let none = StagePipeline(stages: order, records: [], service: nil, live: nil, now: 0)
  #expect(none.steps.map(\.stage) == order)
  #expect(none.steps.allSatisfy { $0.status == .pending && $0.seconds == nil && !$0.waiting } && none.detail == nil)

  // Running on a worker that builds the Card: the build circle runs (the
  // worker builds as part of execution), execution has not started yet.
  let records = [record(.prepare, .done, 0, 41_910), record(.evaluate, .done, 41_910, 49_000), record(.execute, .running, 49_000)]
  var live = CardRunState(state: .running, invocation: 1, phase: .execute)
  live.executionService = "ttl"
  live.executionActivity = .building
  live.executionActivityDetail = "rcw86-nustar-events"
  live.cpuMs = 341_230
  live.lastOutputAt = 100_000 - 85_190
  live.stages = records
  let building = StagePipeline(stages: order, records: records, service: "ttl", live: live, now: 100_000)
  #expect(try step(building, .prepare).status == .done && step(building, .prepare).seconds == 41.91)
  #expect(formatShortDuration(try #require(step(building, .prepare).seconds)) == "41.9 s")
  let build = try step(building, .build)
  #expect(build.status == .running && build.onWorker && !build.waiting && build.seconds == 51)
  #expect(try step(building, .execute).status == .pending && step(building, .execute).seconds == nil)
  let detail = try #require(building.detail)
  for part in ["ttl", "rcw86-nustar-events", "CPU 5:41", formatAgo(85.19)] { #expect(detail.contains(part), "\(part) in \(detail)") }
  #expect(!detail.contains("00:0"), "No long times in the pipeline line")
  #expect(formatAgo(85.19) == String(localized: "\(1) 分 \(25) 秒前"))
  #expect(formatAgo(12) == String(localized: "\(12) 秒前") && formatAgo(7500) == String(localized: "\(2) 時間 \(5) 分前"))

  // Waiting for the worker's Nix store: the build circle waits (dashed).
  live.executionActivity = .waitingStore
  let store = StagePipeline(stages: order, records: records, service: "ttl", live: live, now: 100_000)
  #expect(try step(store, .build).status == .running && step(store, .build).waiting)
  // Waiting for a free worker slot: nothing reached the worker; the execution waits.
  live.executionActivity = .unspecified
  live.executionState = .queued
  let queued = StagePipeline(stages: order, records: records, service: "ttl", live: live, now: 100_000)
  #expect(try step(queued, .build).status == .pending && step(queued, .execute).status == .running && step(queued, .execute).waiting)
  // The Card's program runs: the worker's build is behind it.
  live.executionState = .running
  live.executionActivity = .running
  let running = StagePipeline(stages: order, records: records, service: "ttl", live: live, now: 100_000)
  #expect(try step(running, .build).status == .done && step(running, .execute).status == .running && step(running, .execute).seconds == 51)
  #expect(running.detail?.contains("ttl") == true)
  // After the Card ran, the worker uploads its build outputs: the build stays done.
  live.executionActivity = .pushing
  live.executionActivityDetail = "remote-card-plan (2/5 · 1.2 GB / 4.0 GB)"
  let pushing = StagePipeline(stages: order, records: records, service: "ttl", live: live, now: 100_000)
  #expect(try step(pushing, .build).status == .done && step(pushing, .execute).status == .running)
  #expect(pushing.detail?.contains("remote-card-plan (2/5 · 1.2 GB / 4.0 GB)") == true)

  // Failed: the failed stage is red, the later ones never ran, no second line.
  let failedRecords = [record(.prepare, .done, 0, 10), record(.evaluate, .failed, 10, 2_010)]
  let failed = StagePipeline(stages: order, records: failedRecords, service: nil, live: nil, now: 5_000)
  #expect(try step(failed, .evaluate).status == .failed && step(failed, .evaluate).seconds == 2)
  #expect(try step(failed, .build).status == .pending && failed.detail == nil)

  // Succeeded with a cached build, on the project's own environment.
  let cachedRecords = [record(.prepare, .done, 0, 100), record(.evaluate, .done, 100, 200), record(.build, .cached, 200, 200),
                       record(.execute, .done, 200, 4_200), record(.validate, .done, 4_200, 4_300), record(.save, .done, 4_300, 4_310)]
  let cached = StagePipeline(stages: order, records: cachedRecords, service: "local", live: nil, now: 9_000)
  #expect(try step(cached, .build).status == .cached && !step(cached, .build).onWorker)
  #expect(try step(cached, .execute).status == .done && step(cached, .execute).seconds == 4)
  #expect(cached.detail == nil)
  // Once it ended, the second line says the CPU time it used, its Nix build's included.
  let finished = StagePipeline(stages: order, records: cachedRecords, service: "local", live: nil, now: 9_000, cpuMs: 341_230)
  #expect(finished.detail == "CPU 5:41")
  // A finished run on a worker: its build is shown done on the worker when it
  // built, cached when it came from a store (the core's record), and cached
  // when the worker reported no build at all.
  let remoteRecords = cachedRecords.filter { $0.stage != .build }
  let remote = StagePipeline(stages: order, records: remoteRecords, service: "ttl", live: nil, now: 9_000,
                             lastActivity: .running, buildCached: false)
  #expect(try step(remote, .build).status == .done && step(remote, .build).onWorker)
  #expect(try step(StagePipeline(stages: order, records: remoteRecords, service: "ttl", live: nil, now: 9_000,
                                 lastActivity: .running, buildCached: true), .build).status == .cached)
  #expect(try step(StagePipeline(stages: order, records: remoteRecords, service: "ttl", live: nil, now: 9_000), .build).status == .cached)
  #expect(try step(StagePipeline(stages: order, records: remoteRecords, service: "ttl", live: nil, now: 9_000,
                                 lastActivity: .building), .build).status == .done)

  // Stopped while the worker still prepared (the Card never started): the
  // build carries the stop and the whole time, the execution did not run.
  let stoppedRecords = [record(.prepare, .done, 0, 100), record(.evaluate, .done, 100, 200), record(.execute, .cancelled, 200, 290_200)]
  for before in WorkerStages.beforeCard {
    let stopped = StagePipeline(stages: order, records: stoppedRecords, service: "ttl", live: nil, now: 300_000, lastActivity: before)
    let b = try step(stopped, .build)
    #expect(b.status == .cancelled && b.seconds == 290 && b.onWorker, "\(before)")
    #expect(try step(stopped, .execute).status == .notRun && step(stopped, .execute).seconds == nil)
  }
  let failedBefore = StagePipeline(stages: order, records: [stoppedRecords[0], stoppedRecords[1], record(.execute, .failed, 200, 1_200)],
                                   service: "ttl", live: nil, now: 2_000, lastActivity: .building)
  #expect(try step(failedBefore, .build).status == .failed && step(failedBefore, .execute).status == .notRun)
  // Stopped once the Card ran (or while it pushed its outputs after): the build is done, the execution stopped.
  for after in [Tsugumi_Data_ExecutionActivity.running, .pushing] {
    let stopped = StagePipeline(stages: order, records: stoppedRecords, service: "ttl", live: nil, now: 300_000, lastActivity: after)
    #expect(try step(stopped, .build).status == .done && step(stopped, .build).seconds == nil)
    #expect(try step(stopped, .execute).status == .cancelled && step(stopped, .execute).seconds == 290)
  }
  // The Card's face agrees with the pipeline line.
  var ended = CardRunState(state: .cancelled, invocation: 1, phase: .execute)
  ended.executionService = "ttl"
  ended.executionLastActivity = .preparing
  ended.stages = [record(.prepare, .done, 0, 10), record(.evaluate, .done, 10, 20), record(.execute, .cancelled, 20, 290_020)]
  let face = StageProgress.of(ended, code: true)
  let faceBuild = try #require(face.items.first { $0.stage == .build })
  let faceRun = try #require(face.items.first { $0.stage == .execute })
  #expect(faceBuild.status == .cancelled && faceBuild.seconds == 290.02)
  #expect(faceRun.status == .notRun && faceRun.seconds == nil && face.failedPart == .build)
  let line = StagePipeline(stages: [.prepare, .evaluate, .build, .execute], records: ended.stages, service: "ttl", live: ended, now: 300_000)
  #expect(line.steps.map(\.status) == [.done, .done, .cancelled, .notRun])
}

/// An open log section whose stream has not loaded yet says nothing (no
/// "empty" wording that flashes before the text arrives); after a moment,
/// that it loads. The Web UI checks the same (stagePipeline.ts).
@Test func aLoadingLogSectionShowsNoEmptyPlaceholder() {
  for section in ["build", "execute", "save"] {
    #expect(StageLogView.sectionPlaceholder(section, known: true, invocation: 1, loaded: false, waiting: nil, slow: false) == "")
    #expect(StageLogView.sectionPlaceholder(section, known: false, invocation: nil, loaded: false, waiting: nil, slow: false) == "")
    #expect(StageLogView.sectionPlaceholder(section, known: true, invocation: 1, loaded: false, waiting: nil, slow: true)
      == String(localized: "読み込んでいます…"))
  }
  #expect(StageLogView.sectionPlaceholder("execute", known: true, invocation: 1, loaded: true, waiting: nil, slow: false)
    == String(localized: "出力はありません"))
  #expect(StageLogView.sectionPlaceholder("execute", known: true, invocation: nil, loaded: false, waiting: nil, slow: false)
    == String(localized: "この実行では、この Card はまだ動いていません"))
}

/// On the Card's face no two stage circles run at once: while a worker
/// builds the Card, its build circle runs and its run circle has not started;
/// once the Card runs, the build is done.
@Test func aCardOnAWorkerRunsOneStageAtATime() throws {
  let records = [StageRecord(stage: .prepare, outcome: .done, start: 0, end: 10),
                 StageRecord(stage: .evaluate, outcome: .done, start: 10, end: 20),
                 StageRecord(stage: .execute, outcome: .running, start: 20)]
  var live = CardRunState(state: .running, invocation: 1, phase: .execute)
  live.executionService = "ttl"
  live.stages = records
  func circles(_ activity: Tsugumi_Data_ExecutionActivity, queued: Bool = false) throws -> [Stage: StageProgress.Item] {
    var state = live
    state.executionActivity = activity
    state.executionState = queued ? .queued : .running
    let progress = StageProgress.of(state, code: true)
    #expect(progress.items.filter { $0.status == .running }.count <= 1, "Two circles running for \(activity)")
    #expect(StagePipeline(stages: [.prepare, .evaluate, .build, .execute, .validate, .save], records: records,
                          service: "ttl", live: state, now: 100).steps.filter { $0.status == .running }.count <= 1)
    return Dictionary(uniqueKeysWithValues: progress.items.map { ($0.stage, $0) })
  }
  for activity in [Tsugumi_Data_ExecutionActivity.preparing, .building, .substituting] {
    let c = try circles(activity)
    #expect(c[.build]?.status == .running && c[.build]?.waiting == false && c[.execute]?.status == .pending)
  }
  let store = try circles(.waitingStore)
  #expect(store[.build]?.status == .running && store[.build]?.waiting == true && store[.execute]?.status == .pending)
  let queued = try circles(.unspecified, queued: true)
  #expect(queued[.build]?.status == .pending && queued[.execute]?.status == .running && queued[.execute]?.waiting == true)
  let running = try circles(.running)
  #expect(running[.build]?.status == .done && running[.execute]?.status == .running && running[.execute]?.waiting == false)
  // The project's own environment builds first, as the core records it.
  var local = live
  local.executionService = "local"
  local.stages = [StageRecord(stage: .prepare, outcome: .done, start: 0, end: 10),
                  StageRecord(stage: .evaluate, outcome: .running, start: 10)]
  let localProgress = StageProgress.of(local, code: true)
  #expect(localProgress.items.filter { $0.status == .running }.map(\.stage) == [.build])
  // The worker runs the Card but reported no build (everything was in its
  // store or substituted): the build circle is not left empty behind the
  // running one; it takes the cached look, on the face, in the hover's stage
  // table (the same items) and in the pipeline line.
  let nothingBuilt = try circles(.unspecified)
  #expect(nothingBuilt[.build]?.status == .cached && nothingBuilt[.execute]?.status == .running)
  var fetched = live
  fetched.executionState = .running
  fetched.executionLastActivity = .substituting
  #expect(StageProgress.of(fetched, code: true).items.first { $0.stage == .build }?.status == .cached)
  var quiet = live
  quiet.executionState = .running
  let quietLine = StagePipeline(stages: [.prepare, .evaluate, .build, .execute], records: records, service: "ttl", live: quiet, now: 100)
  #expect(quietLine.steps.map(\.status) == [.done, .done, .cached, .running])
  // Ended: the core's record of whether the build came from a store decides.
  var succeeded = live
  succeeded.state = .succeeded
  succeeded.executionLastActivity = .running
  succeeded.stages = [records[0], records[1], StageRecord(stage: .execute, outcome: .done, start: 20, end: 3_020)]
  succeeded.cached = true
  #expect(StageProgress.of(succeeded, code: true).items.first { $0.stage == .build }?.status == .cached)
  succeeded.cached = false
  #expect(StageProgress.of(succeeded, code: true).items.first { $0.stage == .build }?.status == .done)
}

/// The log's sections follow the pipeline line (the same rule as the Card's
/// face). While the worker builds, the build section runs, not done because
/// the core's own preparation and Nix evaluation finished, and holds the
/// worker's build time; the execution has not started yet. The Web UI checks
/// the same (stagePipeline.ts).
@Test func logSectionsAgreeWithThePipelineLine() throws {
  let order: [Stage] = [.prepare, .evaluate, .build, .execute, .validate, .save]
  let build: [Stage] = [.prepare, .evaluate, .build], execute: [Stage] = [.execute], save: [Stage] = [.validate, .save]
  let records = [StageRecord(stage: .prepare, outcome: .done, start: 0, end: 1_300),
                 StageRecord(stage: .evaluate, outcome: .done, start: 1_300, end: 5_420),
                 StageRecord(stage: .execute, outcome: .running, start: 5_420)]
  var live = CardRunState(state: .running, invocation: 1, phase: .execute)
  live.executionService = "ttl"
  live.executionState = .running
  live.executionActivity = .building
  live.executionActivityDetail = "rmfgen"
  live.stages = records
  let now: Int64 = 5_420 + 239_990
  func line(_ state: CardRunState?, _ rs: [StageRecord] = records, service: String = "ttl", lastActivity: Tsugumi_Data_ExecutionActivity? = nil) -> StagePipeline {
    StagePipeline(stages: order, records: rs, service: service, live: state, now: now, lastActivity: lastActivity)
  }
  let building = line(live)
  let b = building.section(build, ended: false)
  #expect(b.status == .running && !b.waiting)
  #expect(abs(try #require(b.seconds) - (1.3 + 4.12 + 239.99)) < 1e-9, "The worker's build time counts in the build section")
  let e = building.section(execute, ended: false)
  #expect(e.status == .pending && e.seconds == nil, "実行 says まだ until the Card starts")
  #expect(building.section(save, ended: false).status == .pending)
  // Waiting for the worker's Nix store: the build section waits.
  live.executionActivity = .waitingStore
  #expect(line(live).section(build, ended: false) == .init(status: .running, waiting: true, seconds: b.seconds))
  // Queued for a free slot: the execution waits, nothing is built yet.
  live.executionActivity = .unspecified
  live.executionState = .queued
  let queued = line(live).section(execute, ended: false)
  #expect(queued.status == .running && queued.waiting)
  // Once the Card runs, the build section is done and the execution runs.
  live.executionState = .running
  live.executionActivity = .running
  #expect(line(live).section(build, ended: false).status == .done)
  #expect(line(live).section(execute, ended: false).status == .running)
  // Nothing was built on the worker: the build section is done with the cache mark.
  live.executionActivity = .unspecified
  let nothing = line(live).section(build, ended: false)
  #expect(nothing.status == .done && nothing.cached)
  // A stage that reported nothing while a later one started is behind it,
  // not still running: no build recorded, the execution runs.
  var local = live
  local.executionService = "local"
  #expect(line(local, service: "local").section(build, ended: false).status == .done)
  #expect(line(local, service: "local").section(execute, ended: false).status == .running)
  // Stopped while the worker built: the build section carries the stop, the execution never ran.
  let stopped = [records[0], records[1], StageRecord(stage: .execute, outcome: .cancelled, start: 5_420, end: 100_000)]
  let ended = line(nil, stopped, lastActivity: .building)
  #expect(ended.section(build, ended: true).status == .cancelled)
  #expect(ended.section(execute, ended: true).status == .notRun)
  #expect(SectionStatus(ended.section(execute, ended: true).status) == .pending)
}

/// An open log section is as tall as its text: one line of output leaves no
/// empty area before the next section.
@MainActor @Test(.timeLimit(.minutes(1))) func aShortLogIsOnlyAsTallAsItsText() async throws {
  NSApplication.shared.setActivationPolicy(.prohibited)
  let reader = LogStreamReader()
  let read = try #require(reader.window.request(.tail))
  let text = "Regions: NE2\n"
  let size = UInt64(text.utf8.count)
  reader.apply(.init(kind: .tail, read: read, result: .success(
    LogPage(offset: 0, end: size, size: size, line: 1, startsLine: true, text: text, form: .terminal))))
  let measurement = LogHeightMeasurement()
  let host = NSHostingView(rootView: VStack(spacing: 0) {
    LogTextView(reader: reader, fitsText: true).layoutPriority(1)
      .background(GeometryReader { geometry in
        Color.clear.onChange(of: geometry.size.height, initial: true) { measurement.height = geometry.size.height }
      })
    Color.red.frame(height: 10)
    Spacer(minLength: 0)
  }.frame(width: 400, height: 400))
  let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 400, height: 400), styleMask: [.borderless], backing: .buffered, defer: true)
  window.contentView = host
  defer { window.contentView = nil }
  host.layoutSubtreeIfNeeded()
  // One line of text, not the room offered (400) nor the old minimum (60).
  try await waitForModel("the log's height") { measurement.height > 10 && measurement.height < 40 }
}

@MainActor @Observable private final class LogHeightMeasurement {
  var height: CGFloat = 0
}

@MainActor @Observable private final class HoverLayoutMeasurement {
  var size = CGSize.zero
}

/// The bubble takes its ideal size: long text must wrap inside it, not run out of it.
@MainActor @Test func theHoverBubbleHoldsItsWrappedText() async throws {
  func size(_ lines: [String], height: CGFloat = 600) async throws -> CGSize {
    let viewport = CGSize(width: 800, height: height)
    let content = HoverBubble.Content(title: "rgb", subtitle: "Draft", symbol: "circle", lines: lines,
                                       status: "未実装（Draft）", problems: ["draft cards are not implemented and cannot run"])
    let measurement = HoverLayoutMeasurement()
    let view = NSHostingView(rootView: HoverBubble(content: content, viewport: viewport) { _ in }
      .fixedSize()
      .onGeometryChange(for: CGSize.self) { $0.size } action: { measurement.size = $0 })
    let window = NSWindow(contentRect: CGRect(origin: .zero, size: viewport),
                          styleMask: [.borderless], backing: .buffered, defer: true)
    window.contentView = view
    defer { window.contentView = nil }
    view.layoutSubtreeIfNeeded()
    try await waitForModel("diagnostic hover layout") { measurement.size.height > 30 }
    return measurement.size
  }
  let description = String(repeating: "3つの帯域画像を同じ座標と大きさのまま割り当てる。", count: 6)
  let short = try await size(["短い説明"])
  let long = try await size([description])
  let constrained = try await size([description], height: 110)
  #expect(long.width <= 361)
  #expect(long.height >= short.height + 40, "\(short) \(long)")
  #expect(constrained.height <= 98)
}

/// A wire to or from a Draft is dotted until the Draft has code. The web checks
/// the same (ui/web/tests/draftWires.tsx).
@MainActor @Test func draftWiresAreDotted() async throws {
  let dir = testTemporaryDirectory.appendingPathComponent("draft-wires-\(UUID().uuidString.prefix(8))")
  defer { try? FileManager.default.removeItem(at: dir) }
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  let (start, log, draft) = ("start", "log", "plan")
  let s = try offlineSession(dir, cards: [(start, fixtureCardJSON(.trigger)), (log, fixtureCardJSON(.code)), (draft, fixtureCardJSON(.draft))])

  #expect(s.isDraftWire(from: draft, to: log), "from a Draft")
  #expect(s.isDraftWire(from: start, to: draft), "to a Draft")
  #expect(s.isDraftWire(from: draft, to: draft), "between Drafts")
  #expect(!s.isDraftWire(from: start, to: log), "between Cards with code")
  #expect(!s.isDraftWire(from: start, to: nil), "from a Card with code to the bar")

  // Once the Card is implemented, endpoint styling becomes solid.
  let implemented = Tsugumi_RPC_DocumentDeltaT()
  implemented.baseRev = 0; implemented.rev = 1; implemented.origin = "agent"
  let definition = Tsugumi_RPC_DefinitionT(); definition.id = draft; definition.cardJson = fixtureCardJSON(.code)
  implemented.definitionsUpsert = [definition]
  s.applyDelta(implemented)
  #expect(s.kind(draft) == .code)
  #expect(!s.isDraftWire(from: draft, to: log))
  #expect(!s.isDraftWire(from: start, to: draft))

  // The dots flow with the active wires' phase, which wraps at one dash period.
  let (on, off) = (WireLayer.dot[0], WireLayer.dot[1])
  #expect(on == 0, "round dots")
  #expect(off > 0)
  let dots = WireLayer.dash.reduce(0, +) / off
  #expect(abs(dots - dots.rounded()) < 1e-9, "a whole number of dots per period")
}

/// A Card whose _card.json has no type or an unknown one is drawn as a failure with
/// the core's diagnostic, no ports and no run. The web checks the same
/// (ui/web/tests/definitionErrors.tsx).
@MainActor @Test func cardsWithoutAKnownTypeAreDefinitionErrors() async throws {
  let dir = testTemporaryDirectory.appendingPathComponent("definition-errors-\(UUID().uuidString.prefix(8))")
  defer { try? FileManager.default.removeItem(at: dir) }
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  let s = try offlineSession(dir, cards: [
    ("old", #"{"inputs":[{"name":"in"}]}"#),
    ("future", #"{"type":"something new","inputs":[{"name":"in"}]}"#),
    ("plan", fixtureCardJSON(.draft)),
    ("step", fixtureCardJSON(.code)),
  ])
  #expect(s.kind("old") == .invalid)
  #expect(s.kind("future") == .invalid)
  #expect(s.kind("plan") == .draft, "a Draft stays a Draft")
  #expect(s.kind("step") == .code)
  #expect(definitionKind(nil) == .invalid, "a Card without a definition")
  #expect(definitionKind(try JSONValue.parse(fixtureCardJSON(.subgraph))) == .subgraph)
  #expect(definitionKind(try JSONValue.parse(#"{"type":"invalid"}"#)) == .invalid, "not a type name")

  // The diagnostic belongs to the callout outside the Card body.
  let missing = #"definitions/old/_card.json: "type" is required (one of code, draft, trigger, merge, collect, subgraph)"#
  let delta = Tsugumi_RPC_DocumentDeltaT()
  delta.baseRev = 0; delta.rev = 1; delta.origin = "agent"
  delta.resolution = ["old", "future", "plan", "step"].map { id in
    let r = Tsugumi_RPC_ResolvedCardT(); r.card = cardPath([id])
    let out = Tsugumi_Data_PortT(); out.name = "out"; r.outputs = [out]
    if id == "old" {
      let d = Tsugumi_Data_DiagnosticT()
      d.severity = .error; d.code = "invalid_document"; d.message = missing; d.card = cardPath([id]); d.path = "definitions/old/_card.json"
      r.diagnostics = [d]
    }
    return r
  }
  s.applyDelta(delta)
  for id in ["old", "future"] {
    let d = s.nodeData(id)
    #expect(d.kind == .invalid && d.inputs.isEmpty && d.outputs.isEmpty, "\(id): no ports, declared or resolved")
    let g = CardGeometry(d, flow: .topToBottom)
    #expect(g.inputs.isEmpty && g.outputs.isEmpty && !g.draft && !g.stages)
    #expect(g.newPortPoint(output: false) == nil, "nothing to draw a wire from")
    #expect(d.idleText == String(localized: "定義エラー"))
  }
  let old = s.nodeData("old")
  #expect(old.summary == nil && old.errors == [missing])
  #expect(!CardGeometry(old, flow: .topToBottom).summary)
  #expect(s.nodeData("future").errors == [String(localized: "Card の定義を読めません")], "without a diagnostic yet")

  // A version from before `type` (or one this build cannot read) has no kind.
  #expect(CardKind.named("") == .invalid && CardKind.named("something new") == .invalid && CardKind.named("merge") == .merge)
  let map = VersionMiniMapLayout([.init(id: "old", x: 0, y: 0, kind: CardKind.named("")), .init(id: "step", x: 300, y: 0)],
                                 flow: .leftToRight, width: 284, height: 132)
  #expect(map.cards["old"]?.invalid == true && map.cards["step"]?.invalid == false)
}

@Test func aDraftOffersCardAttachmentsAndOpensWhileWritten() {
  var data = CardNodeData(title: "x", kind: .draft, summary: "plan", inputs: [], outputs: [])
  let empty = CardGeometry(data, flow: .topToBottom)
  #expect(empty.newPortPoint(output: true) == CGPoint(x: CardGeometry.width / 2, y: empty.height))
  #expect(empty.port(CardGeometry.newPort, output: false) == CGPoint(x: CardGeometry.width / 2, y: 0))
  data.outputs = [PortData(name: "out")]
  let wired = CardGeometry(data, flow: .topToBottom)
  // Even supplied port declarations are ignored: Draft wires use Card anchors.
  #expect(wired.port("out", output: true) == CGPoint(x: CardGeometry.width / 2, y: wired.height))
  #expect(wired.newPortPoint(output: true) == empty.newPortPoint(output: true), "attachment remains available for more wires")
  #expect(wired.height == empty.height)
  // Across a left-to-right flow: the same anchor at the middle of the side.
  let right = CardGeometry(data, flow: .leftToRight)
  #expect(right.port("out", output: true) == CGPoint(x: CardGeometry.width, y: right.height / 2))
  #expect(right.newPortPoint(output: false) == CGPoint(x: 0, y: right.height / 2))
  data.outputs = [PortData(name: "x"), PortData(name: "y")]
  let two = CardGeometry(data, flow: .leftToRight)
  #expect((two.port("x", output: true).y + two.port("y", output: true).y) / 2 == two.height / 2)
  // Without a name, a Draft is its content.
  data.untitled = true
  #expect(CardGeometry(data, flow: .topToBottom).headerRow == CardGeometry.untitledTop)
  data.untitled = false
  data.draftEditing = true
  #expect(CardGeometry(data, flow: .topToBottom).draftRow == CardGeometry.draftEditRow)
  data.draftEditing = false
  data.outputs = []
  data.collapsed = true
  #expect(CardGeometry(data, flow: .topToBottom).newPortPoint(output: true) == nil)
}

@Test func wirePortsFollowTheFlow() {
  let data = CardNodeData(title: "x", kind: .code, summary: nil,
                          inputs: [PortData(name: "a"), PortData(name: "b")], outputs: [PortData(name: "out")])
  let down = CardGeometry(data, flow: .topToBottom)
  #expect(down.port("a", output: false).y == 0 && down.port("out", output: true).y == down.height)
  #expect(down.port("a", output: false).x < down.port("b", output: false).x)
  let right = CardGeometry(data, flow: .leftToRight)
  #expect(right.port("a", output: false).x == 0 && right.port("out", output: true).x == CardGeometry.width)
}

@Test func fileTreeGroupsFolders() throws {
  let tree = FileNode.tree(["default.nix", "src/main.py", "src/lib/a.py", "_outputs.fbs"], base: "definitions/d/")
  #expect(Set(tree.map(\.name)) == ["src", "_outputs.fbs", "default.nix"])
  let src = try #require(tree.first { $0.name == "src" }?.children)
  #expect(Set(src.map(\.name)) == ["lib", "main.py"])
  #expect(src.first { $0.name == "main.py" }?.path == "definitions/d/src/main.py")
  #expect(src.first { $0.name == "lib" }?.children?.first { $0.name == "a.py" }?.path == "definitions/d/src/lib/a.py")
}

@Test func paramsKeepExactIntegerSpelling() {
  #expect(parseParam("9223372036854775807", "int") == .number("9223372036854775807"))
  #expect(parseParam("-3", "int") == .number("-3"))
  #expect(parseParam("1.5e3", "float") == .number("1.5e3"))
  // Values the core must reject are passed on as text, never coerced.
  #expect(parseParam("12abc", "int") == .string("12abc"))
  #expect(parseParam(" x ", "string") == .string(" x "))
}

@Test func theSmallCardsKeepTheirShapeWithASummary() {
  func geometry(_ kind: CardKind) -> CardGeometry {
    CardGeometry(CardNodeData(title: "x", kind: kind, summary: "what it does", inputs: [], outputs: []), flow: .topToBottom)
  }
  for kind in [CardKind.trigger, .merge, .collect] { #expect(!geometry(kind).summary, "\(kind)") }
  // An ordinary Card's description is on hover: its face has the name and status line.
  for kind in [CardKind.code, .subgraph] {
    #expect(geometry(kind).face && !geometry(kind).summary, "\(kind)")
  }
}

@Test func aCardsFaceKeepsItsSizeWhateverItsState() {
  func card(_ title: String) -> CardNodeData {
    CardNodeData(title: title, kind: .code, summary: "what it does",
                 inputs: [PortData(name: "a")], outputs: [PortData(name: "b")])
  }
  var running = card("x")
  running.state = .running
  running.invocation = 1
  running.stage = StageProgress(items: [.init(stage: .build, status: .done), .init(stage: .execute, status: .running),
                                        .init(stage: .validate, status: .pending)])
  var failed = card("x")
  failed.state = .failed
  failed.invocation = 1
  failed.failure = "error: boom\nat line 3"
  var succeeded = card("x")
  succeeded.state = .succeeded
  succeeded.invocation = 3
  succeeded.seconds = 4
  succeeded.endedAt = Date()
  var queued = card("x")
  queued.state = .preparing
  queued.invocation = 1
  queued.activity = "waiting for a worker"
  let long = card(String(repeating: "a long name of a Card ", count: 6))
  for flow in [FlowDirection.topToBottom, .leftToRight] {
    let size = CardGeometry(card("x"), flow: flow).size
    for d in [running, failed, succeeded, queued, long] {
      #expect(CardGeometry(d, flow: flow).size == size, "\(d.title) \(flow)")
    }
  }
  #expect(CardGeometry(card("x"), flow: .topToBottom).face)
}

@Test func aCardsStatusLineSaysItsStateInOneLine() {
  var d = CardNodeData(title: "x", kind: .code, summary: nil, inputs: [], outputs: [])
  #expect(d.statusLine().text == String(localized: "未実行"))
  d.state = .running
  d.invocation = 1
  d.stage = StageProgress(items: [.init(stage: .build, status: .running), .init(stage: .execute, status: .pending),
                                  .init(stage: .validate, status: .pending)])
  #expect(d.statusLine().text == Stage.build.activeLabel)
  #expect(d.statusLine().tone == .state(.running))
  d.activity = "waiting for a worker"
  #expect(d.statusLine().text == "waiting for a worker")
  d.activity = nil
  d.state = .succeeded
  d.invocation = 3
  let now = Date()
  d.endedAt = now.addingTimeInterval(-180)
  d.seconds = 4
  #expect(d.statusLine(now: now).text == Tsugumi_Data_CardState.succeeded.label + " ×3 · " + relativeAge(d.endedAt!, now: now) + " · 4.0 s")
  #expect(d.statusLine().tone == .plain)
  d.state = .failed
  d.failure = "\nerror: no such file\n  at main.py:3"
  d.stage = StageProgress(items: [.init(stage: .build, status: .done), .init(stage: .execute, status: .failed),
                                  .init(stage: .validate, status: .notRun)])
  #expect(d.statusLine().text == "run: no such file")
  #expect(d.statusLine().tone == .failed)
  #expect(d.statusLine().hover == nil)
  // The shown build stage covers preparation, evaluation and build: the gist
  // names the core's own stage that failed, as the log does.
  d.stage = StageProgress(items: [.init(stage: .build, status: .failed), .init(stage: .execute, status: .notRun),
                                  .init(stage: .validate, status: .notRun)], failedPart: .prepare)
  #expect(d.statusLine().text == Stage.prepare.name + ": no such file")
  #expect(formatShortDuration(192) == "3:12" && formatShortDuration(3723) == "1:02:03")
}

@Test func aCardNoWorkerCouldTakeSaysItNeverStarted() {
  // The core names each worker's reason; the face says in red that the Card
  // never started, the hover why for each worker, and the size stays.
  let failure = Tsugumi_Data_FailureT()
  failure.kind = .unsupported
  failure.diagnostic = "No worker could be chosen"
  failure.args = [("stage", "worker_selection"), ("worker:ttl", "timeout"), ("worker:gpu", "failed")].map { k, v in
    let a = Tsugumi_Data_KeyValueT()
    a.key = k
    a.value = v
    return a
  }
  let reasons = failure.notStartedText
  let ttl = "ttl", gpu = "gpu"
  #expect(reasons == String(localized: "\(ttl) の probe が時間切れ") + String(localized: "、") + String(localized: "\(gpu) の probe が失敗"))
  let other = Tsugumi_Data_FailureT()
  other.diagnostic = "no such file"
  #expect(other.notStartedText == nil)
  var d = CardNodeData(title: "x", kind: .code, summary: nil, inputs: [], outputs: [])
  let size = [FlowDirection.topToBottom, .leftToRight].map { CardGeometry(d, flow: $0).size }
  d.state = .failed
  d.invocation = 1
  d.failure = failure.plainText
  d.notStarted = reasons
  #expect(d.statusLine() == CardStatusLine(text: String(localized: "worker を選べず未開始"), tone: .failed, hover: reasons))
  #expect([FlowDirection.topToBottom, .leftToRight].map { CardGeometry(d, flow: $0).size } == size)
}

@Test func aWorkerFromAnotherRevisionIsNamedAsOutOfDate() {
  // The core names the worker's contract and its own; the hover says to rebuild it.
  let failure = Tsugumi_Data_FailureT()
  failure.kind = .unsupported
  failure.args = [("stage", "worker_selection"), ("worker:ttl", "outdated"), ("contract:ttl", "21"), ("contract", "22")].map { k, v in
    let a = Tsugumi_Data_KeyValueT()
    a.key = k
    a.value = v
    return a
  }
  let name = "ttl", worker = "21", current = "22"
  #expect(failure.notStartedText == String(localized: "\(name) は古い版です（契約 \(worker)、この Tsugumi は \(current)）。ワーカーを同じ版で作り直してください"))
}

@MainActor @Test func theHoverHasWhatTheFaceLeavesOut() {
  var d = CardNodeData(title: "x", kind: .code, summary: "what it does", inputs: [], outputs: [])
  let idle = HoverBubble.Content.card(d, details: ["what it does"], runId: nil) {}
  #expect(idle.lines == ["what it does"] && idle.stages.isEmpty && idle.openLog == nil)
  d.state = .failed
  d.invocation = 1
  d.failure = "error: no such file\n  at main.py:3"
  d.stage = StageProgress(items: [.init(stage: .build, status: .done, seconds: 1), .init(stage: .execute, status: .failed),
                                  .init(stage: .validate, status: .notRun)])
  let failed = HoverBubble.Content.card(d, details: ["what it does"], runId: "0123456789") {}
  #expect(failed.lines == ["what it does"])
  #expect(failed.stages.map(\.stage) == [.build, .execute, .validate])
  #expect(failed.failure == d.failure && failed.openLog != nil)
  #expect(failed.problems.isEmpty)
  // A remote execution waiting for its worker: the face says it short, the
  // hover in full, with where it runs and whether it is working.
  var waits = CardNodeData(title: "x", kind: .code, summary: nil, inputs: [], outputs: [])
  waits.state = .running
  waits.invocation = 1
  waits.stage = StageProgress(items: [.init(stage: .build, status: .done), .init(stage: .execute, status: .running, waiting: true),
                                      .init(stage: .validate, status: .pending)])
  waits.activity = "waiting (worker slot)"
  waits.activityDetail = "waiting for a free slot on the worker"
  waits.worker = "ttl"
  waits.liveness = "CPU 00:00:01.00"
  #expect(waits.statusLine().text == "waiting (worker slot)")
  #expect(CardGeometry(waits, flow: .topToBottom).size
    == CardGeometry(CardNodeData(title: "x", kind: .code, summary: nil, inputs: [], outputs: []), flow: .topToBottom).size)
  let hover = HoverBubble.Content.card(waits, details: [], runId: nil) {}
  #expect(hover.stages.contains { $0.stage == .execute && $0.waiting })
  #expect(hover.facts.contains("waiting for a free slot on the worker") && hover.facts.contains("CPU 00:00:01.00"))
  #expect(hover.facts.contains { $0.hasSuffix(": ttl") })
}

@Test func collapsedCardsKeepTheirPortsOnTheEdges() {
  var data = CardNodeData(title: "x", kind: .code, summary: "s",
                          inputs: [PortData(name: "a"), PortData(name: "b")], outputs: [PortData(name: "x")])
  data.collapsed = true
  let closed = CardGeometry(data, flow: .leftToRight)
  #expect(closed.height == CardGeometry.header)
  #expect(closed.port("b", output: false) == CGPoint(x: 0, y: CardGeometry.header / 2))
  let down = CardGeometry(data, flow: .topToBottom)
  #expect(down.port("x", output: true).y == CardGeometry.header)
}

@Test func shortcutsHaveNoDefaultConflicts() {
  let keys = ShortcutAction.allCases.map(\.defaultKey)
  #expect(Set(keys).count == keys.count)
}

@Test func routingCardsHaveTheirShapeAndPorts() {
  let merge = CardGeometry(CardNodeData(title: "m", kind: .merge, summary: nil,
                                        inputs: [PortData(name: "a"), PortData(name: "b")], outputs: [PortData(name: "out")]), flow: .topToBottom)
  #expect(abs(merge.port("out", output: true).x - merge.size.width / 2) < 0.5)
  #expect(merge.port("a", output: false).y == merge.outlineRect.minY)
}

@Test func portLabelsSitOnTheirPortsOrHide() {
  let few = CardGeometry(CardNodeData(title: "x", kind: .code, summary: nil, inputs: [PortData(name: "a"), PortData(name: "b")],
                                      outputs: []), flow: .topToBottom)
  #expect(few.showLabels(input: true))
  let many = CardGeometry(CardNodeData(title: "x", kind: .code, summary: nil,
                                       inputs: (0..<12).map { PortData(name: "port\($0)") }, outputs: []), flow: .topToBottom)
  // Twelve ports on 220 points leave no room: names are shown on hover instead.
  #expect(!many.showLabels(input: true) && many.topLabels == 0)
}

@Test func feedbackWiresGoAroundTheCards() {
  let cards = [CGRect(x: 0, y: 0, width: 220, height: 80), CGRect(x: 0, y: 160, width: 220, height: 80),
               CGRect(x: 0, y: 320, width: 220, height: 80)]
  var router = WireRouter(flow: .topToBottom, obstacles: cards)
  // From the bottom Card back up to the top one.
  let back = router.route(CGPoint(x: 110, y: 400), CGPoint(x: 110, y: 0))
  let middle = cards[1].insetBy(dx: 2, dy: 2)
  for t in stride(from: 0.02, through: 0.98, by: 0.02) {
    let p = back.trimmedPath(from: 0, to: t).currentPoint ?? .zero
    #expect(!middle.contains(p))
  }
  // A second feedback wire gets its own lane.
  let other = router.route(CGPoint(x: 100, y: 400), CGPoint(x: 100, y: 0))
  #expect(abs(other.boundingRect.minX - back.boundingRect.minX) >= 12)
  // Forward wires stay curves along the flow.
  #expect(router.isForward(CGPoint(x: 0, y: 80), CGPoint(x: 0, y: 160)))
}

@Test func wiresBetweenClosePortsTakeTheShortWay() {
  func length(_ path: Path) -> CGFloat {
    let points = stride(from: 0.0, through: 1.0, by: 1.0 / 64).map { path.trimmedPath(from: 0, to: max($0, 0.0001)).currentPoint ?? .zero }
    return zip(points, points.dropFirst()).reduce(0) { $0 + hypot($1.1.x - $1.0.x, $1.1.y - $1.0.y) }
  }
  let merge = CGRect(x: 0, y: 0, width: 80, height: 50)
  for gap in [0, 1, 20, 23, 24, 25, 40] as [CGFloat] {
    let card = CGRect(x: -70, y: 50 + gap, width: 220, height: 90)
    var down = WireRouter(flow: .topToBottom, obstacles: [merge, card])
    #expect(length(down.route(CGPoint(x: 40, y: 50), CGPoint(x: 40, y: 50 + gap))) <= gap + 0.5, "top to bottom, \(gap)")
  }
  var across = WireRouter(flow: .leftToRight, obstacles: [CGRect(x: 0, y: 0, width: 50, height: 80), CGRect(x: 70, y: -70, width: 90, height: 220)])
  #expect(length(across.route(CGPoint(x: 50, y: 40), CGPoint(x: 70, y: 46))) < 25)
  // No bulge past either port.
  var offset = WireRouter(flow: .topToBottom, obstacles: [merge])
  let bent = offset.route(CGPoint(x: 40, y: 50), CGPoint(x: 52, y: 70)).boundingRect
  #expect(bent.minY >= 49.5 && bent.maxY <= 70.5)
  // Upstream by a point is still upstream: it goes around.
  var back = WireRouter(flow: .topToBottom, obstacles: [merge, CGRect(x: -70, y: 70, width: 220, height: 90)])
  #expect(length(back.route(CGPoint(x: 40, y: 51), CGPoint(x: 40, y: 50))) > 50)
  // The short way through a Card is not taken.
  let wall = CGRect(x: 100, y: 45, width: 60, height: 30)
  var blocked = WireRouter(flow: .topToBottom, obstacles: [merge, wall])
  #expect(!blocked.isForward(CGPoint(x: 40, y: 50), CGPoint(x: 220, y: 60)))
  let around = blocked.route(CGPoint(x: 40, y: 50), CGPoint(x: 220, y: 60))
  for t in stride(from: 0.02, through: 0.98, by: 0.02) {
    #expect(!wall.insetBy(dx: 2, dy: 2).contains(around.trimmedPath(from: 0, to: t).currentPoint ?? .zero))
  }
}

/// The paths the web finds for the same wires (ui/web/tests/wireDemos.ts writes the file):
/// real layouts of the demos, so the two routers cannot drift apart.
@Test func wiresRouteAsOnTheWeb() async throws {
  struct Point: Decodable { let x, y: CGFloat }
  struct Rect: Decodable { let x, y, width, height: CGFloat }
  struct Ends: Decodable { let a, b: Point }
  struct Case: Decodable {
    let name, flow: String
    let obstacles: [Rect]
    let wires: [Ends]
  }
  let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../..")
  let cases = try JSONDecoder().decode([Case].self, from: Data(contentsOf: root.appendingPathComponent("tests/fixtures/wire-routes.json")))
  #expect(!cases.isEmpty)
  for c in cases {
    var router = WireRouter(flow: c.flow == "leftToRight" ? .leftToRight : .topToBottom,
                            obstacles: c.obstacles.map { CGRect(x: $0.x, y: $0.y, width: $0.width, height: $0.height) })
    let plans = router.plan(c.wires.map { (a: CGPoint(x: $0.a.x, y: $0.a.y), b: CGPoint(x: $0.b.x, y: $0.b.y)) })
    for (i, plan) in plans.enumerated() {
      let ends = c.wires[i]
      let start = CGPoint(x: ends.a.x, y: ends.a.y), end = CGPoint(x: ends.b.x, y: ends.b.y)
      let path = router.route(start, end)
      // A Path keeps single-precision coordinates.
      #expect(path.currentPoint.map { hypot($0.x - end.x, $0.y - end.y) < 1e-3 } == true)
      if let plan {
        #expect(plan.first == start && plan.last == end)
        #expect(plan.allSatisfy { $0.x.isFinite && $0.y.isFinite })
      }
      for box in c.obstacles {
        let rect = CGRect(x: box.x + 2, y: box.y + 2, width: box.width - 4, height: box.height - 4)
        if rect.contains(start) || rect.contains(end) { continue }
        for t in stride(from: 0.01, through: 0.99, by: 0.01) {
          #expect(!rect.contains(path.trimmedPath(from: 0, to: t).currentPoint ?? start), "\(c.name), wire \(i)")
        }
      }
    }
  }
}

@Test func editedOutputsAreReadFromTheText() {
  let ok = OutputCatalog.parse("""
    namespace t; // ports
    table Num { value:ulong; }
    table Outputs { out:Num (required); extra:Num; /* note */ }
    root_type Outputs;
    """)
  #expect(ok == .ports([PortData(name: "out", typeName: "Num"), PortData(name: "extra", typeName: "Num")]))
  if case .error = OutputCatalog.parse("table Outputs { out:Num ") {} else { Issue.record("an unfinished catalog is an error") }
  if case .error = OutputCatalog.parse("table Outputs { out:Num; }") {} else { Issue.record("root_type is required") }
}

@Test func recentAgesHaveNoSeconds() {
  let now = Date()
  #expect(relativeAge(now.addingTimeInterval(-20), now: now) == String(localized: "たった今"))
  #expect(relativeAge(now.addingTimeInterval(-20 * 60 - 51), now: now) == String(localized: "\(20) 分前"))
  #expect(relativeAge(now.addingTimeInterval(-3 * 3600), now: now) == String(localized: "\(3) 時間前"))
  #expect(relativeAge(now.addingTimeInterval(-2 * 86400), now: now) == String(localized: "\(2) 日前"))
}

@Test func cameraFitsInsideTheFreeArea() {
  var c = CanvasCamera()
  c.fit(CGRect(x: 0, y: 0, width: 400, height: 300), in: CGSize(width: 800, height: 600),
        insets: SwiftUI.EdgeInsets(top: 50, leading: 0, bottom: 50, trailing: 0), maxZoom: 1, margin: 0)
  let tl = c.screen(CGPoint(x: 0, y: 0)), br = c.screen(CGPoint(x: 400, y: 300))
  #expect(tl.y >= 50 && br.y <= 550 && abs((tl.x + br.x) / 2 - 400) < 0.5)
}

@Test func fitAllKeepsTheGraphClearOfTheTopCapsules() {
  // The canvas runs under the window's bars: a tall graph fitted into it
  // starts below them, whatever their height.
  for safeTop in [0.0, 28, 38, 52] {
    // What CanvasWorkspace passes: the strip's height, 0 under a tab bar.
    let top = safeTop > 0 ? TopStrip.height(safeTop: safeTop) : 0
    var c = CanvasCamera()
    c.fit(CGRect(x: 0, y: 0, width: 300, height: 900), in: CGSize(width: 800, height: 600),
          insets: GraphCanvas.fitInsets(top: top), maxZoom: 1.2, margin: 24)
    #expect(c.screen(CGPoint(x: 0, y: 0)).y >= top + 8)
    #expect(c.screen(CGPoint(x: 0, y: 900)).y <= 600 - 48)
    if safeTop > 0 {
      // Below the capsules floating in the strip, with room to spare.
      let strip = CGRect(x: 0, y: 0, width: 800, height: top)
      for open in [false, true] {
        let f = TopStrip.frames(in: strip, chatWidth: 320, chatOpen: open,
                                run: CGSize(width: 170, height: 34), chat: CGSize(width: 40, height: 34))
        #expect(c.screen(CGPoint(x: 0, y: 0)).y >= max(f.run.maxY, f.chat.maxY) + TopStrip.minVerticalMargin)
      }
    }
  }
}

@Test func theTopStripsCapsulesKeepTheirInsetInEveryLayout() {
  let run = CGSize(width: 172, height: 34), toggle = CGSize(width: 40, height: 34)
  let inset = TopStrip.inset
  // The detail's width: the window less the sidebar, if shown (a main window
  // is at least 1100 wide, a Card in Card's window 900).
  for width: CGFloat in [900 - 250, 900, 1100 - 250, 1100, 1600] {
    for safeTop: CGFloat in [28, 52] {
      let strip = CGRect(x: 0, y: 0, width: width, height: TopStrip.height(safeTop: safeTop))
      // Vertically centered on the strip (the traffic lights' center) and inside it.
      func checkVertical(_ r: CGRect) {
        #expect(abs(r.midY - strip.midY) < 0.5)
        #expect(r.minY >= TopStrip.minVerticalMargin && strip.height - r.maxY >= TopStrip.minVerticalMargin)
      }
      // The chat closed: the toggle an inset from the window's edge, the run
      // capsule beside it.
      let closed = TopStrip.frames(in: strip, chatWidth: 480, chatOpen: false, run: run, chat: toggle)
      #expect(closed.chat.size == toggle && closed.run.size == run)
      #expect(abs(strip.maxX - closed.chat.maxX - inset) < 0.5)
      #expect(closed.run.maxX <= closed.chat.minX - TopStrip.gap + 0.5)
      checkVertical(closed.run)
      checkVertical(closed.chat)
      // The chat open, at any width it can take: the header an inset inside
      // its column, the run capsule an inset before the column.
      for wanted: Double in [ChatSplit.minChat, 480, ChatSplit.maxChat] {
        let column = ChatSplit.chatWidth(wanted, in: width)
        guard column - 2 * inset >= TopStrip.minChatHeaderWidth else { continue }
        let columnX = strip.maxX - column
        let open = TopStrip.frames(in: strip, chatWidth: wanted, chatOpen: true, run: run, chat: toggle)
        #expect(abs(open.chat.minX - (columnX + inset)) < 0.5, "\(width) \(wanted)")
        #expect(abs(strip.maxX - open.chat.maxX - inset) < 0.5)
        #expect(abs(columnX - open.run.maxX - inset) < 0.5, "\(width) \(wanted)")
        #expect(open.run.size == run)
        checkVertical(open.run)
        checkVertical(open.chat)
      }
    }
  }
}

@Test func aTitleBarDoubleClickFollowsTheSystemSetting() {
  // Read from the global domain, which differs between Macs: the mapping alone.
  #expect(TitleBarDoubleClick.action(setting: nil, miniaturize: false) == .zoom)
  #expect(TitleBarDoubleClick.action(setting: nil, miniaturize: true) == .minimize)
  for (value, action) in [("Maximize", TitleBarDoubleClick.zoom), ("Fill", .zoom), ("Minimize", .minimize), ("None", .none)] {
    #expect(TitleBarDoubleClick.action(setting: value, miniaturize: true) == action, "\(value)")
  }
}

@Test func aSavedCameraCanLeaveEveryCardOutsideTheViewport() {
  let cards = [CGRect(x: 320, y: 300, width: 200, height: 82),
               CGRect(x: 640, y: 460, width: 200, height: 82)]
  let viewport = CGSize(width: 610, height: 778)
  var camera = CanvasCamera(x: -266, y: -5211, zoom: 1.2)
  #expect(!camera.showsCard(cards, in: viewport))
  camera.fit(cards[0].union(cards[1]), in: viewport, maxZoom: 1.2)
  #expect(cards.allSatisfy { camera.showsCard([$0], in: viewport) })
  // Preserving a restored frame only requires one Card to be visible.
  #expect(camera.showsCard([cards[0].offsetBy(dx: 10_000, dy: 0), cards[1]], in: viewport))
}

@Test func droppedNoteLandsUnderThePointerAtAnyZoom() {
  // The note's grabbed point moves by the pointer's translation: screen points / zoom = world units.
  for zoom in [0.2, 0.5, 1, 2.5, 3] {
    let from = CGPoint(x: 100, y: 40), by = CGSize(width: 60, height: -30)
    let to = GraphCanvas.droppedNoteOrigin(from: from, by: by, zoom: zoom)
    let cam = CanvasCamera(x: 40, y: 40, zoom: zoom)
    let before = cam.screen(from), after = cam.screen(to)
    // Within the rounding to whole world units.
    #expect(abs((after.x - before.x) - by.width) <= zoom / 2 + 1e-9)
    #expect(abs((after.y - before.y) - by.height) <= zoom / 2 + 1e-9)
  }
  #expect(GraphCanvas.droppedNoteOrigin(from: CGPoint(x: 10, y: 10), by: CGSize(width: 15, height: 7), zoom: 2) == CGPoint(x: 18, y: 14))
  // Not snapped to the grid.
  #expect(GraphCanvas.droppedNoteOrigin(from: .zero, by: CGSize(width: 13, height: 0), zoom: 1).x == 13)
}

@MainActor
@Test func editorSelectionAlwaysFitsTheNewText() {
  let tv = NSTextView()
  tv.string = "table Outputs { out:Num (required); }\nroot_type Outputs;\n"
  tv.setSelectedRange(NSRange(location: 40, length: 10))
  // A shorter text: no old range fits; an empty list would raise in AppKit.
  tv.string = "x"
  CodeEditor.select([NSRange(location: 40, length: 10)], in: tv)
  #expect(tv.selectedRange() == NSRange(location: 1, length: 0))
  // A range that partly fits is cut at the end of the text.
  tv.string = "hello world"
  CodeEditor.select([NSRange(location: 6, length: 50)], in: tv)
  #expect(tv.selectedRange() == NSRange(location: 6, length: 5))
  // No ranges at all: a cursor at the start.
  CodeEditor.select([], in: tv)
  #expect(tv.selectedRange() == NSRange(location: 0, length: 0))
  // UTF-16 lengths (an emoji is two units): never half a character.
  tv.string = "日本😀"
  CodeEditor.select([NSRange(location: 3, length: 9)], in: tv)
  #expect(tv.selectedRange() == NSRange(location: 2, length: 2))
}

@Test func treeSitterColorsEveryEditorLanguage() {
  func tokens(_ text: String, _ lang: String) -> Set<String> {
    let spans = TreeSitterHighlighter.spans(text, language: lang) ?? []
    return Set(spans.map { r, t in "\(t):" + (text as NSString).substring(with: r) })
  }
  let cc = tokens("// note\n#include <card.hh>\nstruct Out { int v; };\nauto run() -> int { return 1; }\n", "cc")
  #expect(cc.contains("comment:// note") && cc.contains("keyword:struct") && cc.contains("keyword:return"))
  let py = tokens("# note\ndef main(inputs, report):\n    return \"x\"\n", "py")
  #expect(py.contains("comment:# note") && py.contains("keyword:def") && py.contains("string:\"x\""))
  let nix = tokens("# note\n{ pkgs, ... }: let x = 1; in { a = \"s\"; }\n", "nix")
  #expect(nix.contains("comment:# note") && nix.contains("keyword:let") && nix.contains("number:1"))
  let fbs = tokens("// note\nnamespace t;\ntable Num { value:ulong (required); }\nroot_type Num;\n", "fbs")
  #expect(fbs.contains("comment:// note") && fbs.contains("keyword:table") && fbs.contains("type:Num") && fbs.contains("property:value"))
  #expect(TreeSitterHighlighter.spans("x", language: "txt") == nil)
}


@Test @MainActor func markdownTakesMathOutBeforeParsing() {
  let d = MarkdownDocument.make("""
  # Collatz
  $T(n) = n/2$ when *n* is even, and $3n+1$ otherwise; it costs $5 and $6.

  $$
  T(n) = \\begin{cases} n/2 & n \\equiv 0 \\\\ 3n+1 & n \\equiv 1 \\end{cases}
  $$

  `$x$` stays code.
  ```
  $not math$
  ```
  """)
  guard d.blocks.count == 5 else { Issue.record("blocks: \(d.blocks)"); return }
  #expect(d.blocks[0] == .heading(1, [.text("Collatz", [])]))
  if case .paragraph(let inl) = d.blocks[1] {
    #expect(inl.first == .math("T(n) = n/2"))
    #expect(inl.contains(.math("3n+1")))
    #expect(inl.contains(.text("n", [.emphasis])))
    #expect(!inl.contains { if case .math(let t) = $0 { return t.contains("5") } else { return false } })
  } else { Issue.record("paragraph expected") }
  if case .math(let tex) = d.blocks[2] { #expect(tex.hasPrefix("T(n) = \\begin{cases}")) } else { Issue.record("display math expected") }
  #expect(d.blocks[3] == .paragraph([.code("$x$"), .text(" stays code.", [])]))
  #expect(d.blocks[4] == .code("$not math$\n", nil))
  // TeX that cannot be typeset is shown as written.
  #expect(MathImages.image("\\frac{1}{2}", size: 13, display: false) != nil)
  #expect(MathImages.image("\\frac{1}{", size: 13, display: false) == nil)
  // Commands the typesetter lacks are written with ones it has.
  #expect(MathImages.image("T(n) = \\begin{cases} n/2 & n \\equiv 0 \\pmod 2 \\\\ 3n+1 & n \\equiv 1 \\pmod{2} \\end{cases}",
                           size: 13, display: true) != nil)
  #expect(MathImages.image("a \\quad b \\bmod 2", size: 13, display: false) != nil)
}

/// The host registry uses Unix epoch milliseconds on every client.
@MainActor
@Test func hostRecentDatesUseUnixMilliseconds() async throws {
  let recents = try AppModel.decodeRecents([["path": "/project", "name": "Project", "openedAt": 1_700_000_000_125]])
  #expect(recents.count == 1)
  #expect(recents[0].openedAt.timeIntervalSince1970 == 1_700_000_000.125)
  #expect(!HostProjectError(code: "folder_not_empty").description.isEmpty)
}

/// The same constraints as the web (tests/fixtures/port-grid-contract.json, ui/web/tests/portGrid.ts).
@Test func portsFollowTheGridAsOnTheWeb() async throws {
  let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../..")
  let data = try Data(contentsOf: root.appendingPathComponent("tests/fixtures/port-grid-contract.json"))
  let contract = try #require(try JSONSerialization.jsonObject(with: data) as? [String: Any])
  for c in try #require(contract["snap"] as? [[String: Double]]) {
    #expect(abs(snapToGrid(CGFloat(c["v"]!), CGFloat(c["grid"]!)) - CGFloat(c["to"]!)) < 1e-9)
  }
  for c in try #require(contract["ports"] as? [[String: Any]]) {
    let name = "\(c["flow"]!) \(c["kind"]!) grid \(c["grid"]!) \(c["inputs"]!)/\(c["outputs"]!)"
    func ports(_ count: Int) -> [PortData] { (0..<count).map { PortData(name: "p\($0)") } }
    var d = CardNodeData(title: "x", kind: try #require(CardKind(rawValue: c["kind"] as! String)), summary: c["summary"] != nil ? "s" : nil,
                         inputs: ports(c["inputs"] as! Int), outputs: ports(c["outputs"] as! Int))
    d.collapsed = c["collapsed"] != nil
    let grid = c["grid"] as? Double
    d.portGrid = grid.map { CGFloat($0) }
    let flow: FlowDirection = c["flow"] as! String == "topToBottom" ? .topToBottom : .leftToRight
    let g = CardGeometry(d, flow: flow)
    #expect(g.size.width.isFinite && g.size.width > 0 && g.size.height.isFinite && g.size.height > 0)
    let edge = flow == .topToBottom ? g.size.width : g.size.height
    if d.kind == .merge {
      let r = g.outlineRect
      for z: CGFloat in [1, 0.25] {
        // What is drawn: every port is on it, and rounding takes only a little off the wide edge.
        let outline = g.outline(in: CGRect(x: r.minX * z, y: r.minY * z, width: r.width * z, height: r.height * z))
        let line = outline.strokedPath(StrokeStyle(lineWidth: 2 * z))
        for (i, output) in (0..<d.inputs.count).map({ ($0, false) }) + (0..<d.outputs.count).map({ ($0, true) }) {
          let p = g.port("p\(i)", output: output)
          #expect(line.contains(CGPoint(x: p.x * z, y: p.y * z)), "\(name) port \(i) output \(output) on the outline at \(z)")
        }
        let box = outline.boundingRect
        #expect((flow == .topToBottom ? box.width : box.height) >= edge * z - 8 * z, "\(name) keeps its width at \(z)")
      }
      // Many ports make it bigger, not flatter.
      #expect((flow == .topToBottom ? r.height : r.width) >= edge / 4 - 1e-9, "\(name) not flat")
      let icon = g.iconCenter(in: r)
      #expect(abs((flow == .topToBottom ? icon.x : icon.y) - (flow == .topToBottom ? r.midX : r.midY)) < 1e-5, "\(name) icon on the axis")
    }
    for (key, output) in [("in", false), ("out", true)] {
      let count = output ? d.outputs.count : d.inputs.count
      let list = (0..<count).map { i in
        let p = g.port("p\(i)", output: output)
        return flow == .topToBottom ? p.x : p.y
      }
      let apex = d.kind == .merge && output
      for (i, v) in list.enumerated() {
        guard let grid, let n = c["n"] as? Double else { continue }
        // A Card's corner is on a grid line, so this is its place on the canvas too.
        let lines = v * n / grid
        #expect(abs(lines - lines.rounded()) < 1e-5, "\(name) \(key) \(i) on grid/\(n)")
        #expect(v >= 0 && v <= edge, "\(name) on its edge")
        // A triangle's apex takes everything on that side; elsewhere marks never touch.
        if apex { #expect(abs(v - edge / 2) < 1e-5, "\(name) at the apex") }
        else if i > 0, c["collapsed"] == nil || flow == .topToBottom { #expect(v - list[i - 1] >= CardGeometry.minPortPitch, "\(name) apart") }
      }
    }
  }
}

/// Counts the requests to redraw the whole column.
private final class CountingRuler: LineNumberRuler {
  var wholeRedraws = 0
  override var needsDisplay: Bool {
    get { super.needsDisplay }
    set {
      if newValue { wholeRedraws += 1 }
      super.needsDisplay = newValue
    }
  }
}

/// AppKit may ask the line numbers for more than the column's height and then
/// count all of it as drawn, so a scroll alone would leave the rest blank.
@MainActor @Test func scrollingTheEditorRedrawsTheWholeLineNumberColumn() {
  let scroll = NSTextView.scrollableTextView()
  scroll.frame = NSRect(x: 0, y: 0, width: 400, height: 300)
  let tv = scroll.documentView as! NSTextView
  tv.string = (1...100).map { "line \($0)" }.joined(separator: "\n")
  scroll.hasVerticalRuler = true
  scroll.rulersVisible = true
  let ruler = CountingRuler(textView: tv)
  scroll.verticalRulerView = ruler
  tv.layoutManager?.ensureLayout(for: tv.textContainer!)
  ruler.wholeRedraws = 0

  scroll.contentView.scroll(to: NSPoint(x: 0, y: 100))
  #expect(ruler.wholeRedraws > 0)
}

@Test func theChatLeavesRoomForTheWorkbench() {
  let wanted = 480.0
  let room = ChatSplit.minWorkbench
  #expect(ChatSplit.chatWidth(wanted, in: room + wanted) == wanted)
  #expect(ChatSplit.chatWidth(wanted, in: room + wanted / 2) == wanted / 2)
  #expect(ChatSplit.chatWidth(wanted, in: room) == 0)
}

@MainActor @Test func draftImplementationListsWiresWithoutInventingPorts() throws {
  let dir = try SyncTests.project()
  defer { try? FileManager.default.removeItem(at: dir) }
  let session = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
  let document = try JSONValue.parse(fixtureProjectJSON(#"{"canvas":{"cards":[{"id":"plan"},{"id":"start"},{"id":"next"}],"connections":[{"from":{"card":"start","port":"out"},"to":{"card":"plan"}},{"from":{"card":"plan"},"to":{"card":"next"}}]}}"#))
  let snapshot = Tsugumi_RPC_ProjectSnapshotT()
  snapshot.projectJson = document.serialized(); snapshot.layout = []
  snapshot.definitions = ["plan", "start", "next"].map { id in
    let definition = Tsugumi_RPC_DefinitionT(); definition.id = id
    definition.cardJson = fixtureCardJSON(id == "start" ? .trigger : .draft)
    return definition
  }
  session.prepareLayout = { _, layout, _ in layout }
  session.receiveProjectSnapshot(snapshot, projectId: "fixture")
  let editor = CardEditorModel(session: session, cardKey: "plan")
  #expect(session.portData("plan").inputs.isEmpty && session.portData("plan").outputs.isEmpty)
  #expect(editor.draftEnds.isEmpty)
  #expect(session.nodeData("plan").symbol == CardKind.draft.symbol)
  let layoutWires = session.layoutScene([]).wires
  #expect(layoutWires.count == 2)
  let incoming = try #require(layoutWires.first ?? nil)
  let outgoing = try #require(layoutWires.last ?? nil)
  #expect(incoming.toPort == CardGeometry.newPort)
  #expect(outgoing.fromPort == CardGeometry.newPort)
  editor.implementAs("collect")
  #expect(editor.kind == .collect)
  #expect(editor.definition?["symbol"]?.string == "none")
  let input = try #require(editor.draftEnds.first { $0.side == "to" })
  let output = try #require(editor.draftEnds.first { $0.side == "from" })
  #expect(editor.rebindings.isEmpty)
  editor.rebindings[input.id] = "in"
  #expect(editor.rebindings[output.id] == nil, "The output still needs an explicit choice")
  #expect(session.kind("plan") == .draft)
  #expect(session.connections(in: []) == document["canvas"]?["connections"]?.array)
}


@MainActor @Test func notebookFailureNavigationKeepsUnsavedSource() throws {
  let dir = testTemporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: dir) }
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  try fixtureProjectJSON().write(to: dir.appendingPathComponent("tsugumi.json"), atomically: true, encoding: .utf8)
  let session = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
  let model = CardEditorModel(session: session, cardKey: "notebook")
  let source = "definitions/notebook/src/notebook.py"
  model.texts[source] = "unsaved notebook changes"
  model.openSource("src/notebook.py", line: 8)
  #expect(model.category == .source)
  #expect(model.shownFile == source && model.sourceLine == 8)
  #expect(model.texts[source] == "unsaved notebook changes")
  model.openSource("../notebook.py", line: 10)
  model.openSource("/tmp/notebook.py", line: 10)
  #expect(model.shownFile == source && model.sourceLine == 8)
  model.openFile("definitions/notebook/src/main.py")
  #expect(model.sourceLine == nil)
  model.openSource("src/notebook.py", line: 8)
  model.closeFile(source)
  #expect(model.sourceLine == nil)
}
