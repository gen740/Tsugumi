// The Agent of the web UI, run by the host server: streaming model turns,
// tool definitions and argument conversion generated from tsugumi.bfbs, real
// RPC execution against the project's tsugumid, persistence per project and
// cancellation. It edits documents only through core RPC, never UI state.
import { randomUUID } from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'
import { Builder } from 'flatbuffers'
import { BaseType, BFBS, Field } from '../src/shared/bfbs'
import { FlatJSON } from '../src/shared/flatjson'
import { RPCClient } from '../src/shared/rpc'
import { AGENT, fill } from '../src/shared/agentShared'
import systemPrompt from '../../agent/Shared/system-prompt.md'

// ---- Tool catalog --------------------------------------------------------------

export interface ToolSpec {
  name: string
  payloadType: number
  object: number
  description: string
  inputSchema: Record<string, unknown>
  mutating: boolean
}

export const DEFAULT_REQUESTS = AGENT.requests
/** Filled by the Agent, never by the model. */
const AGENT_FIELDS = new Set(AGENT.agentFields)

/** The permissions an approval_required Failure names. */
export function approvalRequired(json: string): string[] | undefined {
  const value = (JSON.parse(json) as { value?: { kind?: string; args?: { key?: string; value?: string }[] } }).value
  if (value?.kind !== 'approval_required') return undefined
  const asked = (value.args ?? []).filter((a) => a.key === 'permission').map((a) => String(a.value))
  return asked.length ? asked : undefined
}

/** Undefined means the original operation has no terminal receipt yet. */
export function recoveredOperationError(json: string): boolean | undefined {
  try {
    const o = JSON.parse(json), v = o.value, r = v?.receipt
    if (o.reply !== 'OperationResult' || v?.state !== 'completed' ||
        typeof r?.result_type !== 'string' || !r.result_type || r.result_type === 'NONE' ||
        !r.result || typeof r.result !== 'object' || Array.isArray(r.result)) return undefined
    return r.result_type === 'Failure'
  } catch { return undefined }
}

export const toolName = (r: string) => r.replace('Request', '').replace(/(?<!^)([A-Z])/g, '_$1').toLowerCase()

export class ToolCatalog {
  readonly tools: ToolSpec[]
  readonly replyNames = new Map<number, string>()

  constructor(readonly schema: BFBS, requests = DEFAULT_REQUESTS) {
    const union = schema.enums.find((e) => e.name === 'Tsugumi.RPC.RequestPayload')
    if (!union) throw new Error('RequestPayload union not found')
    const reply = schema.enums.find((e) => e.name === 'Tsugumi.RPC.ReplyPayload')
    for (const v of reply?.values ?? []) this.replyNames.set(v.value, v.name)
    this.tools = requests.map((r) => {
      const index = schema.object(`Tsugumi.RPC.${r}`)
      const v = union.values.find((x) => x.unionType?.index === index)
      if (index < 0 || !v) throw new Error(`request ${r} not found`)
      const obj = schema.objects[index]
      const doc = obj.documentation.join(' ').trim()
      return {
        name: toolName(r), payloadType: v.value, object: index,
        description: `${r.replace('Request', '')}: ${doc}`,
        inputSchema: this.objectSchema(index, 0, AGENT_FIELDS),
        mutating: obj.fields.some((f) => f.name === 'op_id'),
      }
    })
  }

  tool(name: string) {
    return this.tools.find((t) => t.name === name)
  }

  private objectSchema(index: number, depth: number, skip = new Set<string>()): Record<string, unknown> {
    if (depth >= 8) return { type: 'object' }
    const obj = this.schema.objects[index]
    const props: Record<string, unknown> = {}
    const required: string[] = []
    for (const f of obj.fields) {
      if (skip.has(f.name) || f.type.base === BaseType.utype) continue
      const p = this.typeSchema(f, depth)
      if (f.documentation.length) p.description = f.documentation.join(' ')
      if (f.type.base === BaseType.union) {
        const e = this.schema.enums[f.type.index]
        props[`${f.name}_type`] = { type: 'string', enum: e.values.filter((x) => x.value !== 0).map((x) => x.name), description: `variant of ${f.name}` }
      }
      props[f.name] = p
      if (f.required) required.push(f.name)
    }
    const out: Record<string, unknown> = { type: 'object', properties: props, additionalProperties: false }
    if (required.length) out.required = required
    return out
  }

  private typeSchema(f: Field, depth: number): Record<string, unknown> {
    const t = f.type
    switch (t.base) {
      case BaseType.bool: return { type: 'boolean' }
      case BaseType.byte: case BaseType.ubyte: case BaseType.short: case BaseType.ushort: case BaseType.int: case BaseType.uint:
        if (t.index >= 0) return { type: 'string', enum: this.schema.enums[t.index].values.map((v) => v.name) }
        return { type: 'integer' }
      case BaseType.long: case BaseType.ulong:
        return { type: 'string', pattern: '^-?[0-9]+$', description: '64 bit integer as a decimal string' }
      case BaseType.float: case BaseType.double: return { type: 'number' }
      case BaseType.string: return { type: 'string' }
      case BaseType.obj: return this.objectSchema(t.index, depth + 1)
      case BaseType.union: {
        const e = this.schema.enums[t.index]
        return { anyOf: e.values.filter((v) => v.unionType && v.unionType.index >= 0).map((v) => this.objectSchema(v.unionType!.index, depth + 1)) }
      }
      case BaseType.vector:
        switch (t.element) {
          case BaseType.ubyte: case BaseType.byte: return { type: 'string', description: 'UTF-8 text (or {"base64": ...})' }
          case BaseType.string: return { type: 'array', items: { type: 'string' } }
          case BaseType.obj: return { type: 'array', items: this.objectSchema(t.index, depth + 1) }
          case BaseType.long: case BaseType.ulong: return { type: 'array', items: { type: 'string' } }
          case BaseType.double: case BaseType.float: return { type: 'array', items: { type: 'number' } }
          case BaseType.bool: return { type: 'array', items: { type: 'boolean' } }
          default: return { type: 'array' }
        }
      default: return {}
    }
  }
}

