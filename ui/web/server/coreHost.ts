import type { ChatRow } from '../shared/chatPresentation'
// Platform adapter for the core-owned services. The browser keeps the HTTP/SSE
// API; service credentials and binary socket connections stay in this host.
import { spawn } from 'node:child_process'
import { ProviderKeys } from './providerKeys'
import net from 'node:net'
import { CoreHostError, encodeRequest, decodeReply, hostMethod } from './serviceWire'
export { CoreHostError } from './serviceWire'
import fs from 'node:fs'
import { L } from './i18n'
import { runtimeRoot } from './paths'
import os from 'node:os'
import path from 'node:path'
import { randomUUID } from 'node:crypto'
import type { Environments, HostEvent } from './environment'
import { ToolCatalog, type ProviderConfig, type AgentEvent, type Conversation } from './agent'
import { BFBS } from '../shared/bfbs'

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
  private waiting = new Map<bigint, { resolve: (v: any) => void; reject: (e: Error) => void }>()
  private opening?: Promise<void>
  // A plugin retry after the page answers must resume the approved call ID.

  constructor(private host: CoreHost, private external: boolean, private hostContract = false) {}

  private open() {
    return this.opening ??= this.start().catch((e) => { this.opening = undefined; throw e })
  }

  private async start() {
    const ready = await this.host.ready()
    const socket = net.createConnection(this.hostContract ? ready.host_socket : ready.unix_socket)
    this.socket = socket
    const chunks: Buffer[] = []
    let available = 0
    const take = (size: number): Buffer => {
      const result = Buffer.allocUnsafe(size)
      let copied = 0
      while (copied < size) {
        const chunk = chunks[0]
        const count = Math.min(chunk.length, size - copied)
        chunk.copy(result, copied, 0, count)
        copied += count
        if (count === chunk.length) chunks.shift()
        else chunks[0] = chunk.subarray(count)
      }
      available -= size
      return result
    }
    let length: number | undefined
    const fail = (error: Error) => {
      if (this.socket !== socket) return
      this.socket = undefined
      this.opening = undefined
      for (const p of this.waiting.values()) p.reject(error)
      this.waiting.clear()
      socket.destroy()
    }
    socket.on('error', fail)
    socket.on('close', () => fail(new Error('Service connection closed')))
    socket.on('data', (data) => {
      try {
        chunks.push(data)
        available += data.length
        while (true) {
          if (length === undefined) {
            if (available < 4) break
            length = take(4).readUInt32LE(0)
          }
          if (available < length) break
          const reply = decodeReply(take(length), this.hostContract)
          length = undefined
          if (reply.event) { this.host.event(reply.event); continue }
          const p = this.waiting.get(reply.id)
          if (!p) continue // A cancelled call may still reply.
          this.waiting.delete(reply.id)
          if (reply.error) p.reject(reply.error)
          else p.resolve(reply.value)
        }
      } catch (e) { fail(e as Error) }
    })
    await new Promise<void>((resolve, reject) => {
      socket.once('connect', resolve)
      socket.once('error', reject)
      socket.once('close', () => reject(new Error('Service connection closed')))
    })
    await this.write('HelloRequest', { token: this.external ? ready.external_token : ready.user_token,
      client_id: `web-${this.external ? 'external' : 'user'}-${randomUUID()}` })
    if (this.hostContract) this.host.event({ type: 'Connected', value: {} })
  }

  private write(method: string, value: unknown, signal?: AbortSignal): Promise<any> {
    signal?.throwIfAborted()
    if (!this.socket) return Promise.reject(new Error('Service connection is closed'))
    const requestId = ++this.nextId
    const data = encodeRequest(method, value, requestId, this.hostContract)
    const frame = Buffer.allocUnsafe(4 + data.length)
    frame.writeUInt32LE(data.length)
    frame.set(data, 4)
    return new Promise((resolve, reject) => {
      const cancel = () => {
        if (!this.waiting.delete(requestId)) return
        signal?.removeEventListener('abort', cancel)
        // The shared socket remains usable by other calls/conversations.
        if (method === 'ToolCallRequest') void this.write('ToolCallCancelRequest', { request_id: requestId }).catch(() => {})
        reject(signal?.reason)
      }
      const finish = () => signal?.removeEventListener('abort', cancel)
      this.waiting.set(requestId, {
        resolve: value => { finish(); resolve(value) },
        reject: error => { finish(); reject(error) },
      })
      this.socket!.write(frame)
      signal?.addEventListener('abort', cancel, { once: true })
    })
  }

  async call<T>(method: string, value: unknown = {}, signal?: AbortSignal): Promise<T> {
    signal?.throwIfAborted()
    await this.open()
    signal?.throwIfAborted()
    return this.write(method, value, signal)
  }
}

