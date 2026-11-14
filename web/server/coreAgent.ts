// Platform adapter for the core-owned services. The browser keeps the HTTP/SSE
// API; service credentials and binary socket connections stay in this host.
import { spawn } from 'node:child_process'
import { ProviderKeys } from './providerKeys'
import net from 'node:net'
import { CoreAgentError, encodeRequest, decodeReply, hostMethod } from './serviceWire'
export { CoreAgentError } from './serviceWire'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { randomUUID } from 'node:crypto'
import type { Environments, HostEvent } from './environment'
import { ToolCatalog, type ProviderConfig, type AgentEvent, type Conversation } from './agent'
import { BFBS } from '../src/shared/bfbs'

interface Ready {
  endpoint: string
  unix_socket: string
  host_socket: string
  user_token: string
  external_token: string
  server_id: string
  pid: number
  protocol: number
}

class Connection {
  private socket?: net.Socket
  private nextId = 0n
  private waiting = new Map<bigint, { resolve: (v: any) => void; reject: (e: Error) => void; timer: ReturnType<typeof setTimeout> }>()
  private opening?: Promise<void>
  // A plugin retry after the page answers must resume the approved call ID.
  private approvals = new Map<string, string>()

  constructor(private host: CoreAgentHost, private external: boolean, private hostContract = false) {}

  private open() {
    return this.opening ??= this.start().catch((e) => { this.opening = undefined; throw e })
  }

  private async start() {
    const ready = await this.host.ready()
    const socket = net.createConnection(this.hostContract ? ready.host_socket : ready.unix_socket)
    this.socket = socket
    let input = Buffer.alloc(0)
    const fail = (error: Error) => {
      if (this.socket !== socket) return
      this.socket = undefined
      this.opening = undefined
      for (const p of this.waiting.values()) { clearTimeout(p.timer); p.reject(error) }
      this.waiting.clear()
      socket.destroy()
    }
    socket.on('error', fail)
    socket.on('close', () => fail(new Error('Service connection closed')))
    socket.on('data', (data) => {
      try {
        input = Buffer.concat([input, data])
        while (input.length >= 4) {
          const length = input.readUInt32LE(0)
          if (length === 0 || length > 16 << 20) throw new Error('Invalid service frame length')
          if (input.length < length + 4) break
          const reply = decodeReply(input.subarray(4, 4 + length), this.hostContract)
          input = input.subarray(4 + length)
          if (reply.event) { this.host.event(reply.event); continue }
          if (reply.id > this.nextId || reply.id === 0n) throw new Error('Invalid service request ID')
          const p = this.waiting.get(reply.id)
          if (!p) continue // A late reply after its deadline.
          this.waiting.delete(reply.id)
          clearTimeout(p.timer)
          if (reply.error) p.reject(reply.error)
          else p.resolve(reply.value)
        }
      } catch (e) { fail(e as Error) }
    })
    await new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => { reject(new Error('Service connection timed out')); socket.destroy() }, 3000)
      socket.once('connect', () => { clearTimeout(timer); resolve() })
      socket.once('error', (e) => { clearTimeout(timer); reject(e) })
    })
    await this.write('HelloRequest', { token: this.external ? ready.external_token : ready.user_token,
      client_id: `web-${this.external ? 'external' : 'user'}-${randomUUID()}`, protocol: 1 })
  }

  private write(method: string, value: unknown): Promise<any> {
    if (!this.socket) return Promise.reject(new Error('Service connection is closed'))
    const requestId = ++this.nextId
    const data = encodeRequest(method, value, requestId, this.hostContract)
    const frame = Buffer.allocUnsafe(4 + data.length)
    frame.writeUInt32LE(data.length)
    frame.set(data, 4)
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.waiting.delete(requestId)
        reject(new Error('Service request timed out'))
      }, method === 'EnvironmentRequest' || method === 'HostRuntimeRequest' ? 3_600_000 : 120_000)
      timer.unref()
      this.waiting.set(requestId, { resolve, reject, timer })
      this.socket!.write(frame)
    })
  }

  async call<T>(method: string, value: unknown = {}): Promise<T> {
    await this.open()
    if (!this.external || method !== 'ToolCallRequest') return this.write(method, value)
    const call = { ...(value as Record<string, unknown>) }
    const key = JSON.stringify([call.conversation_id, call.project_id, call.name, call.arguments_json])
    call.op_id = this.approvals.get(key) ?? call.op_id
    try {
      const result = await this.write(method, call)
      this.approvals.delete(key)
      return result
    } catch (error) {
      if (error instanceof CoreAgentError && error.code === 'grant_required' && error.message.includes('operation_approval\n'))
        this.approvals.set(key, String(call.op_id))
      else if (error instanceof CoreAgentError && (error.code === 'permission_denied' || error.code === 'conflict')) this.approvals.delete(key)
      throw error
    }
  }
}

