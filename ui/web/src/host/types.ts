import type { ChatRow } from '../../shared/chatPresentation'
// Presentation DTOs of the Agent the page's host adapter assembles.
export type ContentBlock =
  | { type: 'text'; text: string }
  | { type: 'tool_use'; id: string; name: string; input: string }
  | { type: 'tool_result'; tool_use_id: string; content: string; is_error: boolean; images?: ToolImage[] }
  /** A search the provider ran: what was asked and what came back. */
  | { type: 'web_search'; id: string; query: string; sources: WebSource[]; error?: string }

export interface WebSource { url: string; title?: string }

/** An image in a tool's result (a PNG read from
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
  rows: ChatRow[]
  helpers?: { call_id: string; task: number; messages: ChatMessage[]; state: string }[]
  /** inputTokens: context of the last request (uncached, cache read and cache
   *  written); the cache counts add up over the conversation. */
  usage: { inputTokens: number; outputTokens: number; cacheReadTokens?: number; cacheWriteTokens?: number }
  summary?: string
}


// ---- Provider ------------------------------------------------------------------

export interface ProviderConfig {
  purpose?: string
  name: string
  model: string
  /** Wire protocol of the endpoint. */
  api: 'anthropicMessages' | 'openAIResponses'
  /** How requests are authorized: none (the local fixture), an API key, or
   *  the user's ChatGPT plan (Sign in with ChatGPT). */
  auth: 'none' | 'apiKey' | 'chatgpt'
  /** Opaque references to settings, accounts in hostd. */
  profile?: string
  accountProfile?: string
  /** The chat's choices in the model menu, already checked against the model
   *  (modelChoices). None for the requests that are not the Agent's work. */
  usesChoices?: boolean
  effort?: string
  fast?: boolean
}
