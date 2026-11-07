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
import { AGENT, fill, toolSummary } from '../src/shared/agentShared'
import systemPrompt from '../../agent/Shared/system-prompt.md'
import guidePrompt from '../../agent/Shared/guide-prompt.md'
import versionSummaryPrompt from '../../agent/Shared/version-summary-prompt.md'
import demoSkill from '../../agent/Shared/demo-skill.md'
import { ChatGPTError, planError, TransientError } from './chatgpt'

// ---- Tool catalog --------------------------------------------------------------

export interface ToolSpec {
  name: string
  payloadType: number
  object: number
  description: string
  inputSchema: Record<string, unknown>
  mutating: boolean
  /** Run by the model provider (web search): it has no tool_use of ours. */
  server?: boolean
}

export const DEFAULT_REQUESTS = AGENT.requests
/** Filled by the Agent, never by the model. */
const AGENT_FIELDS = new Set(AGENT.agentFields)
/** Fields of a request that the apps use and the model is not offered. */
const NOT_FOR_MODELS: Record<string, string[]> = AGENT.notForModels

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
        inputSchema: this.objectSchema(index, 0, new Set([...AGENT_FIELDS, ...(NOT_FOR_MODELS[r] ?? [])])),
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

/** The arguments of a tool call as the object they must be; undefined: broken
 *  JSON or something else than an object (the API does not check them as they
 *  stream). A broken call goes back as an empty one: the APIs want an object
 *  there, and its text is in its result. */
export function toolArguments(input: string): Record<string, unknown> | undefined {
  try {
    const v = JSON.parse(input)
    return typeof v === 'object' && v !== null && !Array.isArray(v) ? v : undefined
  } catch {
    return undefined
  }
}

export type ContentBlock =
  | { type: 'text'; text: string }
  | { type: 'tool_use'; id: string; name: string; input: string }
  | { type: 'tool_result'; tool_use_id: string; content: string; is_error: boolean; images?: ToolImage[] }
  /** A search the provider ran: what was asked and what came back. */
  | { type: 'web_search'; id: string; query: string; sources: WebSource[]; error?: string }

export interface WebSource { url: string; title?: string }

/** An image in a tool's result (a Card's display in run_record, a PNG read from
 *  the project or an artifact), which the model sees as an image, never as text. */
export interface ToolImage { media_type: string; data: string }

export interface ChatMessage {
  role: 'user' | 'assistant'
  content: ContentBlock[]
  /** What the chat shows for a request the app wrote for the model (the
   *  model gets `content`; this is never sent). */
  display?: string
  /** A reply with search blocks as the Messages API sent them: it wants them
   *  back unchanged (encrypted results, citations) while the search tool is
   *  declared. `content` says the same in the app's own terms. */
  providerContent?: Record<string, unknown>[]
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
  let text: string
  try { text = fs.readFileSync(file, 'utf8') }
  catch (error) {
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') return newConversation(id)
    throw error
  }
  const c = JSON.parse(text)
  const messages = (ms: any) => Array.isArray(ms) && ms.every((m) =>
    m && ['user', 'assistant'].includes(m.role) && Array.isArray(m.content) && m.content.every((b: any) =>
      b && (b.type === 'text' ? typeof b.text === 'string'
        : b.type === 'tool_use' ? [b.id, b.name, b.input].every((v) => typeof v === 'string')
        : b.type === 'web_search' ? [b.id, b.query].every((v) => typeof v === 'string') && Array.isArray(b.sources)
        : b.type === 'tool_result' && [b.tool_use_id, b.content].every((v) => typeof v === 'string') && typeof b.is_error === 'boolean' &&
          (b.images === undefined || (Array.isArray(b.images) && b.images.every((i: any) => [i?.media_type, i?.data].every((v) => typeof v === 'string')))))))
  if (!c || typeof c.id !== 'string' || !Number.isFinite(c.updatedAt) || !messages(c.messages) || !messages(c.archive) ||
      !Array.isArray(c.tools) || !c.tools.every((t: any) => t && [t.id, t.name, t.arguments].every((v) => typeof v === 'string') &&
        ['pending', 'sent', 'completed', 'skipped'].includes(t.state) && (t.opId === undefined || typeof t.opId === 'string')) ||
      !c.usage || !Number.isFinite(c.usage.inputTokens) || !Number.isFinite(c.usage.outputTokens)) {
    throw new Error(`Invalid conversation file: ${file}`)
  }
  return c
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
  /** A piece of a tool call's arguments as the model writes them, for the calls that carry whole files. */
  | { kind: 'toolInput'; index: number; name: string; delta: string }
  /** Earlier turns are being summarized: the next reply waits for it. */
  | { kind: 'compacting' }
  | { kind: 'compacted' }
  | { kind: 'retrying'; message: string }
  /** The text streamed so far in this reply is dropped: it is asked again. */
  | { kind: 'discardText' }
  | { kind: 'error'; message: string }
  | { kind: 'finished' }
  /** The turn used its steps (model replies with tool calls) without
   *  finishing; `unconfirmedRun` is a run it started and never saw end. */
  | { kind: 'stepLimit'; steps: number; unconfirmedRun?: string }
  /** What a helper started by `delegate` does (its own events). */
  | { kind: 'helper'; name: string; event: AgentEvent }

// ---- Provider ------------------------------------------------------------------

export interface ProviderConfig {
  name: string
  baseURL: string
  model: string
  /** Wire protocol of the endpoint. */
  api: 'anthropicMessages' | 'openAIResponses'
  /** How requests are authorized: none (the local fixture), an API key, or
   *  the user's ChatGPT plan (Sign in with ChatGPT). */
  auth: 'none' | 'apiKey' | 'chatgpt'
  /** Read on this host only (Keychain, 0600 file or environment); never sent
   *  to the page, tsugumid or Card workers. */
  apiKey?: string
  /** The ChatGPT access token (refreshed near its expiry; `stale` is one the API refused). */
  token?: (stale?: string) => Promise<string>
  /** Output limit of one model reply: a Card's sources fit in one save. */
  maxTokens: number
  contextLimit: number
  /** The user's setting (Settings > API): the model may search the web. */
  webSearch?: boolean
  /** The chat's choices in the model menu, already checked against the model
   *  (modelChoices). None for the requests that are not the Agent's work. */
  effort?: string
  fast?: boolean
}

/** Room for a reply: the deep levels spend much of it on thinking. */
function outputLimit(c: ProviderConfig): number {
  return c.effort === 'xhigh' || c.effort === 'max' ? Math.max(c.maxTokens, AGENT.modelOptions.largeOutputTokens) : c.maxTokens
}

/** The provider runs the searches itself: the local fixture has none. */
export const searchesWeb = (c: ProviderConfig) => !!c.webSearch && c.auth !== 'none'

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
  | { t: 'search'; id: string; query: string }
  | { t: 'searchResult'; id: string; sources: WebSource[]; error?: string }
  | { t: 'citation'; source: WebSource }
  | { t: 'providerContent'; blocks: Record<string, unknown>[] }
  | { t: 'stop'; reason: string; inputTokens: number; outputTokens: number; cacheRead: number; cacheWrite: number }

async function headers(c: ProviderConfig, stale?: string): Promise<Record<string, string>> {
  const h: Record<string, string> = { 'content-type': 'application/json' }
  if (c.api === 'anthropicMessages') h['anthropic-version'] = '2023-06-01'
  if (c.auth === 'apiKey') {
    if (!c.apiKey) throw new Error(`no API key for ${c.name}; add it in Settings > API`)
    if (c.api === 'anthropicMessages') h['x-api-key'] = c.apiKey
    else h.authorization = 'Bearer ' + c.apiKey
  } else if (c.auth === 'chatgpt') {
    h.authorization = 'Bearer ' + (await c.token!(stale))
  }
  return h
}

const endpoint = (c: ProviderConfig, p: string) => new URL(p, c.baseURL.endsWith('/') ? c.baseURL : c.baseURL + '/')

/** Available models, with the names to show (the ChatGPT account's catalog
 *  lists display names, in the server's order). */
export async function listModels(c: ProviderConfig): Promise<{ id: string; label: string }[]> {
  const r = await fetch(endpoint(c, 'v1/models'), { headers: await headers(c), signal: AbortSignal.timeout(15_000) })
  if (r.status !== 200) throw new Error(`cannot list models (${r.status})`)
  const obj = (await r.json()) as { data?: { id: string }[]; models?: { slug: string; display_name?: string; visibility?: string }[] }
  if (c.auth === 'chatgpt' && obj.models) {
    return obj.models.filter((m) => m.visibility === 'list').map((m) => ({ id: m.slug, label: m.display_name ?? m.slug }))
  }
  return (obj.data ?? []).map((m) => ({ id: m.id, label: m.id }))
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

/** The request of the Messages API. */
export function messagesBody(c: ProviderConfig, system: string, messages: ChatMessage[], tools: ToolSpec[]): Record<string, unknown> {
  // Prompt caching: the system prompt and the tools are the same in every
  // step, and each step repeats the conversation before it, so all three end
  // with a cache breakpoint (at most 4 are allowed).
  const cache = { type: 'ephemeral' }
  // Search blocks go back only with the tool declared, and only whole.
  const replay = tools.some((t) => t.server) ? replayable(messages) : new Set<number>()
  const wired: any[] = messages.map((m, i) => ({ role: m.role, content: replay.has(i) ? m.providerContent! : m.content.map(wire) }))
  const last = wired[wired.length - 1]
  if (last?.content.length) {
    const at = last.content.findLastIndex(cacheable)
    if (at >= 0) last.content = last.content.map((b: unknown, i: number) => (i === at ? { ...(b as object), cache_control: cache } : b))
  }
  const body: Record<string, unknown> = {
    model: c.model, max_tokens: outputLimit(c), stream: true,
    system: [{ type: 'text', text: system, cache_control: cache }], messages: wired,
  }
  if (c.effort) body.output_config = { effort: c.effort }
  if (c.fast) body.speed = 'fast'
  if (tools.length) {
    // Eager: the arguments come as the model writes them, not held back until
    // each is whole, so the API no longer checks them (runTool does).
    body.tools = tools.map((t, i) => (t.server ? WEB_SEARCH_WIRE : {
      name: t.name, description: t.description, input_schema: t.inputSchema, eager_input_streaming: true, ...(i === tools.length - 1 ? { cache_control: cache } : {}),
    }))
  }
  return body
}

/** Anthropic Messages API with SSE streaming. */
async function* messagesStream(c: ProviderConfig, system: string, messages: ChatMessage[], tools: ToolSpec[], signal: AbortSignal): AsyncGenerator<StreamEvent> {
  const body = messagesBody(c, system, messages, tools)
  const r = await fetch(endpoint(c, 'v1/messages'), { method: 'POST', headers: await headers(c), body: JSON.stringify(body), signal })
  if (r.status !== 200) throw await HTTPStatusError.from(r)
  let event = ''
  let inputTokens = 0, outputTokens = 0, cacheRead = 0, cacheWrite = 0
  let stopped = false
  // The reply's blocks as they come, to send them back.
  const reply = new Map<number, Record<string, any>>()
  const inputs = new Map<number, string>()
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
      case 'content_block_start': {
        const b = obj.content_block, index = obj.index ?? 0
        if (['text', 'tool_use', 'server_tool_use', 'web_search_tool_result'].includes(b?.type)) reply.set(index, { ...b })
        if (b?.type === 'tool_use') yield { t: 'toolStart', index, id: b.id ?? randomUUID(), name: b.name ?? '' }
        if (b?.type === 'web_search_tool_result') yield { t: 'searchResult', id: b.tool_use_id ?? '', ...searchResultOf(b.content) }
        break
      }
      case 'content_block_delta': {
        const index = obj.index ?? 0, block = reply.get(index)
        if (typeof obj.delta?.text === 'string') {
          if (block) block.text = (block.text ?? '') + obj.delta.text
          yield { t: 'text', text: obj.delta.text }
        }
        if (typeof obj.delta?.partial_json === 'string') {
          inputs.set(index, (inputs.get(index) ?? '') + obj.delta.partial_json)
          if (block?.type !== 'server_tool_use') yield { t: 'toolInput', index, json: obj.delta.partial_json }
        }
        if (obj.delta?.citation) {
          if (block) block.citations = [...(block.citations ?? []), obj.delta.citation]
          if (typeof obj.delta.citation.url === 'string') yield { t: 'citation', source: { url: obj.delta.citation.url, title: obj.delta.citation.title } }
        }
        break
      }
      case 'content_block_stop': {
        const block = reply.get(obj.index ?? 0)
        if (block?.type === 'tool_use' || block?.type === 'server_tool_use') block.input = parsedInput(inputs.get(obj.index ?? 0))
        if (block?.type === 'server_tool_use') yield { t: 'search', id: block.id ?? randomUUID(), query: String(block.input?.query ?? '') }
        break
      }
      case 'message_delta': {
        outputTokens = obj.usage?.output_tokens ?? outputTokens
        if (!obj.delta?.stop_reason) break
        const blocks = replyToSendBack(reply)
        if (blocks) yield { t: 'providerContent', blocks }
        yield { t: 'stop', reason: obj.delta.stop_reason, inputTokens, outputTokens, cacheRead, cacheWrite }
        break
      }
      case 'message_stop': stopped = true; break
      case 'error': throw new Error(obj.error?.message ?? 'stream error')
    }
  }
  // A stream cut before message_stop is a lost connection, not a short answer.
  if (!stopped) throw new ConnectionLost()
}