export class CoreHost {
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
    const repo = runtimeRoot(here)
    const build = process.env.TSUGUMI_BUILD_DIR ?? path.join(repo, 'build')
    const choose = (override: string | undefined, candidates: string[]) => override ?? candidates.find((p) => fs.existsSync(p)) ?? candidates[0]
    this.binary = choose(process.env.TSUGUMI_HOSTD, [path.join(repo, 'bin', 'tsugumi-hostd'), path.join(build, 'hostd', 'tsugumi-hostd')])
    this.schema = choose(process.env.TSUGUMI_AGENT_SCHEMA, [path.join(here, 'agent.bfbs'), path.join(build, 'generated', 'bfbs', 'agent.bfbs')])
    this.state = path.resolve(process.env.TSUGUMI_HOST_STATE ?? (process.platform === 'darwin'
      ? path.join(process.env.TSUGUMI_SUPPORT_DIR ?? path.join(os.homedir(), 'Library', 'Application Support', 'Tsugumi'), 'host-core')
      : path.join(support, 'host-core')))
    this.demoRoot = path.join(repo, 'demos')
  }

  async ready(): Promise<Ready> {
    return this.starting ??= (async () => {
      const socketDir = fs.mkdtempSync(path.join(process.env.TSUGUMI_SOCKET_DIR ?? '/tmp', 'tsugumi-host-'))
      // Only the service creates and numbers state. Move its startup log into
      // the new folder once this process has published readiness.
      const stateLog = path.join(this.state, 'host.log')
      const parent = path.dirname(this.state)
      fs.mkdirSync(parent, { recursive: true, mode: 0o700 })
      const logPath = fs.existsSync(this.state) ? stateLog : path.join(parent, `.host-startup-${randomUUID()}.log`)
      const log = fs.openSync(logPath, 'a', 0o600)
      fs.chmodSync(socketDir, 0o700)
      const child = spawn(this.binary, ['--ensure', '--state-dir', this.state, '--unix', path.join(socketDir, 'agent.sock'),
        '--listen', '127.0.0.1:0', '--demos-dir', this.demoRoot, ...(this.envs.hostArguments ?? [])], { detached: true, stdio: ['ignore', 'pipe', log] })
      fs.closeSync(log)
      return await new Promise<Ready>((resolve, reject) => {
        let bytes = ''
        child.stdout!.setEncoding('utf8')
        child.stdout!.on('data', chunk => {
          bytes += chunk
          if (!bytes.includes('\n')) return
          try {
            const next = JSON.parse(bytes.slice(0, bytes.indexOf('\n'))) as Ready
            if (logPath !== stateLog && next.pid === child.pid) fs.renameSync(logPath, stateLog)
            child.stdout!.destroy()
            child.unref()
            resolve(next)
          } catch (error) { reject(error) }
        })
        child.once('error', reject)
        child.once('exit', () => reject(new CoreHostError('startup_exit', `Host service exited during startup; see ${logPath}`, undefined, logPath)))
      })
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
      provider_id: config.auth === 'chatgpt' ? 'chatgpt' : config.auth === 'none' ? 'fixture' : config.api === 'openAIResponses' ? 'openai' : 'anthropic', model: config.model, purpose: config.purpose, use_choices: !!config.usesChoices }
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
        .filter((v) => v.value !== 0 && v.unionType && !['HelloRequest', 'ToolCallCancelRequest', 'CallIdRequest'].includes(v.name)).map((v) => v.name) ?? []
      this.api = new ToolCatalog(schema, requests)
    }
    return this.api
  }

  async tools(): Promise<Record<string, unknown>[]> {
    const project = await this.external.call<Record<string, unknown>[]>('ToolCatalogRequest')
    const agent = this.apiCatalog().tools.map((t) => ({ name: `agent_${t.name}`, description: t.description,
      inputSchema: t.inputSchema }))
    return [...project, ...agent]
  }

  async externalAPI(name: string, args: unknown, signal?: AbortSignal) {
    const tool = this.apiCatalog().tools.find((t) => `agent_${t.name}` === name)
    if (!tool) throw new Error(`Unknown Agent operation: ${name}`)
    const request = this.apiCatalog().schema.objects[tool.object].name.split('.').at(-1)!
    const value = await this.external.call(request, args ?? {}, signal)
    return { text: JSON.stringify(value, (_, v) => typeof v === 'bigint' ? v.toString() : v), isError: false }
  }
}

