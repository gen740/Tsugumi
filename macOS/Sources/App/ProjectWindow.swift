import SwiftUI
import TsugumiLauncher
import TsugumiRPC
import TsugumiRenderer

struct ProjectWindow: View {
  @Environment(AppModel.self) private var model
  let url: URL
  @State private var session: ProjectSession?
  @State private var error: String?

  var body: some View {
    Group {
      if let session {
        ProjectView(session: session).focusedSceneValue(\.projectSession, session)
      } else if let error {
        ContentUnavailableView("Cannot open the project", systemImage: "exclamationmark.triangle", description: Text(error))
      } else {
        ProgressView()
      }
    }
    .task {
      do { session = try model.session(for: url) } catch { self.error = "\(error)" }
    }
  }
}

enum BottomTab: String, CaseIterable, Identifiable {
  case runs = "Runs & Logs", diagnostics = "Diagnostics", agent = "Agent"
  var id: String { rawValue }
}

struct ProjectView: View {
  @Bindable var session: ProjectSession
  @State private var bottom: BottomTab = .runs
  @State private var showBottom = true
  @State private var runError: String?

  var body: some View {
    VStack(spacing: 0) {
      EnvironmentBanner(session: session)
      HSplitView {
        DefinitionPalette(session: session).frame(minWidth: 180, idealWidth: 210, maxWidth: 280)
        VSplitView {
          CanvasView(session: session).frame(minHeight: 300)
          if showBottom {
            VStack(spacing: 0) {
              Picker("", selection: $bottom) {
                ForEach(BottomTab.allCases) { Text($0.rawValue).tag($0) }
              }
              .pickerStyle(.segmented).labelsHidden().padding(6)
              switch bottom {
              case .runs: RunPanel(session: session)
              case .diagnostics: DiagnosticsView(session: session)
              case .agent: AgentPanel(session: session)
              }
            }
            .frame(minHeight: 180, idealHeight: 260)
          }
        }
        InspectorView(session: session).frame(minWidth: 280, idealWidth: 320, maxWidth: 460)
      }
    }
    .toolbar { toolbar }
    .navigationTitle(session.project["name"]?.string ?? session.identity.url.lastPathComponent)
    .alert("Cannot run", isPresented: Binding(get: { runError != nil }, set: { if !$0 { runError = nil } })) {
      Button("OK") { runError = nil }
    } message: { Text(runError ?? "") }
  }

  @ToolbarContentBuilder private var toolbar: some ToolbarContent {
    ToolbarItemGroup(placement: .navigation) {
      Button { session.undo() } label: { Label("Undo", systemImage: "arrow.uturn.backward") }
        .disabled(session.undoStack.isEmpty)
      Button { session.redo() } label: { Label("Redo", systemImage: "arrow.uturn.forward") }
        .disabled(session.redoStack.isEmpty)
      Button { Task { await session.save() } } label: { Label("Save", systemImage: "square.and.arrow.down") }
        .disabled(!session.dirty || !session.connected)
        .help(session.dirty ? "Commit your edits (⌘S)" : "No unsaved edits")
    }
    ToolbarItemGroup(placement: .primaryAction) {
      Button { runAll() } label: { Label("Run", systemImage: "play.fill") }
        .disabled(!session.connected || session.runActive)
        .help("Run the whole graph from its start Cards")
      Menu {
        Button("Run selected and downstream") { runSelected(propagate: true) }
        Button("Run selected only (last saved inputs)") { runSelected(propagate: false) }
      } label: { Label("Run Selection", systemImage: "play") }
        .disabled(!session.connected || session.runActive || session.selection.count != 1)
      Button { Task { await session.cancelRun() } } label: { Label("Cancel", systemImage: "stop.fill") }
        .disabled(!session.runActive)
      Toggle(isOn: $showBottom) { Label("Panels", systemImage: "rectangle.bottomhalf.inset.filled") }
    }
  }

  private func runAll() {
    // Whole run: start from the trigger of the root graph.
    let trigger = session.cards(in: []).first { c in
      c["definition"]?.string.flatMap { session.definitions[$0]?["template"]?.string } == "trigger"
    }
    guard let id = trigger?["id"]?.string else {
      runError = "Add a trigger Card to run the whole graph, or select a Card."
      return
    }
    Task {
      do { try await session.run(card: [id], propagate: true) } catch { runError = "\(error)" }
    }
  }

  private func runSelected(propagate: Bool) {
    guard let k = session.selection.first else { return }
    Task {
      do { try await session.run(card: path(k), propagate: propagate) } catch { runError = "\(error)" }
    }
  }
}

struct EnvironmentBanner: View {
  let session: ProjectSession

