// The page's adapter for hostd's Host and Agent services: one WebSocket of
// each on this origin (hostd checks the page's cookie), with the page's Host
// Hello token. hostd owns the state; this keeps presentation glue only.
import { ProviderKeys } from './providerKeys'
import { CoreHostError, encodeRequest, decodeReply, hostMethod } from './serviceWire'
export { CoreHostError } from './serviceWire'
import type { Environments, HostEvent } from './environment'
import { EnvironmentPhase } from '../../shared/gen/tsugumi/host/environment-phase'
import type { ProviderConfig, Conversation } from './types'
import { openPageRpc } from '../wire/host'
import type { Transport } from '../wire/rpc'

class Connection {
  private transport?: Transport
  private nextId = 0n
  private opening?: Promise<void>

  constructor(private host: CoreHost, private hostContract: boolean) {}

  private connect() {
    return this.opening ??= this.start().catch((e) => { this.opening = undefined; throw e })
  }

  private async start() {
    const { token, transport } = await openPageRpc(this.hostContract ? 'host' : 'agent')
    this.transport = transport
    transport.onEvent = (frame) => {
      const reply = decodeReply(frame, this.hostContract)
      if (reply.event) this.host.event(reply.event)
    }
    transport.onClose = () => {
      if (this.transport !== transport) return
      this.transport = undefined
      this.opening = undefined
      if (this.hostContract) this.host.event({ type: 'Disconnected', value: new Error('Service connection closed') })
    }
    try {
      await this.write('HelloRequest', { token, client: 'Web' })
    } catch (error) {
      transport.close()
      throw error
    }
    if (this.hostContract) this.host.event({ type: 'Connected', value: {} })
  }

  private async write(method: string, value: unknown, signal?: AbortSignal): Promise<any> {
    signal?.throwIfAborted()
    const transport = this.transport
    if (!transport) throw new Error('Service connection is closed')
    const requestId = ++this.nextId
    const reply = transport.call(encodeRequest(method, value, requestId, this.hostContract), 0)
    const aborted = signal && new Promise<never>((_, reject) => signal.addEventListener('abort', () => {
      // The shared socket remains usable by other calls and conversations.
      if (method === 'ToolCallRequest') void this.write('ToolCallCancelRequest', { request_id: requestId }).catch(() => {})
      reject(signal.reason)
    }, { once: true }))
    const decoded = decodeReply(await (aborted ? Promise.race([reply, aborted]) : reply), this.hostContract)
    if (decoded.error) throw decoded.error
    return decoded.value
  }

  async call<T>(method: string, value: unknown = {}, signal?: AbortSignal): Promise<T> {
    signal?.throwIfAborted()
    await this.connect()
    signal?.throwIfAborted()
    return this.write(method, value, signal)
  }
}

export class CoreHost {
  readonly keys = new ProviderKeys((method, fields) => this.user.call(method, fields))
  private readonly agent = new Connection(this, false)
  private readonly host = new Connection(this, true)
  readonly user = {
    call: <T = any>(method: string, value: any = {}, signal?: AbortSignal): Promise<T> =>
      (hostMethod(method) ? this.host : this.agent).call<T>(method, value, signal),
  }
  private hostListeners = new Set<(event: HostEvent) => void>()
  observeHost(fn: (event: HostEvent) => void) {
    this.hostListeners.add(fn)
    return () => this.hostListeners.delete(fn)
  }
  event(event: HostEvent) { for (const listener of this.hostListeners) listener(event) }

  constructor(readonly envs: Environments) {}

  /** Opens the Host connection (its events start with it). */
  async connectHost() { await this.user.call('HostActivityRequest') }

  async getSettings(profile: string): Promise<{ revision: string; values: { key: string; value_json: string }[] }> {
    return this.user.call('SettingsGetRequest', { profile })
  }

  async project(folder: string): Promise<string> {
    const current = await this.envs.state(folder)
    const state = current.phase === EnvironmentPhase.Ready ? current : await this.envs.start(folder)
    if (state.phase !== EnvironmentPhase.Ready || !state.project_id) throw new Error(state.message || 'Project core is not ready')
    return state.project_id
  }

  async conversation(id: string, folder?: string, role?: string) {
    return this.user.call<ConversationSnapshot>('ConversationCreateRequest', {
      conversation_id: id, project_id: folder ? await this.project(folder) : undefined, role,
    })
  }

  async cancelConversation(id: string) {
    await this.connectHost()
    let notify: (() => void) | undefined
    let failure: Error | undefined
    const off = this.observeHost(({ type, value }) => {
      if (type === 'Disconnected') failure = value
      else if (type !== 'ConversationChanged' || value.conversation_id !== id) return
      notify?.()
      notify = undefined
    })
    // Install before asking to cancel. A fresh snapshot covers completion that
    // happened before a notification could wake the next read.
    const wait = () => new Promise<void>((resolve) => { notify = resolve })
    try {
      await this.user.call('ConversationCancelRequest', { conversation_id: id })
      for (;;) {
        const changed = wait()
        const current = await this.user.call<ConversationSnapshot>('ConversationGetRequest', { conversation_id: id })
        if (!current.busy) return
        if (failure) throw failure
        await changed
        if (failure) throw failure
      }
    } finally { off() }
  }

  async provider(config: ProviderConfig) {
    return { profile: config.profile ?? 'desktop', account_profile: config.accountProfile,
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
    return this.user.call('ModelsListRequest', { provider_id: id, refresh: true, profile: selection.profile, account_profile: selection.account_profile })
  }

  async summarize(conversationId: string, config: ProviderConfig): Promise<{ text: string; usage: { inputTokens: number; outputTokens: number } }> {
    return this.user.call('ConversationSummarizeRequest', { conversation_id: conversationId, provider: await this.provider(config) })
  }
}

export interface InterruptedOperation { kind: string; id?: string; name?: string; arguments?: string }

export interface ModelBinding { providerId: string; model: string; effort?: string; fast: boolean }
export interface ConversationSnapshot { modelBinding?: ModelBinding; interruptedOperation?: InterruptedOperation; conversation: Conversation; busy: boolean; resumable?: boolean; continuable?: boolean; runWake?: boolean; project_id?: string; event_seq: bigint }
