// Platform adapter for the core-owned Agent. The C++ client owns framing,
// WebSocket/Unix transport and FlatBuffers validation; this process only
// translates the web host's UI requests and events.
import { ChildProcessWithoutNullStreams, spawn } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import readline from 'node:readline'
import { randomUUID } from 'node:crypto'
import type { Environments } from './env'
import { ToolCatalog, type ProviderConfig, type AgentEvent, type Conversation } from './agent'
import { BFBS } from '../src/shared/bfbs'

interface Ready {
  endpoint: string
  unix_socket: string
  user_token: string
  external_token: string
  server_id: string
  pid: number
  protocol: number
}

export interface AgentAccessRequest {
  id: string
  project_id: string
  conversation_id?: string
  access: string | number
  scope?: string | number
  reason?: string
  state?: string
}

export class CoreAgentError extends Error {
  constructor(readonly code: string, message: string, readonly requestId?: string) {
    super(message)
  }
}

class Connection {
  private process?: ChildProcessWithoutNullStreams
  private nextId = 0
  private waiting = new Map<string, { resolve: (v: any) => void; reject: (e: Error) => void; timer: ReturnType<typeof setTimeout> }>()
  private opening?: Promise<void>

  constructor(private host: CoreAgentHost, private external: boolean) {}

  private open() {
    return this.opening ??= this.start().catch((e) => { this.opening = undefined; throw e })
  }

  private async start() {
    const ready = await this.host.ready()
    const proc = spawn(this.host.clientBinary, ['--endpoint', `unix:${ready.unix_socket}`, '--schema', this.host.schema,
      '--subprotocol', 'tsugumi.agent.v1'], { stdio: ['pipe', 'pipe', 'pipe'] })
    this.process = proc
    let diagnostic = ''
    proc.stderr.on('data', (d) => { diagnostic = (diagnostic + String(d)).slice(-4000) })
    const fail = (reason: string) => {
      if (this.process !== proc) return
      this.process = undefined
      this.opening = undefined
      for (const p of this.waiting.values()) { clearTimeout(p.timer); p.reject(new Error(reason)) }
      this.waiting.clear()
    }
    proc.on('error', (e) => fail(e.message))
    proc.on('exit', () => fail(diagnostic || 'Agent connection closed'))
    readline.createInterface({ input: proc.stdout }).on('line', (line) => {
      let message: any
      try { message = JSON.parse(line) } catch { fail('Invalid response from the C++ Agent client'); proc.kill(); return }
      if (message.transport_error) { fail(message.transport_error.message ?? 'Agent transport failed'); proc.kill(); return }
      const p = this.waiting.get(String(message.request_id))
      if (!p) return
      this.waiting.delete(String(message.request_id))
      clearTimeout(p.timer)
      const reply = message.payload?.value
      if (message.payload_type !== 'Reply') return p.reject(new Error('Unexpected Agent message'))
      if (message.payload.value_type === 'Failure') {
        p.reject(new CoreAgentError(reply.code, reply.message, reply.request_id))
      } else if (message.payload.value_type === 'Result') {
        try { p.resolve(JSON.parse(reply.json)) } catch { p.reject(new Error('Invalid Agent result')) }
      } else p.resolve(reply)
    })
    await this.write('HelloRequest', { token: this.external ? ready.external_token : ready.user_token,
      client_id: `web-${this.external ? 'external' : 'user'}-${randomUUID()}`, protocol: 1 })
  }

  private write(method: string, value: unknown): Promise<any> {
    const requestId = ++this.nextId
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.waiting.delete(String(requestId))
        reject(new Error('Agent request timed out'))
      }, 120_000)
      timer.unref()
      this.waiting.set(String(requestId), { resolve, reject, timer })
      const data = JSON.stringify({ request_id: requestId, payload_type: 'Request', payload: { value_type: method, value } }) + '\n'
      if (!this.process) { clearTimeout(timer); this.waiting.delete(String(requestId)); reject(new Error('Agent connection is closed')); return }
      this.process.stdin.write(data)
    })
  }

  async call<T>(method: string, value: unknown = {}): Promise<T> {
    await this.open()
    return this.write(method, value)
  }
}

export class CoreAgentHost {
  readonly clientBinary: string
  readonly schema: string
  readonly state: string
  private readonly binary: string
  private readonly demoRoot: string
  private starting?: Promise<Ready>
  readonly user = new Connection(this, false)
  readonly external = new Connection(this, true)
  private projects = new Map<string, string>()
  private api?: ToolCatalog

