import TsugumiViewPresentation
import AppKit
import Foundation
import FlatBuffers
import TsugumiRPC

/// Headless end-to-end check of the app's own path (demo copy -> environment
/// -> RPC session -> edit/save -> Run -> displays -> undo as a new save).
/// Enabled with TSUGUMI_SMOKE_DEMO=<id>; the JSON report goes to
/// TSUGUMI_SMOKE_OUT and the app quits afterwards.
@MainActor
enum SmokeTest {
  static func runIfRequested(_ model: AppModel) {
    let env = ProcessInfo.processInfo.environment
    guard let demoId = env["TSUGUMI_SMOKE_DEMO"], let out = env["TSUGUMI_SMOKE_OUT"] else { return }
    model.smokeMode = true
    Task {
      var report: [String: Any] = ["demo": demoId]
      let t0 = Date()
      do {
        await model.refreshHome()
        guard let demo = model.demos.first(where: { $0.id == demoId }) else { throw CocoaError(.fileNoSuchFile) }
        let url = try await model.copyDemo(demo)
        report["project"] = url.path
        let s = try model.session(for: url)
        while !s.connected {
          if case .failed(let m, _, _) = s.environment { throw RPCFailure(kind: .internalError, diagnostic: m) }
          try await Task.sleep(for: .milliseconds(100))
          if Date().timeIntervalSince(t0) > 900 { throw RPCError.timeout }
        }
        report["operable_s"] = Date().timeIntervalSince(t0)
        let before = s.rev
        s.edit { $0.setPosition("start", CGPoint(x: 5, y: 5)) }
        await s.save()
        report["layout_save_rev"] = [before, s.rev]
        let trigger = s.cards(in: []).compactMap { $0["id"]?.string }.first { s.kind(key([$0])) == .trigger } ?? "start"
        let t1 = Date()
        try await s.run(card: [trigger], propagate: true)
        try await Task.sleep(for: .milliseconds(200))
        while s.runActive {
          try await Task.sleep(for: .milliseconds(100))
          if Date().timeIntervalSince(t1) > 1800 { throw RPCError.timeout }
        }
        // When the UI saw the end vs the core's own clock (accepted -> ended).
        report["run_ui_s"] = Date().timeIntervalSince(t1)
        if let r = s.runStatus, let a = r.acceptedAt, let e = r.endedAt { report["run_core_s"] = Double(e - a) / 1000 }
        report["notifications"] = [s.notificationCount, s.notificationBatches]
        try await Task.sleep(for: .milliseconds(500))
        report["run_s"] = Date().timeIntervalSince(t1)
        report["run_state"] = String(describing: s.runStatus?.state ?? .idle)
        // A second run: builds and workers are ready, so this is the steady state.
        if env["TSUGUMI_SMOKE_RERUN"] == "1" {
          let n0 = s.notificationCount, b0 = s.notificationBatches
          let t2 = Date()
          try await s.run(card: [trigger], propagate: true)
          try await Task.sleep(for: .milliseconds(200))
          while s.runActive { try await Task.sleep(for: .milliseconds(50)) }
          report["rerun_ui_s"] = Date().timeIntervalSince(t2)
          if let r = s.runStatus, let a = r.acceptedAt, let e = r.endedAt { report["rerun_core_s"] = Double(e - a) / 1000 }
          report["rerun_notifications"] = [s.notificationCount - n0, s.notificationBatches - b0]
        }
        report["card_states"] = s.cardStates.mapValues { String(describing: $0.state) }
        report["views"] = Dictionary(grouping: s.cardViews.openings, by: { key($0.card) })
          .mapValues { $0.map { "\($0.name):\($0.type)" }.sorted() }
        await s.undo()
        await s.save()
        report["undo_save_rev"] = s.rev
        var ok = s.saveError == nil
        report["save_error"] = s.saveError ?? ""

        // Layout-only UI data (notes, groups, collapse) passes the core schema.
        let first = s.cards(in: []).compactMap { $0["id"]?.string }.prefix(2).map { $0 }
        _ = try await s.addNote(in: [], at: CGPoint(x: 10, y: 400))
        _ = try await s.addGroup(Set(first), in: [])
        s.setCollapsed([first[0]], true)
        await s.save()
        report["layout_extras"] = s.saveError ?? "ok"
        ok = ok && s.saveError == nil && s.confirmedLayout["groups"] != nil

        // Draft wires attach directly to the Card.
        let dk = try await s.addCard(template: .draft, in: [], at: CGPoint(x: 0, y: 600), name: "smoke-draft")
        let draft = path(dk)[0]
        let next = try await s.addCard(template: .draft, in: [], at: CGPoint(x: 300, y: 600), name: "smoke-next")
        try await s.connect(from: (dk, CardGeometry.newPort), to: (next, CardGeometry.newPort), in: [])
        await s.save()
        report["draft_outputs"] = s.resolution[dk]?.outputs.map(\.name) ?? []
        let draftWire = s.connections(in: []).contains { c in
          c["from"]?["card"]?.string == draft && c["from"]?["port"]?.string == ""
            && c["to"]?["card"]?.string == path(next)[0] && c["to"]?["port"]?.string == ""
        }
        report["draft_wire"] = draftWire
        ok = ok && s.saveError == nil && s.resolution[dk]?.outputs.isEmpty == true
          && s.resolution[next]?.inputs.isEmpty == true && draftWire

        // Another client saves while this window edits: both changes stay.
        if case .ready(let ep) = s.environment {
          let other = RPCClient(endpoint: ep.url)
          _ = try await other.connect(token: ep.token, expectedProjectId: nil, client: .macos)
          let snap = try await other.inspect()
          s.edit { $0.setPosition(dk, CGPoint(x: 333, y: 333)) }
          var layout = Tsugumi_Layout_LayoutT()
          if !snap.layout.isEmpty {
            var buffer = ByteBuffer(bytes: snap.layout)
            let stored: Tsugumi_Layout_Layout = try getCheckedRoot(byteBuffer: &buffer)
            layout = stored.unpack()
          }
          let start = layout.cards.compactMap { $0 }.first { $0.key == "start" }
            ?? Tsugumi_Layout_CardPlaceT()
          start.key = "start"; start.x = 77; start.y = 77
          layout.cards.removeAll { $0?.key == start.key }
          layout.cards.append(start)
          var builder = FlatBufferBuilder(initialSize: 256)
          let offset = Tsugumi_Layout_Layout.pack(&builder, obj: &layout)
          Tsugumi_Layout_Layout.finish(&builder, end: offset)
          _ = try? await other.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "smoke:other",
                                    changes: [Change.setLayout(builder.sizedByteArray)])
          await other.close()
          await s.save()
          for _ in 0..<50 where s.confirmedLayout["cards"]?["start"]?["x"]?.double != 77 { try await Task.sleep(for: .milliseconds(100)) }
          let merged = s.saveError == nil && !s.dirty && s.confirmedLayout["cards"]?[dk]?["x"]?.double == 333
          report["concurrent_edit"] = ["merged": merged, "error": s.saveError ?? ""] as [String: Any]
          ok = ok && merged
        }
        report["ok"] = ok
      } catch {
        report["ok"] = false
        report["error"] = "\(error)"
      }
      if let data = try? JSONSerialization.data(withJSONObject: report, options: [.prettyPrinted, .sortedKeys]) {
        try? data.write(to: URL(fileURLWithPath: out))
      }
      requestQuit()
    }
  }
}