// ---- Web search ----------------------------------------------------------------

const WEB_SEARCH_TOOL: ToolSpec = {
  name: AGENT.webSearch.name, payloadType: -1, object: -1, description: '', inputSchema: {}, mutating: false, server: true,
}
/** The basic tool: dynamic filtering (the later versions) runs code at the
 *  provider, which needs a container kept through the turn and is outside
 *  zero data retention. A turn's searches are capped. */
const WEB_SEARCH_WIRE = { type: AGENT.webSearch.anthropicType, name: AGENT.webSearch.name, max_uses: AGENT.webSearch.maxUses }

const parsedInput = (json: string | undefined): any => toolArguments(json ?? '{}') ?? {}

/** The sources in a web_search_tool_result's content, or its error object. */
function searchResultOf(content: any): { sources: WebSource[]; error?: string } {
  if (!Array.isArray(content)) return { sources: [], error: String(content?.error_code ?? 'unavailable') }
  return { sources: content.filter((r) => typeof r?.url === 'string').map((r) => ({ url: r.url, title: r.title })) }
}

/** A reply's blocks to send back, when it has anything only they can carry.
 *  No thinking blocks: this Agent sends none back anywhere, and one in an
 *  edited history is refused. */
function replyToSendBack(reply: Map<number, Record<string, any>>): Record<string, unknown>[] | undefined {
  const blocks = [...reply.entries()].sort(([a], [b]) => a - b).map(([, b]) => {
    const { citations, ...rest } = b
    return citations?.length ? { ...rest, citations } : rest
  }).filter((b) => b.type !== 'text' || b.text)
  return blocks.some((b) => b.type === 'server_tool_use' || b.type === 'web_search_tool_result' || b.citations) ? blocks : undefined
}

/** Whether a block may hold a cache breakpoint: not the provider's own blocks. */
const cacheable = (b: Record<string, unknown>) => b.type === 'tool_use' || b.type === 'tool_result' || (b.type === 'text' && !b.citations)

/** The messages whose search blocks can go back to the API as they came. It
 *  refuses a search without its result, unless the conversation ends there
 *  (pause_turn, or one that waits for our tool results), and a result without
 *  its search: after a stopped turn or a compaction those go in words. */
export function replayable(messages: ChatMessage[]): Set<number> {
  const keep = new Set(messages.flatMap((m, i) => (m.providerContent ? [i] : [])))
  const last = messages.length - 1
  const waitingForResults = last >= 0 && messages[last].role === 'user' && messages[last].content.every((b) => b.type === 'tool_result')
  for (;;) {
    const open = new Map<string, number>()
    let bad: number | undefined
    for (const i of [...keep].sort((a, b) => a - b)) {
      for (const b of messages[i].providerContent!) {
        if (b.type === 'server_tool_use') open.set(String(b.id), i)
        else if (b.type === 'web_search_tool_result' && !open.delete(String(b.tool_use_id))) bad = i
      }
    }
    for (const i of open.values()) if (i !== last && !(i === last - 1 && waitingForResults)) bad = i
    if (bad === undefined) return keep
    keep.delete(bad)
  }
}

/** A search in words, for a request that cannot carry the provider's blocks
 *  (the tool off, another provider). It never names the tool. */
function searchNote(b: Extract<ContentBlock, { type: 'web_search' }>): string {
  const results = b.sources.length ? `; results: ${b.sources.map((s) => s.url).join(', ')}` : ''
  return `[web search: ${b.query}${b.error ? `; failed: ${b.error}` : ''}${results}]`
}

/** What a search found, for the chat's row and the tool record. */
function searchSummary(sources: WebSource[], error?: string): string {
  if (error) return `failed: ${error}`
  return sources.map((s) => (s.title ? `${s.title} — ${s.url}` : s.url)).join('\n') || 'no results'
}

/** The cited sources as Markdown links under a reply. They come from the web:
 *  only http(s) is linked, and a title cannot break out of its link. */