// ---- Conversation ------------------------------------------------------------

export type ContentBlock =
  | { type: 'text'; text: string }
  | { type: 'tool_use'; id: string; name: string; input: string }
  | { type: 'tool_result'; tool_use_id: string; content: string; is_error: boolean }

export interface ChatMessage {
  role: 'user' | 'assistant'
  content: ContentBlock[]
  /** What the chat shows for a request the app wrote for the model (the
   *  model gets `content`; this is never sent). */
  display?: string
}

export interface ToolRecord {
  id: string
  name: string
  arguments: string
  opId?: string
  state: 'pending' | 'sent' | 'completed' | 'skipped'
  result?: string
  isError: boolean
}

export interface Conversation {
  id: string
  title?: string
  updatedAt: number
  messages: ChatMessage[]
  /** Messages replaced by the summary when the context was compacted. */
  archive: ChatMessage[]
  tools: ToolRecord[]
  /** inputTokens: context of the last request (uncached, cache read and cache
   *  written); the cache counts add up over the conversation. */
  usage: { inputTokens: number; outputTokens: number; cacheReadTokens?: number; cacheWriteTokens?: number }
  summary?: string
}

export const newConversation = (id: string): Conversation => ({
  id, updatedAt: Date.now(), messages: [], archive: [], tools: [], usage: { inputTokens: 0, outputTokens: 0 },
})

export function loadConversation(file: string, id: string): Conversation {
  try {
    return { ...newConversation(id), ...JSON.parse(fs.readFileSync(file, 'utf8')) }
  } catch {
    return newConversation(id)
  }
}

const isDir = (p: string) => {
  try {
    return fs.statSync(p).isDirectory()
  } catch {
    return false
  }
}

function saveConversation(file: string, c: Conversation) {
  fs.mkdirSync(path.dirname(file), { recursive: true })
  fs.writeFileSync(file + '.tmp', JSON.stringify(c, null, 2))
  fs.renameSync(file + '.tmp', file)
}

export type AgentEvent =
  | { kind: 'text'; text: string }
  | { kind: 'toolStarted'; id: string; name: string; arguments: string }
  | { kind: 'toolFinished'; id: string; name: string; ok: boolean; summary: string }
  | { kind: 'compacted' }
  | { kind: 'retrying'; message: string }
  /** The text streamed so far in this reply is dropped: it is asked again. */
  | { kind: 'discardText' }
  | { kind: 'error'; message: string }
  | { kind: 'finished' }
  /** The turn used its steps (model replies with tool calls) without
   *  finishing; `unconfirmedRun` is a run it started and never saw end. */
  | { kind: 'stepLimit'; steps: number; unconfirmedRun?: string }

// ---- Provider ------------------------------------------------------------------

export interface ProviderConfig {
  name: string
  baseURL: string
  model: string
  /** Wire protocol of the endpoint. */
  api: 'anthropicMessages' | 'openAIResponses'
  /** Read on this host only (Keychain, 0600 file or environment); never sent
   *  to the page, tsugumid or Card workers. */
  apiKey?: string
  /** A real provider needs a key; the local fixture does not. */
  needsKey: boolean
  /** Output limit of one model reply: a Card's sources fit in one save. */
  maxTokens: number
  contextLimit: number
}

class HTTPStatusError extends Error {
  /** retryAfter: the provider's retry-after in seconds, when it sent one. */
  constructor(readonly status: number, readonly body: string, readonly retryAfter?: number) {
    super(`provider returned ${status}: ${body.slice(0, 400)}`)
  }
  static async from(r: Response) {
    const ms = Number(r.headers.get('retry-after-ms')), s = Number(r.headers.get('retry-after'))
    return new HTTPStatusError(r.status, await r.text(), ms > 0 ? ms / 1000 : s > 0 ? s : undefined)
  }
  get retryable() {
    return this.status === 429 || this.status >= 500
  }
}

class Cancelled extends Error {
  constructor() {
    super('cancelled')
  }
}

/** A stream cut before its end: retried like a failed connection. */
class ConnectionLost extends Error {
  constructor() {
    super('the connection to the model was lost')
  }
}

type StreamEvent =
  | { t: 'text'; text: string }
  | { t: 'toolStart'; index: number; id: string; name: string }
  | { t: 'toolInput'; index: number; json: string }
  | { t: 'stop'; reason: string; inputTokens: number; outputTokens: number; cacheRead: number; cacheWrite: number }

function headers(c: ProviderConfig): Record<string, string> {
  const h: Record<string, string> = { 'content-type': 'application/json' }
  if (c.api === 'anthropicMessages') h['anthropic-version'] = '2023-06-01'
  if (c.needsKey) {
    if (!c.apiKey) throw new Error(`no API key for ${c.name}; add it in Settings > API`)
    if (c.api === 'anthropicMessages') h['x-api-key'] = c.apiKey
    else h.authorization = 'Bearer ' + c.apiKey
  }
  return h
}