export interface InterruptedOperation { kind: string; id?: string; name?: string; arguments?: string }

export interface ModelBinding { providerId: string; model: string; effort?: string; fast: boolean }
export interface RemoteConversation { modelBinding?: ModelBinding; interruptedOperation?: InterruptedOperation; conversation: Conversation; busy: boolean; resumable?: boolean; continuable?: boolean; project_id?: string; event_seq?: bigint }

export class RemoteAgent {
  conversation: Conversation
  busy = false
  continuable = false
  resumable = false
  interruptedOperation?: InterruptedOperation
  modelBinding?: ModelBinding
  private seq = 0n
  private observing = false
  private observationID = 0
  private listener?: (e: AgentEvent) => void
  private transition: Promise<void> = Promise.resolve()
  private changing = 0

  constructor(readonly host: CoreHost, readonly id: string, readonly project?: string, readonly role = 'worker') {
    this.conversation = { id, rows: [], messages: [], archive: [], tools: [], usage: { inputTokens: 0, outputTokens: 0 }, updatedAt: Date.now() }
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
    this.modelBinding = result.modelBinding
  }

  async refresh() {
    this.adopt(await this.host.user.call<RemoteConversation>('ConversationGetRequest', { conversation_id: this.id }))
  }

  private interested = () => true
  observe(listener: (e: AgentEvent) => void, interested = this.interested) {
    this.interested = interested
    this.listener = listener
    if (this.observing || this.changing) return
    this.observing = true
    void this.poll(++this.observationID)
  }

  private async poll(identity: number) {
    try {
      while (this.observing && identity === this.observationID) {
        if (!this.busy && !this.interested()) { this.observing = false; return }
        const result = await this.host.user.call<{ events: { seq: bigint; event: AgentEvent }[]; next_seq: bigint; busy: boolean; rows: ChatRow[]; hasRows: boolean }>(
          'EventsRequest', { conversation_id: this.id, after_seq: this.seq })
        if (identity !== this.observationID) return
        if (result.events.some((e) => e.event.kind === 'finished')) await this.refresh()
        if (identity !== this.observationID) return
        for (const e of result.events) { this.seq = e.seq > this.seq ? e.seq : this.seq; this.listener?.(e.event) }
        if (result.hasRows) this.listener?.({ kind: 'presentation', rows: result.rows })
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
      await this.refresh()
      this.listener?.({ kind: 'modelAccepted' })
    })
  }

  action(action: string, listener: (e: AgentEvent) => void) {
    this.listener = listener
    return this.change(async () => {
      await this.beginTurn()
      await this.host.user.call('ConversationActionRequest', { conversation_id: this.id, action })
      await this.refresh()
      this.listener?.({ kind: 'modelAccepted' })
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