export function sourcesText(sources: WebSource[]): string {
  const seen = new Set<string>()
  const lines: string[] = []
  for (const s of sources) {
    if (!/^https?:\/\//i.test(s.url) || seen.has(s.url)) continue
    seen.add(s.url)
    const url = s.url.replace(/[\s()<>]/g, (c) => encodeURIComponent(c).replace('(', '%28').replace(')', '%29'))
    const title = (s.title ?? s.url).replace(/[\[\]\r\n]+/g, ' ').trim() || s.url
    lines.push(`- [${title}](${url})`)
  }
  return lines.length ? `\n\n${AGENT.webSearch.sources}:\n${lines.join('\n')}\n` : ''
}

/** The ChatGPT plan takes function tools only grouped in a namespace; the
 *  model's calls then name it, and the history repeats it. */
const TOOL_NAMESPACE = 'tsugumi'

/** The conversation as Responses API input items: messages, function calls
 *  (the model's tool uses) and their outputs. */
export function responsesInput(messages: ChatMessage[], namespace?: string): unknown[] {
  const out: unknown[] = []
  for (const m of messages) {
    let texts: unknown[] = []
    const flush = () => {
      if (texts.length) out.push({ role: m.role, content: texts })
      texts = []
    }
    for (const b of m.content) {
      if (b.type === 'text') texts.push({ type: m.role === 'assistant' ? 'output_text' : 'input_text', text: b.text })
      // In words: a web_search_call item would bring back none of its results.
      else if (b.type === 'web_search') texts.push({ type: 'output_text', text: searchNote(b) })
      else if (b.type === 'tool_use') {
        flush()
        out.push({ type: 'function_call', call_id: b.id, name: b.name, arguments: toolArguments(b.input) ? b.input : '{}', ...(namespace ? { namespace } : {}) })
      } else {
        flush()
        const images = b.images ?? []
        out.push({ type: 'function_call_output', call_id: b.tool_use_id, output: images.length
          ? [{ type: 'input_text', text: b.content }, ...images.map((i) => ({ type: 'input_image', image_url: `data:${i.media_type};base64,${i.data}` }))]
          : b.content })
      }
    }
    flush()
  }
  return out
}

/** The request of the Responses API, with an API key or the ChatGPT plan. The
 *  plan takes no max_output_tokens and the tools in a namespace; store false
 *  and stream true always. */
export function responsesBody(c: ProviderConfig, system: string, messages: ChatMessage[], tools: ToolSpec[]): Record<string, unknown> {
  const plan = c.auth === 'chatgpt'
  const body: Record<string, unknown> = {
    model: c.model, instructions: system, input: responsesInput(messages, plan ? TOOL_NAMESPACE : undefined), stream: true, store: false,
  }
  if (!plan) body.max_output_tokens = outputLimit(c)
  if (c.effort) body.reasoning = { effort: c.effort }
  if (c.fast) body.service_tier = 'fast'
  const own = tools.filter((t) => !t.server)
  const hosted: unknown[] = own.length === tools.length ? [] : [{ type: AGENT.webSearch.openAIType }]
  if (tools.length) {
    const functions = own.map((t) => ({ type: 'function', name: t.name, description: t.description, parameters: t.inputSchema }))
    body.tools = [...(plan ? (functions.length ? [{ type: 'namespace', name: TOOL_NAMESPACE, description: 'Tools of the open Tsugumi project.', tools: functions }] : []) : functions), ...hosted]
  }
  // The sources of a search (the plan's rules do not say it takes `include`:
  // there the cited sources are what can be followed).
  if (hosted.length && !plan) body.include = [AGENT.webSearch.openAIInclude]
  return body
}

/** OpenAI Responses API, with an API key or the ChatGPT plan. */
async function* responsesStream(c: ProviderConfig, system: string, messages: ChatMessage[], tools: ToolSpec[], signal: AbortSignal): AsyncGenerator<StreamEvent> {
  const plan = c.auth === 'chatgpt'
  const body = responsesBody(c, system, messages, tools)
  let stale: string | undefined
  let r: Response
  for (;;) {
    const h = await headers(c, stale)
    r = await fetch(endpoint(c, 'v1/responses'), { method: 'POST', headers: h, body: JSON.stringify(body), signal })
    if (r.status === 200) break
    // A refused access token is refreshed once (unless refreshed meanwhile).
    if (plan && r.status === 401 && stale === undefined) {
      stale = h.authorization.replace(/^Bearer /, '')
      await r.body?.cancel()
      continue
    }
    if (!plan) throw await HTTPStatusError.from(r)
    const text = await r.text()
    let obj: any
    try { obj = JSON.parse(text) } catch {}
    throw planError(r.status, obj?.error, typeof obj?.detail === 'string' ? obj.detail : text, r.headers.get('x-request-id'))
  }
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
          let name: string = obj.item.name ?? ''
          if (name.startsWith(TOOL_NAMESPACE + '.')) name = name.slice(TOOL_NAMESPACE.length + 1)
          yield { t: 'toolStart', index, id: obj.item.call_id ?? randomUUID(), name }
        }
        break
      case 'response.function_call_arguments.delta':
        if (typeof obj.delta === 'string') yield { t: 'toolInput', index, json: obj.delta }
        break
      case 'response.output_item.done':
        if (obj.item?.type === 'web_search_call') {
          const id: string = obj.item.id ?? randomUUID()
          const { query, sources } = openAISearch(obj.item.action)
          yield { t: 'search', id, query }
          yield { t: 'searchResult', id, sources, error: obj.item.status === 'failed' ? 'failed' : undefined }
        } else if (obj.item?.type === 'message') {
          for (const c of obj.item.content ?? []) for (const a of c.annotations ?? []) {
            if (a?.type === 'url_citation' && typeof a.url === 'string') yield { t: 'citation', source: { url: a.url, title: a.title } }
          }
        }
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
        if (plan) throw planError(0, e, 'stream error', r.headers.get('x-request-id'))
        throw new Error(e?.message ?? e?.code ?? 'stream error')
      }
    }
  }
  if (!finished) throw new ConnectionLost()
}

/** What a web_search_call did: a search, or a look at a page. */
function openAISearch(action: any): { query: string; sources: WebSource[] } {
  const url = typeof action?.url === 'string' ? action.url : undefined
  const sources: WebSource[] = (action?.sources ?? []).filter((s: any) => typeof s?.url === 'string').map((s: any) => ({ url: s.url }))
  if (url) sources.push({ url })
  const queries: string[] = Array.isArray(action?.queries) && action.queries.length ? action.queries : typeof action?.query === 'string' ? [action.query] : []
  return { query: queries.join(', ') || (typeof action?.pattern === 'string' ? action.pattern : url ?? ''), sources }
}

function wire(b: ContentBlock): unknown {
  if (b.type === 'web_search') return { type: 'text', text: searchNote(b) }
  if (b.type === 'tool_use') {
    const input = toolArguments(b.input) ?? {}
    return { type: 'tool_use', id: b.id, name: b.name, input }
  }
  if (b.type === 'tool_result' && b.images?.length) {
    const { images, ...rest } = b
    return { ...rest, content: [{ type: 'text', text: b.content },
      ...images.map((i) => ({ type: 'image', source: { type: 'base64', media_type: i.media_type, data: i.data } }))] }
  }
  return b
}

// ---- Agent -------------------------------------------------------------------

/** The rules and working examples the model gets (the same text as the macOS
 *  app's Agent; its default.nix and _outputs.fbs examples are the bundled
 *  demos' own files). */
export const SYSTEM_PROMPT = systemPrompt
/** The guide's rules (agent/Shared/guide-prompt.md). */
export const GUIDE_PROMPT = guidePrompt

/** A version's summary, in the page's language, from what it changed
 *  (agent/Shared/version-summary-prompt.md). One line, however the model answers. */
export async function summarizeVersion(c: ProviderConfig, input: string, signal: AbortSignal): Promise<string> {
  const system = versionSummaryPrompt.replace('{language}', 'Japanese (plain form, 常体)')
  let text = ''
  for await (const e of stream(c, system, [{ role: 'user', content: [{ type: 'text', text: input }] }], [], signal)) if (e.t === 'text') text += e.text
  return text.replace(/\s+/g, ' ').trim().slice(0, 300)
}
/** The built-in skill $demo (agent/Shared/demo-skill.md): Tsugumi of the chat shows the screen around. */
export const DEMO_SKILL = demoSkill

/** The bundled demos the Agent may read, as references ("demos/<path>"):
 *  not the Draft demos (tasks to implement) nor the finished demo of each,
 *  which is that task's answer: an Agent that read it would copy it, and
 *  implementing the Draft would show nothing. */