const endpoint = (c: ProviderConfig, p: string) => new URL(p, c.baseURL.endsWith('/') ? c.baseURL : c.baseURL + '/')

export async function listModels(c: ProviderConfig): Promise<string[]> {
  const r = await fetch(endpoint(c, 'v1/models'), { headers: headers(c) })
  if (r.status !== 200) throw new Error(`cannot list models (${r.status})`)
  const obj = (await r.json()) as { data?: { id: string }[] }
  return (obj.data ?? []).map((m) => m.id)
}

/** Lines of a server-sent event stream. */
async function* sseLines(r: Response): AsyncGenerator<string> {
  const reader = r.body!.getReader()
  const dec = new TextDecoder()
  let buf = ''
  for (;;) {
    const { value, done } = await reader.read()
    if (done) break
    buf += dec.decode(value, { stream: true })
    let nl: number
    while ((nl = buf.indexOf('\n')) >= 0) {
      yield buf.slice(0, nl).replace(/\r$/, '')
      buf = buf.slice(nl + 1)
    }
  }
  if (buf) yield buf
}

/** One model reply, streamed; the same code talks to the real provider and the local fixture. */
function stream(c: ProviderConfig, system: string, messages: ChatMessage[], tools: ToolSpec[], signal: AbortSignal): AsyncGenerator<StreamEvent> {
  return c.api === 'openAIResponses' ? responsesStream(c, system, messages, tools, signal) : messagesStream(c, system, messages, tools, signal)
}

/** Anthropic Messages API with SSE streaming. */
async function* messagesStream(c: ProviderConfig, system: string, messages: ChatMessage[], tools: ToolSpec[], signal: AbortSignal): AsyncGenerator<StreamEvent> {
  // Prompt caching: the system prompt and the tools are the same in every
  // step, and each step repeats the conversation before it, so all three end
  // with a cache breakpoint (at most 4 are allowed).
  const cache = { type: 'ephemeral' }
  const wired: any[] = messages.map((m) => ({ role: m.role, content: m.content.map(wire) }))
  const last = wired[wired.length - 1]
  if (last?.content.length) last.content[last.content.length - 1] = { ...last.content[last.content.length - 1], cache_control: cache }
  const body: Record<string, unknown> = {
    model: c.model, max_tokens: c.maxTokens, stream: true,
    system: [{ type: 'text', text: system, cache_control: cache }], messages: wired,
  }
  if (tools.length) {
    body.tools = tools.map((t, i) => ({
      name: t.name, description: t.description, input_schema: t.inputSchema, ...(i === tools.length - 1 ? { cache_control: cache } : {}),
    }))
  }
  const r = await fetch(endpoint(c, 'v1/messages'), { method: 'POST', headers: headers(c), body: JSON.stringify(body), signal })
  if (r.status !== 200) throw await HTTPStatusError.from(r)
  let event = ''
  let inputTokens = 0, outputTokens = 0, cacheRead = 0, cacheWrite = 0
  let stopped = false
  for await (const line of sseLines(r)) {
    if (line.startsWith('event:')) { event = line.slice(6).trim(); continue }
    if (!line.startsWith('data:')) continue
    let obj: any
    try { obj = JSON.parse(line.slice(5)) } catch { continue }
    switch (obj.type ?? event) {
      case 'message_start': {
        const u = obj.message?.usage
        cacheRead = u?.cache_read_input_tokens ?? 0
        cacheWrite = u?.cache_creation_input_tokens ?? 0
        inputTokens = (u?.input_tokens ?? 0) + cacheRead + cacheWrite
        break
      }
      case 'content_block_start':
        if (obj.content_block?.type === 'tool_use') yield { t: 'toolStart', index: obj.index ?? 0, id: obj.content_block.id ?? randomUUID(), name: obj.content_block.name ?? '' }
        break
      case 'content_block_delta':
        if (typeof obj.delta?.text === 'string') yield { t: 'text', text: obj.delta.text }
        if (typeof obj.delta?.partial_json === 'string') yield { t: 'toolInput', index: obj.index ?? 0, json: obj.delta.partial_json }
        break
      case 'message_delta':
        outputTokens = obj.usage?.output_tokens ?? outputTokens
        if (obj.delta?.stop_reason) yield { t: 'stop', reason: obj.delta.stop_reason, inputTokens, outputTokens, cacheRead, cacheWrite }
        break
      case 'message_stop': stopped = true; break
      case 'error': throw new Error(obj.error?.message ?? 'stream error')
    }
  }
  // A stream cut before message_stop is a lost connection, not a short answer.
  if (!stopped) throw new ConnectionLost()
}

/** The conversation as Responses API input items: messages, function calls
 *  (the model's tool uses) and their outputs. */
function responsesInput(messages: ChatMessage[]): unknown[] {
  const out: unknown[] = []
  for (const m of messages) {
    let texts: unknown[] = []
    const flush = () => {
      if (texts.length) out.push({ role: m.role, content: texts })
      texts = []
    }
    for (const b of m.content) {
      if (b.type === 'text') texts.push({ type: m.role === 'assistant' ? 'output_text' : 'input_text', text: b.text })
      else if (b.type === 'tool_use') {
        flush()
        out.push({ type: 'function_call', call_id: b.id, name: b.name, arguments: b.input })
      } else {
        flush()
        out.push({ type: 'function_call_output', call_id: b.tool_use_id, output: b.content })
      }
    }
    flush()
  }
  return out
}

