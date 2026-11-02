import FlatBuffers
import SwiftUI
import TsugumiAgent
import TsugumiRPC

/// Bridges the Agent's tool calls to this window's authenticated RPC client.
/// The Agent edits documents only through core RPC, never through UI state.
struct RPCToolTransport: ToolTransport {
  let session: ProjectSession
  let replyNames: [UInt8: String]

  func call(payloadType: UInt8, build: (inout FlatBufferBuilder) throws -> Offset) async throws -> (replyType: String, table: FlatBuffers.Table) {
    guard let rpc = await session.rpc else { throw RPCError.disconnected }
    let (type, table) = try await rpc.callRaw(payloadType: payloadType, build: build)
    return (replyNames[type] ?? "Unknown", table)
  }
}

@MainActor
@Observable
final class AgentController {
  struct Item: Identifiable {
    let id = UUID()
    var role: String
    var text: String
    var tool: (name: String, ok: Bool?)?
  }

  private(set) var items: [Item] = []
  private(set) var busy = false
  private(set) var status: String?
  @ObservationIgnored private var agent: Agent?
  private(set) var provider: ProviderConfig

  init(session: ProjectSession) {
    provider = AgentSettings.current()
    guard let bfbs = RPCSchema.bfbs(), let schema = try? BFBS(data: bfbs), let catalog = try? ToolCatalog(schema: schema) else {
      status = "The RPC schema is unavailable; the Agent is disabled."
      return
    }
    var names: [UInt8: String] = [:]
    if let e = schema.enums.first(where: { $0.name == "Tsugumi.RPC.ReplyPayload" }) {
      for v in e.values { names[UInt8(v.value)] = v.name }
    }
    let storage = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("Tsugumi/agent/\(session.identity.id)/conversation.json")
    let agent = Agent(catalog: catalog, transport: RPCToolTransport(session: session, replyNames: names),
                      provider: provider, storage: storage)
    self.agent = agent
    Task {
      let conv = await agent.conversation
      for m in conv.messages {
        for b in m.content {
          switch b {
          case .text(let t): items.append(Item(role: m.role, text: t))
          case .toolUse(_, let n, let a): items.append(Item(role: "tool", text: a, tool: (n, nil)))
          case .toolResult(_, let c, let e): items.append(Item(role: "result", text: String(c.prefix(400)), tool: ("result", !e)))
          }
        }
      }
      // Resume work left unfinished by a disconnect or restart.
      await agent.resume { [weak self] e in Task { @MainActor in self?.handle(e) } }
    }
  }

  func send(_ text: String, context: [String]) {
    guard let agent, !text.isEmpty else { return }
    items.append(Item(role: "user", text: text))
    busy = true
    status = nil
    Task {
      await agent.setProvider(AgentSettings.current())
      await agent.send(text, context: context) { [weak self] e in Task { @MainActor in self?.handle(e) } }
    }
  }

  func cancel() {
    Task { await agent?.cancel() }
    busy = false
    status = "Cancelled. Runs already accepted keep running; use Cancel in the toolbar to stop a run."
  }

  func reset() {
    Task { await agent?.reset() }
    items.removeAll()
  }

  private func handle(_ e: AgentEvent) {
    switch e {
    case .text(let t):
      if let last = items.last, last.role == "assistant", last.tool == nil {
        items[items.count - 1].text += t
      } else {
        items.append(Item(role: "assistant", text: t))
      }
    case .toolStarted(_, let name, let args):
      items.append(Item(role: "tool", text: args, tool: (name, nil)))
    case .toolFinished(_, let name, let ok, let summary):
      if let i = items.lastIndex(where: { $0.tool?.name == name && $0.tool?.ok == nil }) {
        items[i].tool = (name, ok)
        items[i].text += "\n→ " + summary
      }
    case .compacted: status = "Older messages were summarized to stay within the context limit."
    case .retrying(let s): status = s
    case .error(let s):
      busy = false
      status = s
    case .finished:
      busy = false
    }
  }
}

struct AgentPanel: View {
  let session: ProjectSession
  @State private var controller: AgentController?
  @State private var input = ""
  @State private var includeSelection = true