export function referenceDemos(demos: string): Record<string, string> {
  let list: { path?: string }[]
  try {
    list = JSON.parse(fs.readFileSync(path.join(demos, 'index.json'), 'utf8')).demos ?? []
  } catch {
    return {}
  }
  const paths = list.map((d) => d.path).filter((p): p is string => typeof p === 'string')
  const tasks = new Set(paths.filter((p) => p.endsWith('-draft')))
  const out: Record<string, string> = {}
  for (const p of paths) if (!tasks.has(p) && !tasks.has(p + '-draft')) out['demos/' + p] = path.join(demos, p)
  return out
}

/** A helper started by `delegate`: its name, the root-level Cards it owns
 *  and its task. Its saves may change only those Cards' definitions. */
export interface Helper { name: string; cards: string[]; instruction: string; /** How the helpers meet ("runs": "together"): it writes and checks but never runs; the main Agent runs once after all have reported. */ runsTogether?: boolean }

const DELEGATE_TOOL: ToolSpec = { ...AGENT.delegateTool, payloadType: -1, object: -1, mutating: false }
/** How long a helper's save or run waits for the others' (busy), ms. */
const HELPER_WAIT = 900_000
const HELPER_STEPS = 40

/** The tasks of a delegate call, or why they cannot be done. */
export function delegateTasks(input: string): Helper[] | string {
  let raw: unknown
  let runs: unknown = 'each'
  try {
    const parsed = JSON.parse(input)
    raw = parsed?.tasks
    runs = parsed?.runs ?? 'each'
  } catch {}
  if (!Array.isArray(raw)) return 'tasks is not a list of tasks'
  if (raw.length < 2 || raw.length > 8) return `${raw.length} tasks`
  if (runs !== 'each' && runs !== 'together') return `runs is ${String(runs)}`
  const helpers: Helper[] = []
  const owner = new Map<string, string>()
  for (const t of raw as Record<string, unknown>[]) {
    const name = String(t?.name ?? '').trim()
    const cards = Array.isArray(t?.cards) ? (t.cards as unknown[]).map((c) => String(c).trim()) : []
    const instruction = String(t?.instruction ?? '')
    if (!name || name.length > 40 || !/^[\p{L}\p{N}_-]+$/u.test(name)) return `the name "${name}" (letters, digits, - and _)`
    if (helpers.some((h) => h.name === name)) return `two tasks named ${name}`
    if (!cards.length || !instruction) return `${name} has no Cards or no instruction`
    // A helper sees nothing else: a placeholder here is not filled in by anyone.
    if (/__[A-Z][A-Z_]*__/.test(instruction)) return `the instruction of ${name} has a placeholder; write each instruction out in full`
    for (const c of cards) {
      if (!c || c.includes('/')) return `${c} is not a root-level Card id`
      if (owner.has(c)) return `${c} is in both ${owner.get(c)} and ${name}`
      owner.set(c, name)
    }
    helpers.push({ name, cards, instruction, runsTogether: runs === 'together' })
  }
  return helpers
}

/** What a save's changes touch beyond a helper's Cards (undefined: nothing). */
export function helperOutside(h: Helper, changes: unknown): string | undefined {
  if (!Array.isArray(changes)) return undefined
  const out: string[] = []
  for (const c of changes as { value_type?: string; value?: Record<string, unknown> }[]) {
    const kind = c?.value_type ?? '?'
    const v = c?.value ?? {}
    if (kind === 'PutDefinition') {
      const id = String(v.id ?? '')
      if (!h.cards.includes(id)) out.push(`the definition ${id}`)
    } else if (kind === 'PutSource' || kind === 'PatchSource' || kind === 'RemoveSource') {
      const p = String(v.path ?? '')
      const parts = p.split('/')
      if (!(parts.length >= 3 && parts[0] === 'definitions' && h.cards.includes(parts[1]) && !parts.includes('..'))) out.push(p || kind)
    } else {
      out.push(kind)
    }
  }
  return out.length ? out.join(', ') : undefined
}

/** How much of a Card that feeds a helper's the helper gets to read, in bytes:
 *  one file, and all of them together; longer ones it gets by name. */
const BRIEF_FILE_LIMIT = 8000
const BRIEF_FILES_LIMIT = 20_000

/** What a helper starts from, in place of reading the whole project: of a
 *  ProjectSnapshot's value, its Cards, the wires at them, the Cards that feed
 *  them (through the ones that only hand a value on, such as a fork) with the
 *  types and code of those, and who reads their outputs. As the Swift Agent's
 *  (tests/fixtures/agent-behavior.json). */
export async function helperBrief(h: Helper, snapshot: any, read: (path: string) => Promise<string | undefined>): Promise<string> {
  const wires: { from?: { card?: string; port?: string }; to?: { card?: string; port?: string } }[] = parsed(snapshot.project_json)?.canvas?.connections ?? []
  const definitions: any[] = snapshot.definitions ?? []
  const definition = (id: string) => definitions.find((d) => d.id === id)
  const files = (id: string): any[] => (snapshot.sources ?? []).filter((f: any) => f.kind === 'file' && f.exists !== false && String(f.path).startsWith(`definitions/${id}/`))
  const name = (end?: { card?: string; port?: string }) => `${end?.card}.${end?.port}`
  const ports = (list: any[]) => list.map((p) => `${p.name} (${p.kind} ${p.root_type}${p.optional ? ', optional' : ''})`).join(', ') || 'none'
  const about = (id: string) => {
    const d = definition(id)
    if (!d) return [`## ${id}`, 'It is not in the project yet.']
    const out = [`## ${id}`, '_card.json:', String(d.card_json ?? '').trim()]
    if (d.meta_json) out.push('_meta.json:', String(d.meta_json).trim())
    const r = (snapshot.resolution ?? []).find((c: any) => c.card?.segments?.length === 1 && c.card.segments[0] === id)
    if (r) {
      out.push(`ports: in ${ports(r.inputs ?? [])}; out ${ports(r.outputs ?? [])}`)
      const problems = (r.diagnostics ?? []).filter((x: any) => x.code !== 'draft').map((x: any) => `${x.code}: ${x.message}`)
      if (problems.length) out.push(`problems: ${problems.join('; ')}`)
    }
    return out
  }
  const lines: string[] = ['# Your Cards']
  for (const id of h.cards) {
    lines.push(...about(id))
    const at = wires.filter((w) => w.from?.card === id || w.to?.card === id).map((w) => `${name(w.from)} -> ${name(w.to)}`)
    if (at.length) lines.push(`wires: ${at.join(', ')}`)
    const own = files(id).map((f) => `${f.path} (hash ${f.hash})`)
    if (own.length) lines.push(`files: ${own.join(', ')}`)
  }
  // Back along the wires, to the Cards that make the values.
  const feeding: string[] = []
  const back = (id: string) => {
    for (const w of wires) {
      const from = w.to?.card === id ? w.from?.card : undefined
      if (!from || h.cards.includes(from) || feeding.includes(from)) continue
      feeding.push(from)
      if (!files(from).length && parsed(definition(from)?.card_json)?.template !== 'draft') back(from)
    }
  }
  h.cards.forEach(back)
  let left = BRIEF_FILES_LIMIT
  if (feeding.length) lines.push('', '# What feeds them')
  for (const id of feeding) {
    lines.push(...about(id))
    for (const f of files(id)) {
      // (what _card.json and _meta.json say is above, and a Card's build is its own)
      const rel = String(f.path).slice(`definitions/${id}/`.length)
      if (rel === 'default.nix') continue
      const size = Number(f.size_bytes ?? 0)
      const text = size <= BRIEF_FILE_LIMIT && size <= left ? await read(f.path) : undefined
      if (text === undefined) lines.push(`--- ${f.path} (${size} bytes, not shown: read it by its path)`)
      else {
        left -= size
        lines.push(`--- ${f.path}`, text.replace(/\n$/, ''))
      }
    }
  }
  const readers = wires.filter((w) => h.cards.includes(w.from?.card ?? '') && !h.cards.includes(w.to?.card ?? ''))
  if (readers.length) lines.push('', '# What reads their outputs')
  for (const w of readers) {
    const meta = parsed(definition(w.to?.card ?? '')?.meta_json)
    const port = (meta?.inputs ?? []).find((p: any) => p?.name === w.to?.port)?.description
    lines.push(`${name(w.to)}${port ? `: ${port}` : ''} (${w.to?.card}${meta?.description ? `: ${meta.description}` : ''})`)
  }
  return fill(AGENT.helper.brief, { rev: String(snapshot.rev ?? ''), project: lines.join('\n') })
}

function parsed(json: unknown): any {
  try { return JSON.parse(String(json)) } catch { return undefined }
}

/** Whether a read refused for naming a revision gone by goes again at the latest: takes `at_rev` away. */
export function readsLatestInstead(mutating: boolean, failure: string | undefined, args: Record<string, unknown>): boolean {
  if (mutating || failure !== 'revision_conflict' || !('at_rev' in args)) return false
  delete args.at_rev
  return true
}