/** OpenAI Responses API with an API key (not a ChatGPT account sign-in). */
async function* responsesStream(c: ProviderConfig, system: string, messages: ChatMessage[], tools: ToolSpec[], signal: AbortSignal): AsyncGenerator<StreamEvent> {
  const body: Record<string, unknown> = {
    model: c.model, instructions: system, input: responsesInput(messages), max_output_tokens: c.maxTokens, stream: true, store: false,
  }
  if (tools.length) body.tools = tools.map((t) => ({ type: 'function', name: t.name, description: t.description, parameters: t.inputSchema }))
  const r = await fetch(endpoint(c, 'v1/responses'), { method: 'POST', headers: headers(c), body: JSON.stringify(body), signal })
  if (r.status !== 200) throw await HTTPStatusError.from(r)
  let usedTools = false
  let finished = false
  for await (const line of sseLines(r)) {
    if (!line.startsWith('data:')) continue
    let obj: any
    try { obj = JSON.parse(line.slice(5)) } catch { continue }
    const index = typeof obj.output_index === 'number' ? obj.output_index : 0
    switch (obj.type) {
      case 'response.output_text.delta':
        if (typeof obj.delta === 'string') yield { t: 'text', text: obj.delta }
        break
      case 'response.output_item.added':
        if (obj.item?.type === 'function_call') {
          usedTools = true
          yield { t: 'toolStart', index, id: obj.item.call_id ?? randomUUID(), name: obj.item.name ?? '' }
        }
        break
      case 'response.function_call_arguments.delta':
        if (typeof obj.delta === 'string') yield { t: 'toolInput', index, json: obj.delta }
        break
      case 'response.completed': case 'response.incomplete': {
        const u = obj.response?.usage
        const reason = obj.type === 'response.incomplete' ? 'max_tokens' : usedTools ? 'tool_use' : 'end_turn'
        // OpenAI caches long prefixes by itself and reports what it read.
        yield { t: 'stop', reason, inputTokens: u?.input_tokens ?? 0, outputTokens: u?.output_tokens ?? 0,
          cacheRead: u?.input_tokens_details?.cached_tokens ?? 0, cacheWrite: 0 }
        finished = true
        break
      }
      case 'response.failed': case 'error': {
        const e = obj.response?.error ?? obj.error
        throw new Error(e?.message ?? e?.code ?? 'stream error')
      }
    }
  }
  if (!finished) throw new ConnectionLost()
}

function wire(b: ContentBlock): unknown {
  if (b.type === 'tool_use') {
    let input: unknown = {}
    try { input = JSON.parse(b.input) } catch {}
    return { type: 'tool_use', id: b.id, name: b.name, input }
  }
  return b
}

// ---- Agent -------------------------------------------------------------------

/** The rules and working examples the model gets (the same text as the macOS
 *  app's Agent; its default.nix and _outputs.fbs examples are the bundled
 *  demos' own files). */
export const SYSTEM_PROMPT = systemPrompt

/** reference_read: Tsugumi's bundled references, read only. */
const REFERENCE_TOOL: ToolSpec = { ...AGENT.referenceTool, payloadType: -1, object: -1, mutating: false }

const RUN_ACTIVE = new Set(['queued', 'preparing', 'running', 'cancelling'])
const RUN_ENDED = new Set(['succeeded', 'failed', 'cancelled'])

/** The run id and state in a rendered RunAccepted / RunStatus reply
 *  (RunAccepted nests them in "run"). */
export function runReply(json: string): { runId?: string; state?: string } {
  try {
    const v = JSON.parse(json)?.value
    const run = v?.run ?? v
    return { runId: run?.run_id, state: run?.state }
  } catch {
    return {}
  }
}

/** How much of a tool's result the model gets; a longer one says it was cut. */
export const RESULT_LIMIT = 60_000
export function clip(s: string): string {
  if (s.length <= RESULT_LIMIT) return s
  return s.slice(0, RESULT_LIMIT) + `\n[cut: showing the first ${RESULT_LIMIT} of ${s.length} characters; `
    + 'read the rest with a narrower request (an offset, a range or one file)]'
}

/** Keep recent tool calls together with their results; the summary supplies
 *  the leading user message if the retained tail starts with an assistant. */
export function compactionBoundary(messages: ChatMessage[], keep = 4): number {
  let cut = Math.max(0, messages.length - keep)
  while (cut > 0 && messages[cut].content.some((b) => b.type === 'tool_result')) cut--
  return cut
}

export class Agent {
  conversation: Conversation
  /** Model replies with tool calls in one turn before it stops and asks to
   *  continue (Draft generation takes many: write, check, save, run, read). */
  maxSteps = 60
  /** How long run_status waits for a run to end before it answers (ms). */
  statusWait = 300_000
  /** Folders of this host attached to the chat: the only ones
   *  attached_folder_read may read (tsugumid on the host reads any path). */
  folders: string[] = []
  /** Decides on the permissions a save would give Cards: those the user
   *  approves (by their defaults or when the page asks). Without one, none. */
  approver?: (asked: string[]) => Promise<Set<string>>
  private codec: FlatJSON
  private abort?: AbortController
  private turn?: Promise<void>
  /** A run started by a tool call whose end was not seen yet. */
  private unconfirmedRun?: string

