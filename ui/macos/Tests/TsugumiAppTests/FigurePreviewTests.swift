import AppKit
import Observation
import Foundation
import Testing
import TsugumiRPC
@testable import TsugumiApp

/// A chart preview on the canvas: which picture a frame holds while results
/// change, and how it may be shown (the Card setting `displayMode`). A picture
/// is never at full strength as a result it is not a picture of.
@MainActor
struct FigurePreviewTests {
  static func want(_ json: String, size: Int = 160) -> FigureWanted { FigureWanted(name: "plotly", json: json, width: size, height: size) }
  static func look(_ wanted: String, _ lineage: String = "r1#1", held: String?, heldLineage: String = "r1#1", image: Bool = true,
                   _ state: FigureState) -> FigureLook {
    FigureLook.of(wanted: want(wanted), lineage: lineage, picture: held.map { (want($0), heldLineage, image) }, state: state)
  }

  @Test func appearanceIsPartOfPreviewIdentity() {
    var light = Self.want("{}")
    light.colorScheme = "light"
    var dark = light
    dark.colorScheme = "dark"
    #expect(FigureLook.of(wanted: dark, lineage: "r#1", picture: (light, "r#1", true), state: FigureState())
      == .picture(dimmed: true, waiting: true))
    #expect(ModuleThumbnails.Key(name: "view", json: "{}", width: 160, height: 160, colorScheme: "light")
      != ModuleThumbnails.Key(name: "view", json: "{}", width: 160, height: 160, colorScheme: "dark"))
  }

  @Test func binaryInputsArePartOfPreviewIdentity() {
    let old = FigureWanted(name: "view", json: "{}", blobs: ["points": Data([0, 255])], width: 160, height: 160)
    let new = FigureWanted(name: "view", json: "{}", blobs: ["points": Data([0, 254])], width: 160, height: 160)
    #expect(old != new)
    #expect(FigureLook.of(wanted: new, lineage: "r1#1", picture: (old, "r1#1", true), state: FigureState())
      == .picture(dimmed: true, waiting: true))
    let a = ModuleThumbnails.Key(name: "view", json: "{}", blobs: old.blobs, width: 160, height: 160)
    let b = ModuleThumbnails.Key(name: "view", json: "{}", blobs: new.blobs, width: 160, height: 160)
    #expect(a != b)
  }

  @Test func aPictureOfOtherContentIsNeverShownAtFullStrengthInUpdate() {
    for running in [false, true] {
      #expect(Self.look("new", held: "old", FigureState(mode: .update, running: running)) == .picture(dimmed: true, waiting: true))
    }
    #expect(Self.look("new", "r2#1", held: "old", FigureState()) == .picture(dimmed: true, waiting: true))
    #expect(Self.look("{\"y\":[2]}", held: "{\"y\":[1]}", FigureState()) == .picture(dimmed: true, waiting: true), "a same-length update")
  }

  @Test func updateDimsTheResultWhileTheCardOrOneUpstreamRuns() {
    #expect(Self.look("old", held: "old", FigureState()) == .picture(dimmed: false, waiting: false))
    #expect(Self.look("old", held: "old", FigureState(running: true)) == .picture(dimmed: true, waiting: false))
    #expect(Self.look("old", held: "old", FigureState(stale: true)) == .picture(dimmed: true, waiting: false))
    #expect(Self.look("old", held: "old", FigureState(mode: .live, running: true, stale: true)) == .picture(dimmed: false, waiting: false))
  }

  @Test func liveKeepsThePreviousRevisionOnlyOfTheInvocationThatRuns() {
    let live = FigureState(mode: .live, running: true)
    #expect(Self.look("new", held: "old", live) == .picture(dimmed: false, waiting: false), "no gap between revisions")
    #expect(Self.look("new", held: "old", FigureState(mode: .live)) == .picture(dimmed: true, waiting: true), "the Card stopped")
    #expect(Self.look("new", "r2#1", held: "old", live) == .picture(dimmed: true, waiting: true), "another run")
    #expect(Self.look("new", "r1#2", held: "old", live) == .picture(dimmed: true, waiting: true), "another invocation")
  }

  @Test func nothingHeldAndFailures() {
    #expect(Self.look("new", held: nil, FigureState()) == .waiting)
    #expect(Self.look("new", held: "new", image: false, FigureState()) == .failed)
    #expect(Self.look("new", held: "old", image: false, FigureState()) == .waiting, "another result's failure says nothing of this one")
    #expect(FigureLook.of(wanted: Self.want("old", size: 320), lineage: "r1#1", picture: (Self.want("old"), "r1#1", true), state: FigureState())
      == .picture(dimmed: false, waiting: false), "the same content at another size is this result's picture")
  }

  @MainActor @Observable final class Maker {
    private(set) var asked: [String] = []
    private var open: [String: CheckedContinuation<ModuleThumbnails.Result?, Never>] = [:]

    func make(_ wanted: FigureWanted, _: CGRect?) async -> ModuleThumbnails.Result? {
      let json = wanted.json
      return await withTaskCancellationHandler {
        await withCheckedContinuation {
          open[json] = $0
          asked.append(json)
        }
      } onCancel: {
        Task { @MainActor in self.open.removeValue(forKey: json)?.resume(returning: nil) }
      }
    }

    func started(_ json: String) async throws {
      try await waitForModel("request for \(json)") { self.open[json] != nil }
    }

    func finish(_ json: String, failed: Bool = false) {
      open.removeValue(forKey: json)?.resume(returning: failed ? .failed("bad figure") : .image(NSImage(size: NSSize(width: 1, height: 1))))
    }
  }

  @Test func theLastPictureIsHeldUntilTheRevisionStillWantedIsReady() async throws {
    let maker = Maker()
    let preview = FigurePreview(make: maker.make)
    defer { preview.close() }
    preview.want(Self.want("old"), lineage: "r1#1", follow: false)
    try await maker.started("old")
    maker.finish("old")
    try await waitForModel { preview.picture?.wanted.json == "old" }

    for json in ["superseded", "new"] {
      preview.want(Self.want(json), lineage: "r2#1", follow: false)
      try await maker.started(json)
      #expect(preview.picture?.wanted.json == "old")
    }
    maker.finish("new")
    try await waitForModel { preview.picture?.wanted.json == "new" }
    #expect(preview.picture?.lineage == "r2#1")

    preview.close()
    preview.want(Self.want("new"), lineage: "r2#1", follow: false)
    try await maker.started("new")
    #expect(preview.picture == nil)
  }

  @Test func rapidRevisionsOfOneInvocationAdvanceWhenFollowed() async throws {
    let maker = Maker()
    let preview = FigurePreview(make: maker.make)
    defer { preview.close() }
    preview.want(Self.want("r1"), lineage: "r1#1", follow: true)
    try await maker.started("r1")
    for json in ["r2", "r3", "r4"] { preview.want(Self.want(json), lineage: "r1#1", follow: true) }
    maker.finish("r1")
    try await maker.started("r4")
    #expect(preview.picture?.wanted.json == "r1")
    #expect(maker.asked == ["r1", "r4"], "only the newest revision waits")

    preview.want(Self.want("r5"), lineage: "r1#1", follow: true)
    maker.finish("r4", failed: true)
    try await maker.started("r5")
    #expect(preview.picture?.wanted.json == "r1")

    // Another invocation starts even while the previous one is drawing.
    preview.want(Self.want("new"), lineage: "r1#2", follow: true)
    try await maker.started("new")
    maker.finish("new")
    try await waitForModel { preview.picture?.wanted.json == "new" }
    #expect(preview.picture?.lineage == "r1#2")
  }

  @Test func aCardUpstreamThatRunsMakesTheFigureStale() throws {
    func conn(_ from: String, _ to: String) -> JSONValue {
      .object([("from", .object([("card", .string(from)), ("port", .string("out"))])), ("to", .object([("card", .string(to)), ("port", .string("in"))]))])
    }
    // src -> mid -> plot, and side on its own; inside the Card in Card `box`
    // (which `feed` leads to): a -> b.
    let graphs: [[String]: [JSONValue]] = [
      []: [conn("src", "mid"), conn("mid", "plot"), conn("feed", "box")],
      ["box"]: [conn("a", "b")],
    ]
    func stale(_ card: CardKey, busy: Set<CardKey>) -> Bool {
      FigureState.upstreamBusy(card, connections: { graphs[$0] ?? [] }, busy: { busy.contains($0) })
    }
    #expect(stale("plot", busy: ["src"]), "any Card upstream, not only the one before it")
    #expect(stale("plot", busy: ["mid"]))
    #expect(!stale("plot", busy: ["plot"]), "the Card itself is `running`, not `stale`")
    #expect(!stale("plot", busy: ["side", "box/a"]))
    #expect(!stale("src", busy: ["mid", "plot"]), "downstream does not count")
    #expect(stale("box/b", busy: ["box/a"]))
    #expect(stale("box/b", busy: ["feed"]), "upstream of the Card in Card it is inside")
    #expect(!stale("box/a", busy: ["box/b"]))
  }
}