function failureOf(json: string): { kind?: string; currentRev?: string } {
  try {
    const o = JSON.parse(json)
    if (o?.reply !== 'Failure') return {}
    const r = o.value?.current_rev
    return { kind: o.value?.kind, currentRev: r === undefined ? undefined : String(r) }
  } catch {
    return {}
  }
}

/** reference_read: Tsugumi's bundled references, read only. */
const REFERENCE_TOOL: ToolSpec = { ...AGENT.referenceTool, payloadType: -1, object: -1, mutating: false }

/** What an agent outside the web UI (Claude Code through the plugin) may call:
 *  the core's tools and reference_read. Not attached_folder_read (folders are
 *  attached to a chat), delegate or guide_show (those belong to the chat). */
export function externalTools(catalog: ToolCatalog): ToolSpec[] {
  return [...catalog.tools.filter((t) => t.name !== 'attached_folder_read'), REFERENCE_TOOL]
}
const EXTERNAL_TOOL_NAMES = (catalog: ToolCatalog) => new Set(externalTools(catalog).map((t) => t.name))
const GUIDE_SHOW_TOOL: ToolSpec = { ...AGENT.guide.showTool, payloadType: -1, object: -1, mutating: false }
const GUIDE_TOOLS = new Set<string>(AGENT.guide.tools)
type GuideText = Exclude<keyof typeof AGENT.guide, 'tools' | 'showTool'>
/** A note of the app to the guide (agent/Shared/agent.json "guide"), filled in. */
export const guideText = (k: GuideText, values: Record<string, string> = {}) => fill(AGENT.guide[k], values)

/** What an Agent is for: the project's Agent (it reads, saves and runs) or
 *  the guide bird of the app (it reads and explains; guide_show offers the
 *  user buttons, and nothing else of the project changes). */
export type AgentRole = 'worker' | 'guide'

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

/** What one image may be, in base64 characters (the Claude API's limit). */
const IMAGE_LIMIT = 10_000_000
/** Told in the text of a result whose images the provider did not accept. */
const IMAGE_REFUSED = '\n[the model service did not accept the image of this result, so it is not shown]'

/** A tool's result without the data of its images, and those images. Byte
 *  fields come as {"base64": ...}: PNG and JPEG data among them leaves the
 *  text, whole or not (a cut image is no use to read). */
export function splitImages(result: string): { text: string; images: ToolImage[] } {
  if (!result.includes('"base64":')) return { text: result, images: [] }
  let parsed: unknown
  try { parsed = JSON.parse(result) } catch { return { text: result, images: [] } }
  const images: ToolImage[] = []
  let found = false
  const replacing = (v: unknown): unknown => {
    if (Array.isArray(v)) return v.map(replacing)
    if (!v || typeof v !== 'object') return v
    const o = v as Record<string, unknown>, keys = Object.keys(o)
    if (keys.length === 1 && typeof o.base64 === 'string') {
      const png = o.base64.startsWith('iVBORw0KGgo'), jpeg = o.base64.startsWith('/9j/')
      if (png || jpeg) {
        found = true
        const bytes = Buffer.from(o.base64, 'base64')
        const about = `${png ? 'PNG' : 'JPEG'} image, ${bytes.length} bytes`
        // Whether the data runs to its end marker (a read can stop in the middle).
        const whole = png ? bytes.length >= 12 && bytes.subarray(-8, -4).toString('latin1') === 'IEND'
          : bytes.length >= 2 && bytes[bytes.length - 2] === 0xFF && bytes[bytes.length - 1] === 0xD9
        if (!whole) return `${about}: only part of it, not shown`
        if (o.base64.length > IMAGE_LIMIT) return `${about}: too large to show`
        images.push({ media_type: png ? 'image/png' : 'image/jpeg', data: o.base64 })
        return `${about}: shown with this result`
      }
    }
    return Object.fromEntries(keys.sort().map((k) => [k, replacing(o[k])]))
  }
  const out = replacing(parsed)
  return found ? { text: JSON.stringify(out), images } : { text: result, images: [] }
}

const FOLDER_ENTRIES = 300
const FOLDER_FILE_LIMIT = 50_000

/** A reference folder in one call, so that the model need not walk it a level
 *  per reply: a Card's folder gives all its files, any other lists every file
 *  under it. Hidden files and symbolic links are left out. */
export function readFolder(root: string): string {
  const files: string[] = []
  const walk = (rel: string) => {
    for (const d of fs.readdirSync(path.join(root, rel), { withFileTypes: true })) {
      if (d.name.startsWith('.')) continue
      const child = rel ? `${rel}/${d.name}` : d.name
      if (d.isDirectory()) walk(child)
      else if (d.isFile()) files.push(child)
    }
  }
  walk('')
  // As Swift sorts them: by UTF-16 code units, whatever the locale.
  files.sort((a, b) => (a < b ? -1 : a > b ? 1 : 0))
  if (!files.includes('_card.json')) {
    const shown = files.slice(0, FOLDER_ENTRIES).join('\n')
    return files.length > FOLDER_ENTRIES ? `${shown}\n[${files.length - FOLDER_ENTRIES} more files]` : shown
  }
  return files.map((rel) => {
    const data = fs.readFileSync(path.join(root, rel))
    let text: string | undefined
    if (data.length <= FOLDER_FILE_LIMIT) {
      try { text = new TextDecoder('utf-8', { fatal: true }).decode(data) } catch {}
    }
    return `--- ${rel}\n${text ?? `(${data.length} bytes, not shown: read it by its path)`}\n`
  }).join('')
}

/** A tool's result as the model gets it: its images as images, its text clipped. */
export function resultBlock(id: string, result: string, isError: boolean): ContentBlock {
  const { text, images } = splitImages(result)
  return { type: 'tool_result', tool_use_id: id, content: clip(text), is_error: isError, ...(images.length ? { images } : {}) }
}

/** How much of a tool's result the model gets; a longer one says it was cut. */
/** What the model is told of arguments that are not a JSON object: to write
 *  them again, with the text it wrote (it may mend that instead of starting over). */
export const invalidArgumentsText = (input: string) => fill(AGENT.invalidArguments, { json: JSON.stringify({ INVALID_JSON: input }) })

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

/** What ends a turn when the conversation cannot be made to fit the model's
 *  context (the app says what to do: a new conversation). */
export const CONTEXT_FULL_TEXT = "the conversation no longer fits the model's context"

/** A provider's refusal of a request over the model's context: nothing of it
 *  was answered, and asking the same again gets the same answer. */
export function isContextOverflow(text: string): boolean {
  const t = text.toLowerCase()
  return ['prompt is too long', 'input is too long', 'exceed context limit', 'request_too_large', 'context_length_exceeded',
    'exceeds the context window', 'maximum context length'].some((m) => t.includes(m))
}

const overflowed = (e: unknown) => isContextOverflow(String((e as Error)?.message ?? e))

/** A tool's result cut to make room in the context (the whole result stays in
 *  its record, and in what the chat shows of it). */
const CONTEXT_CUT_NOTE = "\n[cut to fit the model's context: read it again with a narrower request]"

/** The tool results among these messages, cut to the same length so that
 *  together they take at most `room` characters. */