  constructor(
    readonly catalog: ToolCatalog,
    private readonly rpc: () => Promise<RPCClient>,
    private provider: ProviderConfig,
    readonly storage: string,
    readonly id: string,
    /** Bundled references for reference_read: a top-level name ("demos",
     *  "sdk/python/tsugumi/__init__.py", "card-guide.md") -> where it is. */
    private readonly references: Record<string, string> = {},
  ) {
    this.codec = new FlatJSON(catalog.schema)
    this.conversation = loadConversation(storage, id)
  }

  /** The RPC tools and the Agent's own (reference_read, when references exist). */
  get tools(): ToolSpec[] {
    return Object.keys(this.references).length ? [...this.catalog.tools, REFERENCE_TOOL] : this.catalog.tools
  }

  get busy() {
    return this.turn !== undefined
  }

  setProvider(p: ProviderConfig) {
    this.provider = p
  }

  private save() {
    saveConversation(this.storage, this.conversation)
  }

  /** Stops model streaming and tools not yet sent. Runs already accepted by the
   *  core keep running; cancelling them needs an explicit run_cancel. */
  cancel() {
    this.abort?.abort()
  }

  /** Starts a turn once the previous one has ended: a cancelled turn's last
   *  steps never interleave with the next one's. */
  private start(fn: (signal: AbortSignal) => Promise<void>) {
    this.abort?.abort()
    const ac = new AbortController()
    this.abort = ac
    const previous = this.turn ?? Promise.resolve()
    const t = previous.catch(() => {}).then(() => fn(ac.signal)).finally(() => {
      if (this.turn === t) this.turn = undefined
    })
    this.turn = t
  }

  /** Sends a prepared prompt (references and skills already attached);
   *  `display` is what the chat shows for a request the app wrote. */
  send(prompt: string, title: string, events: (e: AgentEvent) => void, display?: string) {
    if (!this.conversation.title) this.conversation.title = title.slice(0, 60)
    this.conversation.messages.push({ role: 'user', content: [{ type: 'text', text: prompt }], ...(display ? { display } : {}) })
    this.conversation.updatedAt = Date.now()
    this.save()
    this.start((s) => this.loop(events, s))
  }

  /** Goes on after the step limit stopped a turn. */
  continueAfterLimit(events: (e: AgentEvent) => void) {
    let note = 'Continue from where you stopped (the step limit ended the previous turn). Check the current state first'
    if (this.unconfirmedRun) note += ` and the outcome of run ${this.unconfirmedRun} (run_status), which was started and never checked`
    note += ', then finish the remaining work.'
    this.conversation.messages.push({ role: 'user', content: [{ type: 'text', text: note }], display: '（止まったところから続けます）' })
    this.conversation.updatedAt = Date.now()
    this.save()
    this.start((s) => this.loop(events, s))
  }

  /** A turn whose model reply never arrived is continued; otherwise the last
   *  request is asked again. Confirmed tool calls are reconciled, never re-sent. */
  retry(events: (e: AgentEvent) => void) {
    const last = this.conversation.messages[this.conversation.messages.length - 1]
    if (!(last && last.role === 'user')) {
      const lastText = [...this.conversation.messages].reverse().find((m) => m.role === 'user' && m.content.some((b) => b.type === 'text'))
      if (!lastText) return
      this.conversation.messages.push(lastText)
      this.conversation.updatedAt = Date.now()
      this.save()
    }
    this.start((s) => this.loop(events, s))
  }

  compact(events: (e: AgentEvent) => void) {
    this.start(async (s) => {
      try {
        await this.compactIfNeeded(events, s, true)
        events({ kind: 'finished' })
      } catch (e) {
        events({ kind: 'error', message: s.aborted ? 'cancelled' : String((e as Error).message ?? e) })
      }
    })
  }

  /** Resumes after a restart or disconnect: tool calls that were sent but never
   *  answered are reconciled with OperationGet instead of being re-sent. */
  resume(events: (e: AgentEvent) => void) {
    const last = this.conversation.messages[this.conversation.messages.length - 1]
    if (!this.conversation.tools.some((t) => t.state === 'sent' || t.state === 'pending') && last?.role !== 'user') return
    this.start((s) => this.loop(events, s))
  }

