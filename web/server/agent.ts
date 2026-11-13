// Presentation DTOs and schema reflection. Agent execution lives in core.
import { BaseType, BFBS, Field } from '../src/shared/bfbs'
import { AGENT, fill } from '../src/shared/agentShared'

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


export const toolName = (r: string) => r.replace('Request', '').replace(/(?<!^)([A-Z])/g, '_$1').toLowerCase()

export class ToolCatalog {
  readonly tools: ToolSpec[]
  readonly replyNames = new Map<number, string>()

  constructor(readonly schema: BFBS, requests = DEFAULT_REQUESTS, namespace = 'Tsugumi.RPC') {
    const union = schema.enums.find((e) => e.name === `${namespace}.RequestPayload`)
    if (!union) throw new Error('RequestPayload union not found')
    const reply = schema.enums.find((e) => e.name === `${namespace}.ReplyPayload`)
    for (const v of reply?.values ?? []) this.replyNames.set(v.value, v.name)
    this.tools = requests.map((r) => {
      const index = schema.object(`${namespace}.${r}`)
      const v = union.values.find((x) => x.unionType?.index === index)
      if (index < 0 || !v) throw new Error(`request ${r} not found`)
      const obj = schema.objects[index]
      const doc = obj.documentation.join(' ').trim()
      return {
        name: toolName(r), payloadType: v.value, object: index,
        description: `${r.replace('Request', '')}: ${doc}`,
        inputSchema: this.objectSchema(index, 0, namespace === 'Tsugumi.RPC' ? new Set([...AGENT_FIELDS, ...(NOT_FOR_MODELS[r] ?? [])]) : new Set()),
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
  | { kind: 'permission'; id: string; asked: string[]; from?: string }
  | { kind: 'permissionAnswered'; id: string }

// ---- Provider ------------------------------------------------------------------

export interface ProviderConfig {
  name: string
  model: string
  /** Wire protocol of the endpoint. */
  api: 'anthropicMessages' | 'openAIResponses'
  /** How requests are authorized: none (the local fixture), an API key, or
   *  the user's ChatGPT plan (Sign in with ChatGPT). */
  auth: 'none' | 'apiKey' | 'chatgpt'
  /** Opaque references to settings, accounts and ephemeral keys in agentd. */
  profile?: string
  accountProfile?: string
  keyProfile?: string
  /** The chat's choices in the model menu, already checked against the model
   *  (modelChoices). None for the requests that are not the Agent's work. */
  usesChoices?: boolean
  effort?: string
  fast?: boolean
}

type GuideText = Exclude<keyof typeof AGENT.guide, 'tools' | 'showTool' | 'controls'>
export const guideText = (key: GuideText, values: Record<string, string> = {}) => fill(AGENT.guide[key], values)