  var body: some View {
    VStack(spacing: 4) {
      if let c = controller {
        ScrollViewReader { proxy in
          ScrollView {
            LazyVStack(alignment: .leading, spacing: 6) {
              ForEach(c.items) { item in row(item).id(item.id) }
            }
            .padding(8)
          }
          .onChange(of: c.items.count) { if let last = c.items.last { proxy.scrollTo(last.id) } }
        }
        if let s = c.status { Text(s).font(.caption).foregroundStyle(.secondary).padding(.horizontal, 8) }
        HStack {
          Toggle("Selected Cards", isOn: $includeSelection).toggleStyle(.checkbox).font(.caption)
          TextField("Ask the Agent (it edits through the same save and run RPC as the UI)", text: $input)
            .onSubmit(send)
          if c.busy {
            Button("Stop", action: c.cancel)
          } else {
            Button("Send", action: send).disabled(input.isEmpty || !session.connected)
          }
          Menu { Button("New conversation", action: c.reset) } label: { Image(systemName: "ellipsis.circle") }
            .menuStyle(.borderlessButton).fixedSize()
        }
        .padding(6)
        Text("Provider: \(c.provider.name) · \(c.provider.model)").font(.caption2).foregroundStyle(.secondary)
      } else {
        ProgressView()
      }
    }
    .task { if controller == nil { controller = AgentController(session: session) } }
  }

  private func send() {
    let ctx = includeSelection ? session.selection.sorted() : []
    controller?.send(input, context: ctx)
    input = ""
  }

  @ViewBuilder private func row(_ item: AgentController.Item) -> some View {
    switch item.role {
    case "user":
      Text(item.text).padding(6).background(.blue.opacity(0.12), in: RoundedRectangle(cornerRadius: 6))
    case "tool":
      HStack(alignment: .top) {
        Image(systemName: item.tool?.ok == nil ? "hourglass" : (item.tool!.ok! ? "checkmark.circle" : "xmark.circle"))
        VStack(alignment: .leading) {
          Text(item.tool?.name ?? "").font(.caption.bold())
          Text(item.text).font(.caption.monospaced()).lineLimit(6)
        }
      }
      .foregroundStyle(.secondary)
    case "result":
      EmptyView()
    default:
      Text(item.text).textSelection(.enabled)
    }
  }
}

enum AgentSettings {
  static func current() -> ProviderConfig {
    let d = UserDefaults.standard
    if d.string(forKey: "agent.provider") == "fixture" {
      return .localFixture(port: d.integer(forKey: "agent.fixturePort") == 0 ? 8765 : d.integer(forKey: "agent.fixturePort"))
    }
    var c = ProviderConfig.anthropic
    if let m = d.string(forKey: "agent.model"), !m.isEmpty { c.model = m }
    return c
  }
}

struct AgentSettingsView: View {
  @AppStorage("agent.provider") private var provider = "anthropic"
  @AppStorage("agent.model") private var model = ProviderConfig.anthropic.model
  @AppStorage("agent.fixturePort") private var port = 8765
  @State private var key = ""
  @State private var models: [String] = []
  @State private var message: String?

  var body: some View {
    Form {
      Picker("Model provider", selection: $provider) {
        Text("Anthropic API").tag("anthropic")
        Text("Local fixture (tests, no network)").tag("fixture")
      }
      if provider == "anthropic" {
        TextField("Model", text: $model)
        SecureField("API key (stored in the Keychain)", text: $key)
        HStack {
          Button("Save key") {
            do { try Keychain.save(key, account: ProviderConfig.anthropic.name); key = ""; message = "Saved to the Keychain." }
            catch { message = "\(error)" }
          }
          Button("List models") {
            Task {
              do { models = try await MessagesProvider(config: AgentSettings.current()).models() } catch { message = "\(error)" }
            }
          }
        }
        if !models.isEmpty { Text(models.joined(separator: ", ")).font(.caption) }
      } else {
        TextField("Fixture port", value: $port, format: .number)
        Text("Start it with: python3 tests/fixtures/model_fixture.py --port \(port)").font(.caption)
      }
      if let message { Text(message).font(.caption) }
      Text("The Agent is optional; every operation also works from the UI.").font(.caption).foregroundStyle(.secondary)
    }
    .padding()
  }
}