  private async loop(events: (e: AgentEvent) => void, signal: AbortSignal) {
    try {
      await this.reconcilePending(events)
      this.closeDanglingToolUses()
      let cutReplies = 0
      let done = false
      for (let i = 0; i < this.maxSteps; i++) {
        if (signal.aborted) throw new Cancelled()
        await this.compactIfNeeded(events, signal)
        let { blocks, stop } = await this.modelTurn(events, signal)
        if (stop === 'max_tokens') {
          // The reply was cut at the output limit, maybe inside a tool call:
          // nothing of it is executed; the model is asked for smaller steps.
          if (++cutReplies > 3) throw new Error("the model's replies were cut at the output limit three times")
          blocks = blocks.filter((b) => b.type !== 'tool_use')
          if (!blocks.length) blocks = [{ type: 'text', text: '[cut at the output limit]' }]
          this.conversation.messages.push({ role: 'assistant', content: blocks })
          this.conversation.messages.push({
            role: 'user',
            content: [{ type: 'text', text: 'Your last reply was cut at the output limit, so its tool call was not run and nothing was saved. Make the '
              + 'change in smaller steps: one Card per project_save, and long source files in saves of their own.' }],
            display: '（応答が長すぎて途中で切れたため、小さく分けてやり直します）',
          })
          events({ kind: 'retrying', message: 'the reply was cut at the output limit; asking for smaller steps' })
          this.save()
          continue
        }
        this.conversation.messages.push({ role: 'assistant', content: blocks })
        this.save()
        const uses = blocks.filter((b): b is Extract<ContentBlock, { type: 'tool_use' }> => b.type === 'tool_use')
        if (!uses.length || stop !== 'tool_use') {
          done = true
          break
        }
        const results: ContentBlock[] = []
        for (const u of uses) {
          if (signal.aborted) throw new Cancelled()
          results.push(await this.runTool(u.id, u.name, u.input, events, signal))
        }
        // Cancelled meanwhile: the results are in the tool records; the next
        // turn answers the tool calls from them (never this cancelled one).
        if (signal.aborted) throw new Cancelled()
        this.conversation.messages.push({ role: 'user', content: results })
        this.save()
      }
      // Never a silent stop: the limit is its own outcome, and it can go on.
      events(done ? { kind: 'finished' } : { kind: 'stepLimit', steps: this.maxSteps, unconfirmedRun: this.unconfirmedRun })
    } catch (e) {
      if (signal.aborted || e instanceof Cancelled) {
        // Tools that were not sent are recorded as skipped; nothing is re-sent later.
        for (const t of this.conversation.tools) if (t.state === 'pending') t.state = 'skipped'
        this.save()
        events({ kind: 'error', message: 'cancelled' })
      } else {
        events({ kind: 'error', message: String((e as Error).message ?? e) })
      }
    }
  }

  /** One streamed model response; transient provider errors are retried with
   *  backoff before any of its tool calls has been executed. */
  private async modelTurn(events: (e: AgentEvent) => void, signal: AbortSignal) {
    for (let attempt = 0; ; attempt++) {
      let text = ''
      const tools = new Map<number, { id: string; name: string; json: string }>()
      const order: number[] = []
      let stop = 'end_turn'
      try {
        for await (const e of stream(this.provider, SYSTEM_PROMPT, this.wireMessages(), this.tools, signal)) {
          if (e.t === 'text') {
            text += e.text
            events({ kind: 'text', text: e.text })
          } else if (e.t === 'toolStart') {
            tools.set(e.index, { id: e.id, name: e.name, json: '' })
            order.push(e.index)
          } else if (e.t === 'toolInput') {
            const t = tools.get(e.index)
            if (t) t.json += e.json
          } else {
            stop = e.reason
            const u = this.conversation.usage
            u.inputTokens = e.inputTokens
            u.outputTokens += e.outputTokens
            u.cacheReadTokens = (u.cacheReadTokens ?? 0) + e.cacheRead
            u.cacheWriteTokens = (u.cacheWriteTokens ?? 0) + e.cacheWrite
          }
        }
        if (signal.aborted) throw new Cancelled()
        const blocks: ContentBlock[] = []
        if (text) blocks.push({ type: 'text', text })
        for (const i of order) {
          const t = tools.get(i)!
          blocks.push({ type: 'tool_use', id: t.id, name: t.name, input: t.json || '{}' })
        }
        return { blocks, stop }
      } catch (e) {
        if (signal.aborted) throw new Cancelled()
        const retryable = e instanceof HTTPStatusError ? e.retryable : e instanceof TypeError || e instanceof ConnectionLost
        if (!retryable || attempt >= 3) throw e
        // The reply is asked again: what it had streamed goes.
        if (text) events({ kind: 'discardText' })
        // The provider's retry-after when it gave one (rate limits), else backoff.
        const wait = Math.min(120, Math.max(e instanceof HTTPStatusError ? e.retryAfter ?? 0 : 0, 0.5 * (1 << (attempt + 1))))
        events({ kind: 'retrying', message: e instanceof HTTPStatusError
          ? `provider returned ${e.status}; retry ${attempt + 1} in ${Math.ceil(wait)} s` : `connection to the model failed; retry ${attempt + 1}` })
        await new Promise((r) => setTimeout(r, wait * 1000))
        continue
      }
    }
  }

  private wireMessages(): ChatMessage[] {
    const s = this.conversation.summary
    if (!s) return this.conversation.messages
    return [{ role: 'user', content: [{ type: 'text', text: `Summary of the earlier conversation:\n${s}` }] }, ...this.conversation.messages]
  }

  /** Reads a bundled reference file or lists a folder. */
  readReference(raw: string): [string, boolean] {
    const rel = raw.replace(/^[/ ]+|[/ ]+$/g, '').replaceAll('./', '')
    const names = Object.keys(this.references).sort()
    if (!rel || rel === '.') return [names.map((n) => (isDir(this.references[n]) ? n + '/' : n)).join('\n'), false]
    const top = names.find((n) => rel === n || rel.startsWith(n + '/'))
    if (rel.split('/').includes('..') || !top) return [`not found: ${rel} (reference_read "" lists what there is)`, true]
    const base = this.references[top]
    const target = rel === top ? base : path.join(base, rel.slice(top.length + 1))
    // Never outside the reference (symbolic links included).
    let real: string, realBase: string
    try {
      real = fs.realpathSync(target)
      realBase = fs.realpathSync(base)
    } catch {
      return [`not found: ${rel}`, true]
    }
    if (real !== realBase && !real.startsWith(realBase + '/')) return [`not found: ${rel}`, true]
    if (isDir(real)) {
      const entries = fs.readdirSync(real, { withFileTypes: true }).filter((d) => !d.name.startsWith('.'))
      return [entries.map((d) => (d.isDirectory() ? d.name + '/' : d.name)).sort().join('\n'), false]
    }
    try {
      const data = fs.readFileSync(real)
      if (data.length > 200_000) return [data.subarray(0, 200_000).toString('utf8') + '\n[cut at 200000 bytes]', false]
      return [data.toString('utf8'), false]
    } catch {
      return [`cannot read ${rel}`, true]
    }
  }

