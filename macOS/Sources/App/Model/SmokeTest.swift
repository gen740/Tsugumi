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
        guard let demo = model.demos.first(where: { $0.id == demoId }) else { throw CocoaError(.fileNoSuchFile) }
        let url = try model.copyDemo(demo)
        report["project"] = url.path
        let s = try model.session(for: url)
        while !s.connected {
          if case .failed(let m, _, _) = s.environment { throw RPCFailure(kind: .internalError, diagnostic: m) }
          try await Task.sleep(for: .milliseconds(100))
          if Date().timeIntervalSince(t0) > 900 { throw RPCError.timeout }
        }
        report["operable_s"] = Date().timeIntervalSince(t0)
        let before = s.rev
        s.edit { $0.setPosition("begin", CGPoint(x: 5, y: 5)) }
        await s.save()
        report["layout_save_rev"] = [before, s.rev]
        let trigger = s.cards(in: []).first { $0["definition"]?.string == "trigger" }?["id"]?.string ?? "begin"
        let t1 = Date()
        try await s.run(card: [trigger], propagate: true)
        try await Task.sleep(for: .milliseconds(200))
        while s.runActive {
          try await Task.sleep(for: .milliseconds(100))
          if Date().timeIntervalSince(t1) > 1800 { throw RPCError.timeout }
        }
        try await Task.sleep(for: .milliseconds(500))
        report["run_s"] = Date().timeIntervalSince(t1)
        report["run_state"] = String(describing: s.runStatus?.state ?? .idle)
        report["card_states"] = s.cardStates.mapValues { String(describing: $0.state) }
        report["displays"] = s.displays.mapValues { d -> String in
          switch d.value {
          case .text(let t): return String(t.prefix(200))
          case .scalar(let v, let l): return "\(l)=\(v)"
          case .lineChart(let series): return "chart \(series.map { "\($0.id):\($0.x.count)" })"
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
        if let shown = s.resolution.keys.first(where: { s.cardSetting($0, "canvasEnabled")?.bool == true }) {
          s.updateDisplayFrame(shown) { $0["hidden"] = .bool(true) }
        }
        await s.save()
        report["layout_extras"] = s.saveError ?? "ok"
        ok = ok && s.saveError == nil && s.confirmedLayout["groups"] != nil

        // A Draft with provisional outputs can be wired before it is implemented.
        let draft = s.createDefinition(name: "Smoke draft", template: .draft)
        s.edit { $0.putSource("definitions/\(draft)/_outputs.fbs", DraftPort.fbs([DraftPort(name: "summary", type: "string")], namespace: "draft_smoke")) }
        let dk = s.addCard(definition: draft, in: [], at: CGPoint(x: 0, y: 600))
        await s.save()
        report["draft_outputs"] = s.resolution[dk]?.outputs.map(\.name) ?? []
        ok = ok && s.saveError == nil && s.resolution[dk]?.outputs.map(\.name) == ["summary"]

        // Review scenario: another client saves while this draft is dirty. The
        // draft keeps its base revision, so saving it must conflict instead of
        // overwriting the other change.
        if case .ready(let ep) = s.environment {
          s.edit { $0.setPosition(dk, CGPoint(x: 333, y: 333)) }
          let base = s.draftBaseRev
          let other = RPCClient(endpoint: ep.url)
          _ = try await other.connect(token: ep.token, clientId: UUID(), expectedProjectId: nil)
          let snap = try await other.inspect()
          var layout = try JSONValue.parse(snap.layoutJson)
          layout["cards"]?["begin"] = .object([("x", .real(77)), ("y", .real(77))])
          _ = try await other.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "smoke:other", changes: [Change.setLayout(layout.serialized())])
          await other.close()
          for _ in 0..<50 where s.rev == base { try await Task.sleep(for: .milliseconds(100)) }
          let remoteRev = s.rev
          await s.save()
          let conflicted = s.saveError != nil && s.dirty && s.draftBaseRev == base && s.rev == remoteRev
          report["review_conflict"] = ["base": base, "remote": remoteRev, "conflicted": conflicted, "error": s.saveError ?? ""] as [String: Any]
          s.discardDraft()
          ok = ok && conflicted && !s.dirty && s.confirmedLayout["cards"]?["begin"]?["x"]?.double == 77
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