export class CoreAgentHost {
  readonly keyProfile = `web:keys:${randomUUID()}`
  readonly keys = new ProviderKeys((method, fields) => this.user.call(method, fields), this.keyProfile)
  private environment?: Promise<void>
  readonly schema: string
  readonly state: string
  private readonly binary: string
  private readonly demoRoot: string
  private starting?: Promise<Ready>
  private readonly agent = new Connection(this, false)
  private readonly host = new Connection(this, false, true)
  readonly user = {
    call: <T = any>(method: string, value: any = {}): Promise<T> =>
      (hostMethod(method) ? this.host : this.agent).call<T>(method, value),
  }
  readonly external = new Connection(this, true)
  private projects = new Map<string, string>()
  private hostListeners = new Set<(event: HostEvent) => void>()
  observeHost(fn: (event: HostEvent) => void) {
    this.hostListeners.add(fn)
    return () => this.hostListeners.delete(fn)
  }
  event(event: HostEvent) { for (const listener of this.hostListeners) listener(event) }
  private api?: ToolCatalog

  constructor(readonly support: string, here: string, readonly envs: Environments) {
    const repo = path.resolve(here, '..', '..')
    const build = process.env.TSUGUMI_BUILD_DIR ?? path.join(repo, 'build')
    const choose = (override: string | undefined, candidates: string[]) => override ?? candidates.find((p) => fs.existsSync(p)) ?? candidates[0]
    this.binary = choose(process.env.TSUGUMI_AGENTD, [path.join(repo, 'bin', 'tsugumi-agentd'), path.join(build, 'core', 'tsugumi-agentd')])
    this.schema = choose(process.env.TSUGUMI_AGENT_SCHEMA, [path.join(here, 'agent.bfbs'), path.join(build, 'generated', 'bfbs', 'agent.bfbs')])
    this.state = process.env.TSUGUMI_AGENT_STATE ?? (process.platform === 'darwin'
      ? path.join(process.env.TSUGUMI_SUPPORT_DIR ?? path.join(os.homedir(), 'Library', 'Application Support', 'Tsugumi'), 'agent-core')
      : path.join(support, 'agent-core'))
    this.demoRoot = path.join(repo, 'demos')
  }

  async ready(): Promise<Ready> {
    const file = path.join(this.state, 'agent.ready')
    const read = (): Ready | undefined => {
      try {
        const r = JSON.parse(fs.readFileSync(file, 'utf8')) as Ready
        if (r.protocol !== 1 || !r.unix_socket || !r.host_socket || !r.user_token || !r.external_token || !r.pid) return undefined
        process.kill(r.pid, 0)
        return r
      } catch { return undefined }
    }
    const existing = read()
    if (existing) return existing
    return this.starting ??= (async () => {
      fs.mkdirSync(this.state, { recursive: true, mode: 0o700 })
      const log = fs.openSync(path.join(this.state, 'agent.log'), 'a', 0o600)
      const socketDir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-agent-'))
      fs.chmodSync(socketDir, 0o700)
      const child = spawn(this.binary, ['--state-dir', this.state, '--unix', path.join(socketDir, 'agent.sock'),
        '--listen', '127.0.0.1:0', '--demos-dir', this.demoRoot, ...(this.envs.agentArguments ?? [])], { detached: true, stdio: ['ignore', log, log] })
      fs.closeSync(log)
      child.unref()
      let error: Error | undefined
      child.on('error', (e) => { error = e })
      const end = Date.now() + 15_000
      while (Date.now() < end) {
        if (error) throw error
        const next = read()
        if (next) return next
        if (child.exitCode !== null) throw new Error(`Core Agent exited during startup; see ${path.join(this.state, 'agent.log')}`)
        await new Promise((resolve) => setTimeout(resolve, 50))
      }
      throw new Error(`Core Agent did not become ready; see ${path.join(this.state, 'agent.log')}`)
    })().finally(() => { this.starting = undefined })
  }