  /** Remembers a run the model started until it sees the run end. */
  private noteRun(tool: string, reply: string, json: string) {
    const { runId, state } = runReply(json)
    if (tool === 'run' && reply === 'RunAccepted' && runId) this.unconfirmedRun = runId
    else if (reply === 'RunStatus' && runId && runId === this.unconfirmedRun && state && RUN_ENDED.has(state)) this.unconfirmedRun = undefined
  }

  /** Every tool_use needs its tool_result at the start of the next message. A
   *  turn stopped between them (cancelled during a tool, a crash) gets the
   *  results recorded so far and "not run" for the rest, so the next request
   *  (a retry, a new message) is valid. */
  closeDanglingToolUses() {
    const ms = this.conversation.messages
    let changed = false
    for (let i = 0; i < ms.length; i++) {
      const ids = ms[i].role === 'assistant' ? ms[i].content.flatMap((b) => (b.type === 'tool_use' ? [b.id] : [])) : []
      const next = ms[i + 1]
      const answered = new Set(next?.role === 'user' ? next.content.flatMap((b) => (b.type === 'tool_result' ? [b.tool_use_id] : [])) : [])
      const missing = ids.filter((id) => !answered.has(id))
      if (!missing.length) continue
      const results: ContentBlock[] = missing.map((id) => {
        const r = this.conversation.tools.find((t) => t.id === id)
        if (r?.state === 'completed') return { type: 'tool_result', tool_use_id: id, content: clip(r.result ?? ''), is_error: r.isError }
        if (r?.state === 'pending') r.state = 'skipped'
        return {
          type: 'tool_result', tool_use_id: id, is_error: true,
          content: r?.state === 'sent'
            ? `sent, but the turn was stopped before the answer; check the outcome with operation_get (op_id ${r.opId ?? ''})`
            : 'not run: the turn was stopped before this call',
        }
      })
      // Tool results come first in their message.
      if (next?.role === 'user') next.content = [...results, ...next.content]
      else ms.splice(i + 1, 0, { role: 'user', content: results })
      changed = true
    }
    if (changed) this.save()
  }

  /** Validates arguments, executes the RPC and records the result. */
  private async runTool(id: string, name: string, input: string, events: (e: AgentEvent) => void, signal?: AbortSignal): Promise<ContentBlock> {
    events({ kind: 'toolStarted', id, name, arguments: input })
    const record: ToolRecord = this.conversation.tools.find((t) => t.id === id) ?? { id, name, arguments: input, state: 'pending', isError: false }
    this.conversation.tools = this.conversation.tools.filter((t) => t.id !== id)
    if (name === REFERENCE_TOOL.name && Object.keys(this.references).length) {
      let p = ''
      try {
        p = String(JSON.parse(input)?.path ?? '')
      } catch {}
      const [text, isError] = this.readReference(p)
      return this.finish(record, text, isError, events)
    }
    const spec = this.catalog.tool(name)
    if (!spec) return this.finish(record, `unknown tool ${name}`, true, events)
    // Incomplete or invalid arguments are never executed.
    let args: Record<string, unknown>
    try {
      const v = JSON.parse(input)
      if (typeof v !== 'object' || v === null || Array.isArray(v)) throw new Error()
      args = v
    } catch {
      return this.finish(record, 'the arguments are not a complete JSON object', true, events)
    }
    // tsugumid on this host reads any absolute folder: only the attached ones are offered.
    if (name === 'attached_folder_read') {
      const folder = typeof args.folder === 'string' ? args.folder.replace(/\/+$/, '') : ''
      if (!this.folders.includes(folder)) {
        return this.finish(record, JSON.stringify({ reply: 'Failure', value: { kind: 'not_found', message: `the folder ${folder} is not attached here (attach it in the chat)` } }), true, events)
      }
    }
    const full = { ...args }
    // Fields the Agent fills itself: never the model's.
    for (const f of AGENT_FIELDS) delete full[f]
    if (spec.mutating) {
      record.opId = record.opId ?? randomUUID()
      full.op_id = record.opId
      full.origin = 'agent'
    }
    // Validate by building once before sending anything.
    try {
      this.codec.build(spec.object, full, new Builder(1024))
    } catch (e) {
      return this.finish(record, String((e as Error).message), true, events)
    }
    record.state = 'sent'
    this.conversation.tools.push(record)
    this.save()
    try {
      const rpc = await this.rpc()
      const call = async () => {
        const r = await rpc.callRaw(spec.payloadType, (b) => this.codec.build(spec.object, full, b))
        const type = this.catalog.replyNames.get(r.type) ?? 'Unknown'
        return { type, json: this.renderReply(type, r.bb, r.table) }
      }
      let { type: replyType, json } = await call()
      // A save that widens what Cards may do: the user approves it (the server
      // applies their defaults or asks the page), then it is sent again as a
      // new operation with the approvals; declined, nothing is saved.
      const asked = replyType === 'Failure' ? approvalRequired(json) : undefined
      if (asked) {
        const approved = (await this.approver?.(asked)) ?? new Set<string>()
        const declined = asked.filter((p) => !approved.has(p))
        if (declined.length) return this.finish(record, fill(AGENT.permissionDeclined, { permissions: declined.join(', ') }), true, events)
        record.opId = randomUUID()
        full.op_id = record.opId
        full.approved_permissions = asked
        // The persisted record must name the operation we are about to send.
        this.save()
        if (signal?.aborted) throw new Cancelled()
        ;({ type: replyType, json } = await call())
      }
      // A run still building or running: wait here (up to statusWait) instead
      // of the model spending a reply on each look.
      if (name === 'run_status') {
        const deadline = Date.now() + this.statusWait
        while (replyType === 'RunStatus' && RUN_ACTIVE.has(runReply(json).state ?? '') && Date.now() < deadline) {
          await new Promise((r) => setTimeout(r, 1000))
          if (signal?.aborted) break
          ;({ type: replyType, json } = await call())
        }
      }
      this.noteRun(name, replyType, json)
      return this.finish(record, json, replyType === 'Failure', events)
    } catch (e) {
      // Unknown outcome: keep the record as sent so a resume asks OperationGet.
      if (spec.mutating) {
        return this.finish(record, `the connection failed after sending; outcome unknown (op_id ${record.opId}). Use operation_get.`, true, events, true)
      }
      return this.finish(record, String((e as Error).message ?? e), true, events)
    }
  }