  var body: some View {
    HStack(spacing: 8) {
      switch session.environment {
      case .ready:
        Circle().fill(session.connected ? .green : .orange).frame(width: 8, height: 8)
        Text(session.connected ? "Environment ready · revision \(session.rev)" : "Connecting…")
      case .preparing(let s), .starting(let s):
        ProgressView().controlSize(.small)
        Text(s)
      case .reconnecting:
        ProgressView().controlSize(.small)
        Text("Reconnecting… (your edits are kept)")
      case .stopped:
        Circle().fill(.gray).frame(width: 8, height: 8)
        Text("Environment stopped")
        Button("Start") { session.start() }
      case .failed(let m, let recovery, let log):
        Image(systemName: "xmark.octagon.fill").foregroundStyle(.red)
        VStack(alignment: .leading) {
          Text(m).bold()
          Text(recovery).font(.caption)
          if !log.isEmpty { Text(log.suffix(300)).font(.caption.monospaced()).lineLimit(3) }
        }
        Button("Retry") { session.start() }
      }
      if let m = session.connectionMessage, !session.connected { Text(m).font(.caption).foregroundStyle(.secondary).lineLimit(1) }
      Spacer()
      if session.dirty { Text("Unsaved edits").font(.caption).padding(.horizontal, 6).background(.yellow.opacity(0.3), in: Capsule()) }
      if let e = session.saveError {
        Text(e).font(.caption).foregroundStyle(.red).lineLimit(2)
        Button("Discard my edits") { session.discardDraft() }
        Button("Reload from disk") { Task { await session.reloadExternal() } }
      }
      if let s = session.runStatus?.state {
        Text("Run: \(String(describing: s))").font(.caption.monospaced())
      }
      Menu("Environment") {
        Button("Stop environment") { session.stopEnvironment() }
        Button("Delete environment data (keeps documents)…") {
          Task {
            try? await session.environments.deleteData(session.identity)
          }
        }
      }
      .menuStyle(.borderlessButton).fixedSize()
    }
    .font(.callout)
    .padding(.horizontal, 10).padding(.vertical, 5)
    .background(.bar)
  }
}

struct DefinitionPalette: View {
  @Bindable var session: ProjectSession

  var body: some View {
    List {
      Section("Add a Card") {
        ForEach(session.definitions.keys.sorted(), id: \.self) { id in
          Button {
            add(id)
          } label: {
            VStack(alignment: .leading) {
              Text(session.definitions[id]?["name"]?.string ?? id)
              Text(session.definitions[id]?["template"]?.string ?? session.definitions[id]?["language"]?.string ?? "draft")
                .font(.caption).foregroundStyle(.secondary)
            }
          }
          .buttonStyle(.plain)
        }
      }
    }
  }

  private func add(_ def: String) {
    let existing = Set(session.cards(in: session.scope).compactMap { $0["id"]?.string })
    var n = 1
    var id = def
    while existing.contains(id) { n += 1; id = "\(def)-\(n)" }
    let k = key(session.scope + [id])
    session.edit { s in
      s.updateGraph(s.scope) { g in
        var cards = g["cards"]?.array ?? []
        cards.append(.object([("id", .string(id)), ("definition", .string(def))]))
        g["cards"] = .array(cards)
      }
      let cam = s.cameras[key(s.scope)] ?? (40, 40, 1)
      s.setPosition(k, CGPoint(x: (200 - cam.x) / cam.zoom, y: (120 - cam.y) / cam.zoom))
    }
    session.selection = [k]
  }
}

struct DisplayPanel: View {
  let display: DisplaySnapshot?
  let title: String

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      Text(title).font(.caption.bold()).foregroundStyle(.secondary)
      switch display?.value {
      case .text(let t)?:
        ScrollView { Text(t).font(.system(size: 11, design: .monospaced)).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading) }
      case .scalar(let v, let label)?:
        VStack { Text(label).font(.caption); Text(v, format: .number.precision(.significantDigits(6))).font(.title.monospacedDigit()) }
          .frame(maxWidth: .infinity, maxHeight: .infinity)
      case .lineChart(let series)?:
        DawnLineChart(series: series.map { ChartSeries(id: $0.id, label: $0.label, x: $0.x, y: $0.y) })
          .clipShape(RoundedRectangle(cornerRadius: 4))
        HStack {
          ForEach(series, id: \.id) { Text($0.label).font(.caption2) }
        }
      case nil:
        Text("No display yet").font(.caption).foregroundStyle(.secondary).frame(maxWidth: .infinity, maxHeight: .infinity)
      }
    }
    .padding(8)
    .background(RoundedRectangle(cornerRadius: 8).fill(Color(nsColor: .windowBackgroundColor)))
    .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(.secondary.opacity(0.3)))
  }
}

struct DiagnosticsView: View {
  let session: ProjectSession

  var body: some View {
    List(session.diagnostics) { d in
      HStack(alignment: .top) {
        Image(systemName: d.error ? "xmark.circle.fill" : "exclamationmark.triangle.fill")
          .foregroundStyle(d.error ? .red : .yellow)
        VStack(alignment: .leading) {
          Text(d.message)
          Text([d.card.map { $0.joined(separator: "/") }, d.path, d.code].compactMap { $0 }.joined(separator: " · "))
            .font(.caption).foregroundStyle(.secondary)
        }
      }
      .onTapGesture { if let c = d.card { session.selection = [key(c)] } }
    }
    .overlay { if session.diagnostics.isEmpty { Text("No diagnostics").foregroundStyle(.secondary) } }
  }
}