  provisionEnvironment(): Promise<void> {
    return this.environment ??= this.keys.provisionEnvironment()
  }

  async getSettings(profile: string): Promise<{ revision: number; values: { key: string; value_json: string }[] }> {
    return this.user.call('SettingsGetRequest', { profile })
  }

  async project(folder: string): Promise<string> {
    const state = await this.envs.start(folder)
    if (state.state !== 'ready') throw new Error(state.state === 'failed' ? state.message : 'Project core is not ready')
    this.projects.set(folder, state.projectId)
    return state.projectId
  }

  projectFolder(resource: string): string | undefined {
    for (const [folder, id] of this.projects) if (resource === id || resource.startsWith(`folder:${id}:`)) return folder
    return undefined
  }

  async provider(config: ProviderConfig) {
    const keyProfile = config.auth === 'apiKey'
      ? await this.keys.provision(config.api === 'openAIResponses' ? 'openai' : 'anthropic') : undefined
    return { profile: config.profile ?? 'desktop', account_profile: config.accountProfile, key_profile: keyProfile,
      provider_id: config.auth === 'chatgpt' ? 'chatgpt' : config.auth === 'none' ? 'fixture' : config.api === 'openAIResponses' ? 'openai' : 'anthropic', model: config.model, use_choices: !!config.usesChoices }
  }

  async configure(config: ProviderConfig, conversationId: string) {
    const provider = await this.provider(config)
    await this.user.call('ConfigureRequest', { conversation_id: conversationId, provider })
  }

  async storedModels(id: string): Promise<{ id: string; name: string }[]> {
    return this.user.call('ModelsListRequest', { provider_id: id })
  }

  async models(config: ProviderConfig): Promise<{ id: string; name: string }[]> {
    const id = config.auth === 'chatgpt' ? 'chatgpt' : config.auth === 'none' ? 'fixture' : config.api === 'openAIResponses' ? 'openai' : 'anthropic'
    const selection = await this.provider(config)
    return this.user.call('ModelsListRequest', { provider_id: id, refresh: true, profile: selection.profile, account_profile: selection.account_profile, key_profile: selection.key_profile })
  }

  async summarize(conversationId: string, config: ProviderConfig): Promise<{ text: string; usage: { inputTokens: number; outputTokens: number } }> {
    return this.user.call('ConversationSummarizeRequest', { conversation_id: conversationId, provider: await this.provider(config) })
  }

  private apiCatalog(): ToolCatalog {
    if (!this.api) {
      const schema = new BFBS(new Uint8Array(fs.readFileSync(this.schema)))
      const requests = schema.enums.find((e) => e.name === 'Tsugumi.Agent.RequestPayload')?.values
        .filter((v) => v.value !== 0 && v.unionType && v.name !== 'HelloRequest').map((v) => v.name) ?? []
      this.api = new ToolCatalog(schema, requests, 'Tsugumi.Agent')
    }
    return this.api
  }

  async tools(): Promise<Record<string, unknown>[]> {
    const project = await this.external.call<Record<string, unknown>[]>('ToolCatalogRequest')
    const agent = this.apiCatalog().tools.map((t) => ({ name: `agent_${t.name}`, description: t.description,
      inputSchema: t.inputSchema }))
    return [...project, ...agent]
  }

  async externalAPI(name: string, args: unknown) {
    const tool = this.apiCatalog().tools.find((t) => `agent_${t.name}` === name)
    if (!tool) throw new Error(`Unknown Agent operation: ${name}`)
    const request = this.apiCatalog().schema.objects[tool.object].name.split('.').at(-1)!
    const value = await this.external.call(request, args ?? {})
    return { text: JSON.stringify(value, (_, v) => typeof v === 'bigint' ? v.toString() : v), isError: false }
  }
}

export interface InterruptedOperation { kind: string; id?: string; name?: string; arguments?: string }