export function fitToolResults(messages: ChatMessage[], room: number): ChatMessage[] {
  const note = CONTEXT_CUT_NOTE.length
  const sizes = messages.flatMap((m) => m.content.flatMap((b) => (b.type === 'tool_result' ? [b.content.length] : [])))
  let cap = RESULT_LIMIT
  while (cap > 200 && sizes.reduce((sum, n) => sum + Math.min(n, cap + note), 0) > room) cap = Math.floor(cap / 2)
  return messages.map((m) => ({
    ...m,
    content: m.content.map((b) => (b.type === 'tool_result' && b.content.length > cap + note
      ? { ...b, content: b.content.slice(0, cap) + CONTEXT_CUT_NOTE } : b)),
  }))
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
  /** Each definition's _card.json as this Agent last saw it (inspect, its
   *  own saves): a save that meets only others' layout changes is sent again. */
  private seenDefinitions = new Map<string, string>()
  /** A helper's first message: its task's start, what the app read for it. */
  brief = ''
  /** The latest revision a helper knows, which its saves and runs start from. */
  helperRev?: string

  constructor(
    readonly catalog: ToolCatalog,
    private readonly rpc: () => Promise<RPCClient>,
    private provider: ProviderConfig,
    readonly storage: string,
    readonly id: string,
    /** Bundled references for reference_read: a top-level name ("demos",
     *  "sdk/python/tsugumi/__init__.py", "card-guide.md") -> where it is. */
    private readonly references: Record<string, string> = {},
    /** A helper started by `delegate` (undefined: the main Agent). */
    readonly helper?: Helper,
    readonly role: AgentRole = 'worker',
  ) {
    this.codec = new FlatJSON(catalog.schema)
    this.conversation = loadConversation(storage, id)
  }

  /** The RPC tools and the Agent's own (reference_read, when references
   *  exist; delegate, for the main Agent). The guide reads only, and offers
   *  buttons with guide_show. */
  get tools(): ToolSpec[] {
    const reference = Object.keys(this.references).length ? [REFERENCE_TOOL] : []
    if (this.role === 'guide') return [...this.catalog.tools.filter((t) => GUIDE_TOOLS.has(t.name)), ...reference, GUIDE_SHOW_TOOL]
    return [...(this.searches ? [WEB_SEARCH_TOOL] : []), ...this.catalog.tools, ...reference, ...(this.helper ? [] : [DELEGATE_TOOL, GUIDE_SHOW_TOOL])]
  }

  /** The main Agent may search, when the setting is on (the helpers work from
   *  what it tells them; the guide explains the app). */
  private get searches(): boolean {
    return this.role === 'worker' && !this.helper && searchesWeb(this.provider)
  }

  /** The rules, and for a helper its task. */
  get system(): string {
    if (this.role === 'guide') return GUIDE_PROMPT
    // The main Agent is Tsugumi of the chat: it also guides (guide_show, tours).
    if (!this.helper) return SYSTEM_PROMPT + '\n\n' + AGENT.guide.chatNote + (this.searches ? '\n\n' + AGENT.webSearch.note : '')
    const h = AGENT.helper, together = this.helper.runsTogether
    return SYSTEM_PROMPT + '\n\n' + fill(h.prompt, {
      name: this.helper.name, cards: this.helper.cards.join(', '), instruction: this.helper.instruction,
      running: together ? h.together : h.each, report: together ? h.togetherReport : h.eachReport })
  }

  /** One tool call from an agent outside the web UI (Claude Code through the
   *  plugin): the same checks, approvals and RPC as a model's call here. */
  async call(name: string, input: string, events: (e: AgentEvent) => void): Promise<{ text: string; isError: boolean; images?: ToolImage[] }> {
    if (!EXTERNAL_TOOL_NAMES(this.catalog).has(name)) return { text: `unknown tool ${name}`, isError: true }
    const r = await this.runTool(randomUUID(), name, input, events)
    return r.type === 'tool_result' ? { text: r.content, isError: r.is_error, images: r.images } : { text: '', isError: true }
  }

  /** The value of a read the app makes itself; a failure is thrown. */
  private async readValue(tool: string, args: Record<string, unknown>): Promise<any> {
    const spec = this.catalog.tool(tool)
    if (!spec) throw new Error(`unknown tool ${tool}`)
    const rpc = await this.rpc()
    const r = await rpc.callRaw(spec.payloadType, (b) => this.codec.build(spec.object, args, b))
    const type = this.catalog.replyNames.get(r.type) ?? 'Unknown'
    const reply = JSON.parse(this.renderReply(type, r.bb, r.table))
    if (type === 'Failure') throw new Error(`${tool}: ${reply.value?.kind} ${reply.value?.diagnostic ?? ''}`)
    return reply.value
  }

  /** Runs the helpers of a delegate call at the same time and returns their
   *  reports (an error only when the call itself is wrong or none finished). */
  private async delegate(input: string, events: (e: AgentEvent) => void, signal?: AbortSignal): Promise<[string, boolean]> {
    const tasks = delegateTasks(input)
    if (typeof tasks === 'string') return [fill(AGENT.helper.invalid, { why: tasks }), true]
    // Each helper keeps its conversation next to this one's; a new call starts them afresh.
    const dir = this.storage.replace(/\.json$/, '') + '.helpers'
    fs.mkdirSync(dir, { recursive: true })
    // Each starts from what the app reads for it here, not from reading the project.
    let briefs: string[]
    let rev: string
    try {
      const snapshot = await this.readValue('project_inspect', {})
      rev = String(snapshot.rev)
      const read = async (p: string) => {
        const bytes = (await this.readValue('project_file_read', { path: p })).bytes
        return typeof bytes === 'string' ? bytes : undefined
      }
      briefs = await Promise.all(tasks.map((h) => helperBrief(h, snapshot, read)))
    } catch (e) {
      return [fill(AGENT.helper.unread, { why: String((e as Error).message ?? e) }), true]
    }
    // The user answers one approval at a time, whichever helper asks.
    let line: Promise<unknown> = Promise.resolve()
    const approver = this.approver
    const ask = approver ? (asked: string[]) => {
      const next = line.then(() => approver(asked))
      line = next.catch(() => {})
      return next
    } : undefined
    const reports = await Promise.all(tasks.map(async (h, i) => {
      const file = path.join(dir, h.name + '.json')
      fs.rmSync(file, { force: true })
      const a = new Agent(this.catalog, this.rpc, this.provider, file, `${this.id}.${h.name}`, this.references, h)
      a.maxSteps = HELPER_STEPS
      a.statusWait = this.statusWait
      a.folders = this.folders
      a.approver = ask
      a.brief = briefs[i]
      a.helperRev = rev
      return a.work((e) => events({ kind: 'helper', name: h.name, event: e }), signal)
    }))
    const u = this.conversation.usage
    for (const r of reports) {
      u.outputTokens += r.usage.outputTokens
      u.cacheReadTokens = (u.cacheReadTokens ?? 0) + (r.usage.cacheReadTokens ?? 0)
      u.cacheWriteTokens = (u.cacheWriteTokens ?? 0) + (r.usage.cacheWriteTokens ?? 0)
    }
    this.save()
    const text = tasks.map((h, i) => `## ${h.name} (${h.cards.join(', ')}): ${reports[i].outcome}\n${reports[i].report || '(no report)'}`).join('\n\n')
    const header = tasks.some((h) => h.runsTogether) ? AGENT.helper.reportsTogether : AGENT.helper.reportsEach
    // The helpers' saves moved the revision on: the next save or run starts from here.
    const now = await this.readValue('project_inspect', {}).then((v) => v.rev, () => undefined)
    const revision = now === undefined ? '' : ' ' + fill(AGENT.helper.revision, { rev: String(now) })
    return [header + revision + '\n\n' + text,
      !reports.some((r) => r.outcome === 'finished')]
  }

  /** A helper's whole task in one turn: its last words are its report. */
  async work(events: (e: AgentEvent) => void, outer?: AbortSignal) {
    this.conversation = newConversation(this.id)
    this.conversation.title = this.helper?.name
    this.conversation.messages.push({ role: 'user', content: [{ type: 'text', text: this.brief }] })
    try {
      this.save()
    } catch (e) {
      // Its conversation cannot be kept: this helper does not start.
      events({ kind: 'error', message: String((e as Error).message ?? e) })
      return { outcome: `failed: ${String((e as Error).message ?? e)}`, report: '', usage: this.conversation.usage }
    }
    const ac = new AbortController()
    this.abort = ac
    const stop = () => ac.abort()
    outer?.addEventListener('abort', stop)
    let outcome = 'stopped'
    await this.loop((e) => {
      if (e.kind === 'finished') outcome = 'finished'
      else if (e.kind === 'stepLimit') outcome = `stopped after ${e.steps} steps`
      else if (e.kind === 'error') outcome = e.message === 'cancelled' ? 'cancelled' : `failed: ${e.message}`
      events(e)
    }, ac.signal)
    outer?.removeEventListener('abort', stop)
    const last = [...this.conversation.messages].reverse().find((m) => m.role === 'assistant')
    const report = (last?.content ?? []).filter((b): b is Extract<ContentBlock, { type: 'text' }> => b.type === 'text').map((b) => b.text).join('\n')
    return { outcome, report, usage: this.conversation.usage }
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
      let refusals = 0
      let brokenReplies = 0
      for (let i = 0; i < this.maxSteps; i++) {
        if (signal.aborted) throw new Cancelled()
        await this.compactIfNeeded(events, signal)
        let reply: Awaited<ReturnType<Agent['modelTurn']>>
        try {
          reply = await this.modelTurn(events, signal)
          refusals = 0
        } catch (e) {
          if (signal.aborted || !overflowed(e)) throw e
          // Refused over the context, so it reported no usage to compact by, and
          // the same request would be refused for ever: it is made smaller first.
          if (++refusals > 3) {
            events({ kind: 'error', message: CONTEXT_FULL_TEXT })
            return
          }
          try {
            await this.compactIfNeeded(events, signal, true, this.provider.contextLimit >> refusals)
          } catch (e2) {
            if (signal.aborted || !overflowed(e2)) throw e2 // the summary too: with less room next
          }
          continue
        }
        let { blocks, stop, providerContent } = reply
        if (stop === 'max_tokens' || stop === 'model_context_window_exceeded') {
          // The reply was cut at the output limit or the context window, maybe
          // inside a tool call: nothing of it is executed; the model is asked
          // for smaller steps (and over the threshold, the context is compacted).
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
        // Nothing to say or do (declined before any output, or only thinking,
        // which is not kept). The API refuses empty content anywhere but at the
        // end, so the history holds a note, and the turn says what happened.
        const empty = blocks.length ? undefined : `the model sent an empty reply (stop reason: ${stop})`
        if (empty) blocks = [{ type: 'text', text: `[${empty}]` }]
        this.conversation.messages.push({ role: 'assistant', content: blocks, ...(providerContent ? { providerContent } : {}) })
        this.save()
        if (empty) {
          events({ kind: 'error', message: `${empty}; ask again, or in other words` })
          return
        }
        // The provider paused a long search turn: the reply goes back as it is,
        // with nothing added, and the same request goes on (one more step).
        if (stop === 'pause_turn') continue
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
        // Each try may be a whole file long: asked again a few times, as a cut reply is.
        if (uses.some((u) => !toolArguments(u.input)) && ++brokenReplies > 3) throw new Error("the arguments of the model's tool calls were broken JSON three times")
      }
      // Never a silent stop: the limit is its own outcome, and it can go on.
      events(done ? { kind: 'finished' } : { kind: 'stepLimit', steps: this.maxSteps, unconfirmedRun: this.unconfirmedRun })
    } catch (e) {
      if (signal.aborted || e instanceof Cancelled) {
        // Tools that were not sent are recorded as skipped; nothing is re-sent later.
        for (const t of this.conversation.tools) if (t.state === 'pending') t.state = 'skipped'
        try { this.save() }
        catch (error) { events({ kind: 'error', message: String(error) }); return }
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
      // Text and searches in the order they came; the sources the reply cites.
      const parts: ContentBlock[] = []
      const cited: WebSource[] = []
      let providerContent: Record<string, unknown>[] | undefined
      const flush = () => {
        if (text) parts.push({ type: 'text', text })
        text = ''
      }
      try {
        for await (const e of stream(this.provider, this.system, this.wireMessages(), this.tools, signal)) {
          if (e.t === 'text') {
            text += e.text
            events({ kind: 'text', text: e.text })
          } else if (e.t === 'toolStart') {
            tools.set(e.index, { id: e.id, name: e.name, json: '' })
            order.push(e.index)
          } else if (e.t === 'toolInput') {
            const t = tools.get(e.index)
            if (t) t.json += e.json
            if (t && (t.name === 'project_save' || t.name === 'project_validate')) events({ kind: 'toolInput', index: e.index, name: t.name, delta: e.json })
          } else if (e.t === 'search') {
            flush()
            parts.push({ type: 'web_search', id: e.id, query: e.query, sources: [] })
            events({ kind: 'toolStarted', id: e.id, name: AGENT.webSearch.name, arguments: JSON.stringify({ query: e.query }) })
          } else if (e.t === 'searchResult') {
            this.searchEnded(e.id, e.sources, e.error, parts, events)
          } else if (e.t === 'citation') {
            cited.push(e.source)
          } else if (e.t === 'providerContent') {
            providerContent = e.blocks
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
        const sources = sourcesText(cited)
        if (sources) {
          text += sources
          events({ kind: 'text', text: sources })
        }
        flush()
        const blocks: ContentBlock[] = parts
        for (const i of order) {
          const t = tools.get(i)!
          blocks.push({ type: 'tool_use', id: t.id, name: t.name, input: t.json || '{}' })
        }
        return { blocks, stop, providerContent }
      } catch (e) {
        if (signal.aborted) throw new Cancelled()
        const retryable = e instanceof HTTPStatusError ? e.retryable : e instanceof TypeError || e instanceof ConnectionLost || e instanceof TransientError
        // The provider refused a request with images (a model without vision, a
        // corrupt or oversized picture): asking again as it is would fail for good.
        if (((e instanceof HTTPStatusError && e.status === 400 && e.body.toLowerCase().includes('image')) ||
             (e instanceof ChatGPTError && e.kind === 'unsupported')) && this.dropToolImages()) {
          if (text || parts.length) events({ kind: 'discardText' })
          this.save()
          events({ kind: 'retrying', message: 'the model did not accept an image from a tool; going on without it' })
          continue
        }
        if (!retryable || attempt >= 3) throw e
        // The reply is asked again: what it had streamed goes.
        if (text || parts.length) events({ kind: 'discardText' })
        // The provider's retry-after when it gave one (rate limits), else backoff.
        const wait = Math.min(120, Math.max(e instanceof HTTPStatusError ? e.retryAfter ?? 0 : 0, 0.5 * (1 << (attempt + 1))))
        events({ kind: 'retrying', message: e instanceof HTTPStatusError || e instanceof TransientError
          ? `provider returned ${e.status}; retry ${attempt + 1} in ${Math.ceil(wait)} s` : `connection to the model failed; retry ${attempt + 1}` })
        await new Promise((r) => setTimeout(r, wait * 1000))
        continue
      }
    }
  }

  /** A request with an image was refused: without them the conversation goes
   *  on, and the model is told which results had one. */
  private dropToolImages(): boolean {
    let dropped = false
    for (const m of this.conversation.messages) m.content = m.content.map((b) => {
      if (b.type !== 'tool_result' || !b.images?.length) return b
      dropped = true
      const { images: _, ...rest } = b
      return { ...rest, content: b.content + IMAGE_REFUSED }
    })
    return dropped
  }

  /** A search's result: in this reply's blocks, or in an earlier reply's when
   *  it waited for our tools. Recorded at once: the query has left, whatever
   *  becomes of the reply. */
  private searchEnded(id: string, sources: WebSource[], error: string | undefined, parts: ContentBlock[], events: (e: AgentEvent) => void) {
    let block: Extract<ContentBlock, { type: 'web_search' }> | undefined
    for (const b of [...parts, ...this.conversation.messages.flatMap((m) => m.content)]) {
      if (b.type === 'web_search' && b.id === id) { block = b; break }
    }
    if (block) {
      block.sources = sources
      if (error) block.error = error
    }
    const summary = searchSummary(sources, error)
    this.conversation.tools = this.conversation.tools.filter((t) => t.id !== id)
    this.conversation.tools.push({ id, name: AGENT.webSearch.name, arguments: JSON.stringify({ query: block?.query ?? '' }), state: 'completed', result: summary, isError: !!error })
    this.save()
    events({ kind: 'toolFinished', id, name: AGENT.webSearch.name, ok: !error, summary })
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
    if (!top && !rel.split('/').includes('..')) {
      // A folder that only holds references ("demos"): its entries.
      const inside = names.filter((n) => n.startsWith(rel + '/')).map((n) => {
        const rest = n.slice(rel.length + 1)
        return rest.includes('/') || isDir(this.references[n]) ? rest.split('/')[0] + '/' : rest
      })
      if (inside.length) return [[...new Set(inside)].sort().join('\n'), false]
    }
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
    if (isDir(real)) return [readFolder(real), false]
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
        if (r?.state === 'completed') return resultBlock(id, r.result ?? '', !!r.isError)
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
    // Broken arguments never run, whatever the tool: the model is told to write
    // them again. (A reply cut at the output limit does not come this far: loop.)
    const args = toolArguments(input)
    if (!args) return this.finish(record, invalidArgumentsText(input), true, events)
    if (name === REFERENCE_TOOL.name && Object.keys(this.references).length) {
      const [text, isError] = this.readReference(String(args.path ?? ''))
      return this.finish(record, text, isError, events)
    }
    if (name === GUIDE_SHOW_TOOL.name && (this.role === 'guide' || !this.helper)) return this.finish(record, AGENT.guide.shown, false, events)
    if (this.role === 'guide') {
      if (!GUIDE_TOOLS.has(name)) return this.finish(record, guideText('readOnly', { tool: name }), true, events)
    }
    if (name === DELEGATE_TOOL.name && !this.helper && this.role === 'worker') {
      const [text, isError] = await this.delegate(input, events, signal)
      return this.finish(record, text, isError, events)
    }
    const spec = this.catalog.tool(name)
    if (!spec) return this.finish(record, `unknown tool ${name}`, true, events)
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
    // A helper saves only its own Cards.
    const outside = this.helper && name === 'project_save' ? helperOutside(this.helper, full.changes) : undefined
    if (this.helper && outside) {
      return this.finish(record, fill(AGENT.helper.outside, { name: this.helper.name, cards: this.helper.cards.join(', '), what: outside }), true, events)
    }
    // A helper told only to write leaves the run to the main Agent.
    if (this.helper?.runsTogether && name === 'run') {
      return this.finish(record, fill(AGENT.helper.runRefused, { name: this.helper.name }), true, events)
    }
    if (spec.mutating) {
      record.opId = record.opId ?? randomUUID()
      full.op_id = record.opId
      full.origin = 'agent'
      // A helper leaves base_rev to the app: the latest it knows, and a conflict goes on below.
      if (this.helper && this.helperRev !== undefined && (spec.inputSchema as any).properties?.base_rev) full.base_rev = this.helperRev
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
      // A read that named a revision gone by (the canvas tidied, or the
      // model's guess) reads the latest; its reply says which that is.
      if (readsLatestInstead(spec.mutating, failureOf(json).kind, full)) ({ type: replyType, json } = await call())
      // A save or run that met only changes it does not touch (the canvas
      // tidied, a Card dragged) goes on from the current revision.
      if (!this.helper && failureOf(json).kind === 'revision_conflict') {
        const rev = await this.rebased(name, full, rpc)
        if (rev !== undefined) {
          full.base_rev = rev
          record.opId = randomUUID()
          full.op_id = record.opId
          ;({ type: replyType, json } = await call())
        }
      }
      // Helpers save and run at the same time: one meeting another's run
      // (busy) waits for it to end, one meeting another's save
      // (revision_conflict) goes on from the current revision. A helper's
      // saves only touch its own Cards, so nothing of another is lost.
      if (this.helper && spec.mutating) {
        const deadline = Date.now() + HELPER_WAIT
        for (let f = failureOf(json); (f.kind === 'busy' || f.kind === 'revision_conflict') && Date.now() < deadline; f = failureOf(json)) {
          if (signal?.aborted) break
          if (f.kind === 'revision_conflict') {
            if (f.currentRev === undefined) break
            full.base_rev = f.currentRev
          } else {
            await new Promise((r) => setTimeout(r, 1000))
          }
          record.opId = randomUUID()
          full.op_id = record.opId
          ;({ type: replyType, json } = await call())
        }
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
      this.noteDefinitions(replyType, json)
      if (this.helper && (replyType === 'SaveResult' || replyType === 'ProjectSnapshot')) {
        const rev = parsed(json)?.value?.rev
        if (rev !== undefined) this.helperRev = String(rev)
      }
      return this.finish(record, json, replyType === 'Failure', events)
    } catch (e) {
      // Unknown outcome: keep the record as sent so a resume asks OperationGet.
      if (spec.mutating) {
        return this.finish(record, `the connection failed after sending; outcome unknown (op_id ${record.opId}). Use operation_get.`, true, events, true)
      }
      return this.finish(record, String((e as Error).message ?? e), true, events)
    }
  }

  /** Remembers the definitions a reply shows (an inspect's, a save's). */
  private noteDefinitions(reply: string, json: string) {
    if (reply !== 'ProjectSnapshot' && reply !== 'SaveResult') return
    let v: any
    try {
      v = JSON.parse(json).value
    } catch {
      return
    }
    if (reply === 'ProjectSnapshot') {
      this.seenDefinitions = new Map()
      for (const d of v?.definitions ?? []) if (typeof d?.id === 'string') this.seenDefinitions.set(d.id, d.card_json ?? '')
    } else {
      for (const d of v?.delta?.definitions_upsert ?? []) if (typeof d?.id === 'string') this.seenDefinitions.set(d.id, d.card_json ?? '')
      for (const id of v?.delta?.definitions_removed ?? []) this.seenDefinitions.delete(id)
    }
  }

  /** The current revision, when sending again changes nothing of what others
   *  did: a save that rewrites only definitions (and files, which the core
   *  checks by their base_hash) still as this Agent saw them; a run when no
   *  definition it saw has changed. undefined: the model sees the conflict. */
  private async rebased(tool: string, args: Record<string, unknown>, rpc: RPCClient): Promise<string | undefined> {
    const inspect = this.catalog.tool('project_inspect')
    if ((tool !== 'project_save' && tool !== 'run') || !this.seenDefinitions.size || !inspect) return undefined
    let ids: string[] = []
    if (tool === 'project_save') {
      if (!Array.isArray(args.changes)) return undefined
      for (const c of args.changes as { value_type?: string; value?: Record<string, unknown> }[]) {
        if (c?.value_type === 'PutDefinition') ids.push(String(c.value?.id ?? ''))
        else if (c?.value_type !== 'PutSource') return undefined  // the document, the layout, removals, renames: the model decides
      }
    } else {
      ids = [...this.seenDefinitions.keys()]
    }
    try {
      const r = await rpc.callRaw(inspect.payloadType, (b) => this.codec.build(inspect.object, {}, b))
      const type = this.catalog.replyNames.get(r.type) ?? 'Unknown'
      if (type !== 'ProjectSnapshot') return undefined
      const v = JSON.parse(this.renderReply(type, r.bb, r.table)).value
      const now = new Map<string, string>()
      for (const d of v?.definitions ?? []) if (typeof d?.id === 'string') now.set(d.id, d.card_json ?? '')
      for (const id of ids) if (now.get(id) !== this.seenDefinitions.get(id)) return undefined
      return v?.rev === undefined ? undefined : String(v.rev)
    } catch {
      return undefined
    }
  }

  private finish(record: ToolRecord, result: string, isError: boolean, events: (e: AgentEvent) => void, keepSent = false): ContentBlock {
    record.state = keepSent ? 'sent' : 'completed'
    record.result = result
    record.isError = isError
    this.conversation.tools = this.conversation.tools.filter((t) => t.id !== record.id)
    this.conversation.tools.push(record)
    this.save()
    events({ kind: 'toolFinished', id: record.id, name: record.name, ok: !isError, summary: toolSummary(result) })
    return resultBlock(record.id, result, isError)
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
            ? resultBlock(t.id, t.result!, error ?? true) : b)
      } catch {}
    }
    this.save()
  }

  /** The model's summary of these messages, in a request of about `room` characters at most. */
  private async summary(old: ChatMessage[], room: number, signal: AbortSignal): Promise<string> {
    let transcript = ''
    for (const m of old) {
      for (const b of m.content) {
        if (b.type === 'text') transcript += `${m.role}: ${b.text}\n`
        else if (b.type === 'tool_use') transcript += `tool call ${b.name} ${b.input.slice(0, 500)}\n`
        else if (b.type === 'web_search') transcript += `${searchNote(b)}\n`
        else transcript += `tool result${b.is_error ? ' (error)' : ''}: ${b.content.slice(0, 800)}\n`
      }
    }
    const earlier = this.conversation.summary ? `Earlier summary: ${this.conversation.summary}\n` : ''
    const text = earlier + transcript.slice(-room)
    const ask: ChatMessage[] = [{ role: 'user', content: [{ type: 'text', text: `Summarize this Tsugumi session so far in under 200 words, keeping revisions, run ids and decisions:\n\n${text}` }] }]
    let summary = ''
    // A short summary: neither the chosen effort nor Fast mode's price.
    for await (const e of stream({ ...this.provider, effort: undefined, fast: false }, 'You summarize conversations.', ask, [], signal)) if (e.t === 'text') summary += e.text
    if (signal.aborted) throw new Cancelled()
    if (!summary.trim()) throw new Error('the context summary was empty; the conversation was kept')
    return summary
  }

  /** Context compaction, when the last request approached the provider's context
   *  limit: older turns are replaced by a model-written summary (the full history
   *  stays archived), and the tool results of the turns kept are cut to `room`
   *  characters (half the limit by default), or what is kept could fill the context
   *  by itself. A summary that fails then ends nothing: the request may still fit,
   *  and if not its refusal comes back here with `force`, where the failure is the turn's. */
  private async compactIfNeeded(events: (e: AgentEvent) => void, signal: AbortSignal, force = false, room = this.provider.contextLimit / 2) {
    const c = this.conversation
    if (!force && c.usage.inputTokens <= (this.provider.contextLimit * 3) / 4) return
    const messages = c.messages.slice()
    const cut = messages.length > (force ? 4 : 6) ? compactionBoundary(messages) : 0
    const kept = fitToolResults(messages.slice(cut), room)
    const shrunk = kept.some((m, i) => m.content.some((b, j) => b !== messages[cut + i].content[j]))
    if (!cut && !shrunk) return
    events({ kind: 'compacting' })
    let summary = c.summary
    if (cut) {
      try {
        summary = await this.summary(messages.slice(0, cut), room, signal)
      } catch (e) {
        if (force || signal.aborted) throw e
        events({ kind: 'retrying', message: `the earlier turns could not be summarized (${String((e as Error).message ?? e)}); going on without it` })
        return
      }
    }
    if (c.messages.length !== messages.length) return
    c.summary = summary
    c.archive = [...c.archive, ...messages.slice(0, cut)]
    c.messages = kept
    c.usage.inputTokens = 0
    this.save()
    events({ kind: 'compacted' })
  }
}