  private finish(record: ToolRecord, result: string, isError: boolean, events: (e: AgentEvent) => void, keepSent = false): ContentBlock {
    record.state = keepSent ? 'sent' : 'completed'
    record.result = result
    record.isError = isError
    this.conversation.tools = this.conversation.tools.filter((t) => t.id !== record.id)
    this.conversation.tools.push(record)
    this.save()
    events({ kind: 'toolFinished', id: record.id, name: record.name, ok: !isError, summary: result.slice(0, 400) })
    return { type: 'tool_result', tool_use_id: record.id, content: clip(result), is_error: isError }
  }

  private renderReply(type: string, bb: import('flatbuffers').ByteBuffer, table: number): string {
    const i = this.catalog.schema.object(type === 'Failure' ? 'Tsugumi.Data.Failure' : `Tsugumi.RPC.${type}`)
    if (i < 0) return JSON.stringify({ reply: type })
    return JSON.stringify({ reply: type, value: this.codec.json(i, bb, table) })
  }

  private async reconcilePending(events: (e: AgentEvent) => void) {
    const op = this.catalog.tool('operation_get')
    if (!op) return
    for (const t of this.conversation.tools) {
      if (t.state !== 'sent' || !t.opId) continue
      try {
        const rpc = await this.rpc()
        const r = await rpc.callRaw(op.payloadType, (b) => this.codec.build(op.object, { op_id: t.opId }, b))
        t.result = this.renderReply(this.catalog.replyNames.get(r.type) ?? 'Unknown', r.bb, r.table)
        const error = recoveredOperationError(t.result)
        if (error !== undefined) {
          t.state = 'completed'
          t.isError = error
          events({ kind: 'toolFinished', id: t.id, name: t.name, ok: !error, summary: 'reconciled with operation_get' })
        }
        for (const m of this.conversation.messages) m.content = m.content.map((b) =>
          b.type === 'tool_result' && b.tool_use_id === t.id
            ? { ...b, content: clip(t.result!), is_error: error ?? true } : b)
      } catch {}
    }
    this.save()
  }

  /** Older turns are replaced by a model-written summary when the last request
   *  approached the provider's context limit (the full history stays archived). */
  private async compactIfNeeded(events: (e: AgentEvent) => void, signal: AbortSignal, force = false) {
    const c = this.conversation
    if (!(force || c.usage.inputTokens > (this.provider.contextLimit * 3) / 4) || c.messages.length <= (force ? 4 : 6)) return
    const messages = c.messages.slice(), cut = compactionBoundary(messages)
    if (!cut) return
    let transcript = c.summary ? `Earlier summary: ${c.summary}\n` : ''
    for (const m of messages.slice(0, cut)) {
      for (const b of m.content) {
        if (b.type === 'text') transcript += `${m.role}: ${b.text}\n`
        else if (b.type === 'tool_use') transcript += `tool call ${b.name} ${b.input.slice(0, 500)}\n`
        else transcript += `tool result${b.is_error ? ' (error)' : ''}: ${b.content.slice(0, 800)}\n`
      }
    }
    const ask: ChatMessage[] = [{ role: 'user', content: [{ type: 'text', text: `Summarize this Tsugumi session so far in under 200 words, keeping revisions, run ids and decisions:\n\n${transcript}` }] }]
    let summary = ''
    for await (const e of stream(this.provider, 'You summarize conversations.', ask, [], signal)) if (e.t === 'text') summary += e.text
    if (signal.aborted) throw new Cancelled()
    if (!summary.trim()) throw new Error('the context summary was empty; the conversation was kept')
    if (c.messages.length !== messages.length) return
    c.summary = summary
    c.archive = [...c.archive, ...messages.slice(0, cut)]
    c.messages = messages.slice(cut)
    c.usage.inputTokens = 0
    this.save()
    events({ kind: 'compacted' })
  }
}