export interface RemoteConversation { interruptedOperation?: InterruptedOperation; conversation: Conversation; busy: boolean; resumable?: boolean; continuable?: boolean; project_id?: string; event_seq?: bigint }

export class RemoteAgent {
  conversation: Conversation
  busy = false
  continuable = false
  resumable = false
  interruptedOperation?: InterruptedOperation
  private seq = 0n
  private observing = false
  private observationID = 0
  private listener?: (e: AgentEvent) => void
  private transition: Promise<void> = Promise.resolve()
  private changing = 0

  constructor(readonly host: CoreAgentHost, readonly id: string, readonly project?: string, readonly role = 'worker') {
    this.conversation = { id, messages: [], archive: [], tools: [], usage: { inputTokens: 0, outputTokens: 0 }, updatedAt: Date.now() }
  }

  async open() {
    const project = this.project ? await this.host.project(this.project) : undefined
    const result = await this.host.user.call<RemoteConversation>('ConversationCreateRequest', {
      conversation_id: this.id, project_id: project, role: this.role,
    })
    this.adopt(result)
    this.seq = result.event_seq ?? 0n
    return this
  }

  private adopt(result: RemoteConversation) {
    this.conversation = result.conversation
    this.busy = result.busy
    this.resumable = result.resumable === true
    this.continuable = result.continuable === true
    this.interruptedOperation = result.interruptedOperation
  }

  async refresh() {
    this.adopt(await this.host.user.call<RemoteConversation>('ConversationGetRequest', { conversation_id: this.id }))
  }

  observe(listener: (e: AgentEvent) => void) {
    this.listener = listener
    if (this.observing || this.changing) return
    this.observing = true
    void this.poll(++this.observationID)
  }

  private async poll(identity: number) {
    try {
      while (this.observing && identity === this.observationID) {
        const result = await this.host.user.call<{ events: { seq: bigint; event: AgentEvent }[]; next_seq: bigint; busy: boolean }>(
          'EventsRequest', { conversation_id: this.id, after_seq: this.seq })
        if (identity !== this.observationID) return
        if (result.events.some((e) => e.event.kind === 'finished')) await this.refresh()
        if (identity !== this.observationID) return
        for (const e of result.events) { this.seq = e.seq > this.seq ? e.seq : this.seq; this.listener?.(e.event) }
        this.seq = result.next_seq > this.seq ? result.next_seq : this.seq
        const wasBusy = this.busy
        this.busy = result.busy
        if (wasBusy && !this.busy) await this.refresh()
        await new Promise<void>((resolve) => { const t = setTimeout(resolve, this.busy ? 80 : 500); t.unref() })
      }
    } catch (e) {
      if (identity !== this.observationID) return
      this.observing = false
      this.listener?.({ kind: 'error', message: (e as Error).message })
    }
  }

  private change(run: () => Promise<void>) {
    this.changing++
    this.close()
    const pending = this.transition.catch(() => {}).then(run)
    this.transition = pending
    return pending.finally(() => {
      this.changing--
      if (!this.changing && this.listener) this.observe(this.listener)
    })
  }

  private async beginTurn() {
    const result = await this.host.user.call<RemoteConversation>('ConversationGetRequest', { conversation_id: this.id })
    this.adopt(result)
    this.seq = result.event_seq ?? this.seq
  }

  send(text: string, listener: (e: AgentEvent) => void, turnId = randomUUID(), selections: Record<string, unknown> = {}) {
    this.listener = listener
    return this.change(async () => {
      await this.beginTurn()
      await this.host.user.call('ConversationSendRequest', { conversation_id: this.id, turn_id: turnId, text, ...selections })
      this.busy = true
    })
  }

  action(action: string, listener: (e: AgentEvent) => void) {
    this.listener = listener
    return this.change(async () => {
      await this.beginTurn()
      await this.host.user.call('ConversationActionRequest', { conversation_id: this.id, action })
      await this.refresh()
    })
  }

  cancel() {
    return this.change(async () => {
      await this.host.user.call('ConversationCancelRequest', { conversation_id: this.id })
      do {
        await this.beginTurn()
        if (this.busy) await new Promise((resolve) => setTimeout(resolve, 20))
      } while (this.busy)
    })
  }
  close() { this.observing = false; this.observationID++ }
}