  constructor(readonly support: string, here: string, readonly envs: Environments) {
    const repo = path.resolve(here, '..', '..')
    const build = process.env.TSUGUMI_BUILD_DIR ?? path.join(repo, 'build')
    const choose = (override: string | undefined, candidates: string[]) => override ?? candidates.find((p) => fs.existsSync(p)) ?? candidates[0]
    this.binary = choose(process.env.TSUGUMI_AGENTD, [path.join(repo, 'bin', 'tsugumi-agentd'), path.join(build, 'core', 'tsugumi-agentd')])
    this.clientBinary = choose(process.env.TSUGUMI_RPC_CLIENT, [path.join(repo, 'bin', 'tsugumi-rpc'), path.join(build, 'client', 'tsugumi-rpc')])
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
        if (r.protocol !== 1 || !r.unix_socket || !r.user_token || !r.external_token || !r.pid) return undefined
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
        '--listen', '127.0.0.1:0', '--demos-dir', this.demoRoot], { detached: true, stdio: ['ignore', log, log] })
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

  async project(folder: string): Promise<string> {
    const state = await this.envs.start(folder)
    if (state.state !== 'ready') throw new Error(state.state === 'failed' ? state.message : 'Project core is not ready')
    await this.user.call('RegisterProjectRequest', { project_id: state.projectId,
      endpoint: state.unix ? `unix:${state.unix}` : state.endpoint, token: state.token, title: path.basename(folder) })
    this.projects.set(folder, state.projectId)
    return state.projectId
  }

  projectFolder(resource: string): string | undefined {
    for (const [folder, id] of this.projects) if (resource === id || resource.startsWith(`folder:${id}:`)) return folder
    return undefined
  }

  async configure(config: ProviderConfig, conversationId?: string) {
    const token = config.token ? await config.token() : undefined
    const provider_json = JSON.stringify({ ...config, token: token?.value, accountId: token?.clientId, credential_id: token?.clientId })
    await this.user.call('ConfigureRequest', { provider_json })
    if (conversationId) await this.user.call('ConfigureRequest', { conversation_id: conversationId, provider_json })
  }

  async models(config: ProviderConfig): Promise<{ id: string; label: string }[]> {
    const token = config.token ? await config.token() : undefined
    return this.user.call('ModelsListRequest', { provider_json: JSON.stringify({ ...config, token: token?.value,
      accountId: token?.clientId, credential_id: token?.clientId }) })
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
    return { text: JSON.stringify(value), isError: false }
  }
}

export interface RemoteConversation { conversation: Conversation; busy: boolean; resumable?: boolean; continuable?: boolean; project_id?: string; event_seq?: number }

export class RemoteAgent {
  conversation: Conversation
  busy = false
  continuable = false
  private resumable = false
  private seq = 0
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
    this.seq = Number(result.event_seq ?? 0)
    await this.resumeInterrupted()
    return this
  }

  private adopt(result: RemoteConversation) {
    this.conversation = result.conversation
    this.busy = result.busy
    this.resumable = result.resumable === true
    this.continuable = result.continuable === true
  }

  private async resumeInterrupted() {
    if (!this.busy && this.resumable) {
      try { await this.host.user.call('ConversationActionRequest', { conversation_id: this.id, action: 'Resume' }) }
      catch (error) { if (!(error instanceof CoreAgentError) || error.code !== 'busy') throw error }
      this.busy = true
      this.resumable = false
      this.continuable = false
    }
  }

  async refresh() {
    this.adopt(await this.host.user.call<RemoteConversation>('ConversationGetRequest', { conversation_id: this.id }))
    await this.resumeInterrupted()
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
        const result = await this.host.user.call<{ events: { seq: number; event: AgentEvent }[]; next_seq: number; busy: boolean }>(
          'EventsRequest', { conversation_id: this.id, after_seq: this.seq })
        if (identity !== this.observationID) return
        if (result.events.some((e) => e.event.kind === 'finished')) await this.refresh()
        if (identity !== this.observationID) return
        for (const e of result.events) { this.seq = Math.max(this.seq, Number(e.seq)); this.listener?.(e.event) }
        this.seq = Math.max(this.seq, Number(result.next_seq ?? this.seq))
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
    this.seq = Number(result.event_seq ?? this.seq)
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
