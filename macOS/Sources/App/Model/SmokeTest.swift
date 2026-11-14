import AppKit
import Foundation
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
        report["displays"] = s.displays.mapValues { d -> String in
          switch d.value {
          case .text(let t): return String(t.prefix(200))
          case .scalar(let v, let l): return "\(l)=\(v)"
          default: return d.summary
          }
        }
        s.undo()
        await s.save()
        report["undo_save_rev"] = s.rev
        var ok = s.saveError == nil
        report["save_error"] = s.saveError ?? ""

        // Layout-only UI data (notes, groups, collapse, hidden output frame) passes the core schema.
        let first = s.cards(in: []).compactMap { $0["id"]?.string }.prefix(2).map { $0 }
        _ = s.addNote(in: [], at: CGPoint(x: 10, y: 400))
        _ = s.addGroup(Set(first), in: [])
        s.setCollapsed([first[0]], true)
        if let shown = s.resolution.keys.first(where: { s.displays[$0] != nil }) {
          s.closeDisplayFrame(shown)
        }
        await s.save()
        report["layout_extras"] = s.saveError ?? "ok"
        ok = ok && s.saveError == nil && s.confirmedLayout["groups"] != nil

        // A Draft's declared outputs can be wired before it is implemented.
        let dk = try await s.addCard(template: .draft, in: [], at: CGPoint(x: 0, y: 600), name: "smoke-draft")
        let draft = path(dk)[0]
        s.edit { s in
          var meta = s.definitionMeta[draft] ?? .object([])
          meta["outputs"] = .array([.object([("name", .string("summary")), ("description", .string("the result in words"))])])
          s.setDefinition(draft, card: s.definitions[draft] ?? .object([]), meta: meta)
        }
        await s.save()
        report["draft_outputs"] = s.resolution[dk]?.outputs.map(\.name) ?? []
        ok = ok && s.saveError == nil && s.resolution[dk]?.outputs.map(\.name) == ["summary"]

        // Another client saves while this window edits: both changes stay.
        if case .ready(let ep) = s.environment {
          let other = RPCClient(endpoint: ep.url)
          _ = try await other.connect(token: ep.token, clientId: UUID(), expectedProjectId: nil)
          let snap = try await other.inspect()
          s.edit { $0.setPosition(dk, CGPoint(x: 333, y: 333)) }
          var layout = try JSONValue.parse(snap.layoutJson)
          layout["cards"]?["start"] = .object([("x", .real(77)), ("y", .real(77))])
          _ = try? await other.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "smoke:other", changes: [Change.setLayout(layout.serialized())])
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
      NSApp.terminate(nil)
    }
  }
}
