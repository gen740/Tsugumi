import AppKit
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

  /// Pictures finished by hand, in the order they were asked for.
  @MainActor final class Maker {
    var asked: [String] = []
    private var open: [String: CheckedContinuation<ModuleThumbnails.Result?, Never>] = [:]
    func make(_ w: FigureWanted, _: CGRect?) async -> ModuleThumbnails.Result? {
      asked.append(w.json)
      return await withTaskCancellationHandler {
        await withCheckedContinuation { open[w.json] = $0 }
      } onCancel: {
        Task { @MainActor in self.open.removeValue(forKey: w.json)?.resume(returning: nil) }
      }
    }
    func finish(_ json: String, failed: Bool = false) async {
      open.removeValue(forKey: json)?.resume(returning: failed ? .failed("bad figure") : .image(NSImage(size: NSSize(width: 1, height: 1))))
      await settle()
    }
  }
  static func settle() async { for _ in 0..<20 { await Task.yield() } }

  @Test func theLastPictureIsHeldUntilTheNextIsReady() async {
    let maker = Maker()
    let preview = FigurePreview(make: maker.make)
    preview.want(Self.want("old"), lineage: "r1#1", follow: false)
    await Self.settle(); await maker.finish("old")
    #expect(preview.picture?.wanted.json == "old")
    preview.want(Self.want("new"), lineage: "r2#1", follow: false)
    await Self.settle()
    #expect(preview.picture?.wanted.json == "old", "held while the next is made")
    await maker.finish("new")
    #expect(preview.picture?.wanted.json == "new" && preview.picture?.lineage == "r2#1")
    // A picture of content no longer wanted is not shown when it arrives.
    preview.want(Self.want("a"), lineage: "r2#1", follow: false)
    await Self.settle()
    preview.want(Self.want("b"), lineage: "r2#1", follow: false)
    await Self.settle(); await maker.finish("a")
    #expect(preview.picture?.wanted.json == "new")
    await maker.finish("b")
    #expect(preview.picture?.wanted.json == "b")
    // A frame that left the screen asks again when it returns.
    preview.close()
    preview.want(Self.want("b"), lineage: "r2#1", follow: false)
    await Self.settle()
    #expect(maker.asked == ["old", "new", "a", "b", "b"])
  }

  @Test func rapidRevisionsOfOneInvocationAdvanceWhenFollowed() async {
    let maker = Maker()
    let preview = FigurePreview(make: maker.make)
    preview.want(Self.want("r1"), lineage: "r1#1", follow: true)
    await Self.settle()
    for r in ["r2", "r3", "r4"] { preview.want(Self.want(r), lineage: "r1#1", follow: true) }
    await Self.settle()
    #expect(maker.asked == ["r1"], "one picture at a time for this frame")
    await maker.finish("r1")
    #expect(preview.picture?.wanted.json == "r1")
    #expect(maker.asked == ["r1", "r4"], "the newest waits; those between are skipped")
    // A revision that cannot be drawn while a newer one waits leaves the last picture.
    preview.want(Self.want("r5"), lineage: "r1#1", follow: true)
    await maker.finish("r4", failed: true)
    #expect(preview.picture?.wanted.json == "r1")
    await maker.finish("r5")
    #expect(preview.picture?.wanted.json == "r5")
    // Another invocation does not wait behind the last one's picture.
    preview.want(Self.want("s1"), lineage: "r1#1", follow: true)
    await Self.settle()
    preview.want(Self.want("t1"), lineage: "r1#2", follow: true)
    await Self.settle()
    #expect(Array(maker.asked.suffix(2)) == ["s1", "t1"])
    await maker.finish("t1")
    #expect(preview.picture?.wanted.json == "t1" && preview.picture?.lineage == "r1#2")
  }

  @Test func updateShowsOnlyTheRevisionStillWanted() async {
    let maker = Maker()
    let preview = FigurePreview(make: maker.make)
    preview.want(Self.want("done"), lineage: "r1#1", follow: false)
    await Self.settle(); await maker.finish("done")
    for r in ["r1", "r2", "r3"] {
      preview.want(Self.want(r), lineage: "r2#1", follow: false)
      await Self.settle()
      #expect(preview.picture?.wanted.json == "done")
    }
    await maker.finish("r3")
    #expect(preview.picture?.wanted.json == "r3")
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
