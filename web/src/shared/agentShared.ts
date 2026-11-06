// What the Swift and the web Agent share (agent/Shared/agent.json): the RPC
// requests offered as tools, the fields the Agent fills itself, its own
// reference tool and the request that has Draft Cards implemented.
import shared from '../../../agent/Shared/agent.json'

export const AGENT = shared

/** What a connection offers for a model (agent/Shared/agent.json, from the
 *  providers' documents). A model not listed offers nothing: nothing is sent
 *  that the provider might refuse. */
export interface ModelOptions {
  efforts: string[]
  /** The level the model uses when none is sent, when its documents say. */
  defaultEffort?: string
  /** Fast mode's price as a multiple of the normal one; none when not offered. */
  fastPrice?: number
}

export function modelOptions(connection: string, model: string): ModelOptions {
  const c = (shared.modelOptions.connections as Record<string, { effort: { default: string | null; levels: string[]; models: string[] }[]; fast: Record<string, number> } | undefined>)[connection]
  const group = c?.effort.find((g) => g.models.includes(model))
  return { efforts: group?.levels ?? [], defaultEffort: group?.default ?? undefined, fastPrice: c?.fast[model] }
}

/** What goes into the chat's requests: the chosen level if the model takes it,
 *  and Fast mode for the model it was turned on for. */
export function modelChoices(connection: string, model: string, effort: string, fastModel: string): { effort?: string; fast: boolean } {
  const o = modelOptions(connection, model)
  return { effort: o.efforts.includes(effort) ? effort : undefined, fast: o.fastPrice !== undefined && fastModel === model }
}

/** `{name}` in a text replaced by its value, in one pass (values are not read again). */
export function fill(text: string, values: Record<string, string>): string {
  return text.replace(/\{(\w+)\}/g, (m, k: string) => values[k] ?? m)
}

/** The request that has the model implement Draft Cards of a scope. */
export function draftRequest(scope: string[], cards: { key: string; definition: string; description: string; content: string; context: string }[]): string {
  const t = shared.draftRequest
  const list = cards.map((c) => fill(t.card, { ...c })).join('\n')
  const where = scope.length === 0 ? t.root : fill(t.child, { scope: scope.join('/') })
  return fill(t.text, { where, list })
}

/** The request that has the model follow the user's comments on Cards of a scope. */
export function commentRequest(scope: string[], cards: { key: string; definition: string; comments: { id: string; text: string }[] }[]): string {
  const t = shared.commentRequest, d = shared.draftRequest
  const list = cards.map((c) => fill(t.card, {
    key: c.key, definition: c.definition, comments: c.comments.map((x) => fill(t.comment, { ...x })).join('\n'),
  })).join('\n')
  const where = scope.length === 0 ? d.root : fill(d.child, { scope: scope.join('/') })
  return fill(t.text, { where, list })
}

/** What a tool's row in the chat shows of its result: its start, after what a
 *  save did to the comments on Cards (the result's end, which the start cuts off). */
export function toolSummary(result: string): string {
  let comments: unknown
  if (result.startsWith('{"reply":"SaveResult"') && result.includes('"comments"')) {
    try {
      comments = JSON.parse(result)?.value?.comments
    } catch {}
  }
  const head = Array.isArray(comments) && comments.length ? `comments: ${JSON.stringify(comments)}\n` : ''
  return head + result.slice(0, 400)
}

/** A change to a comment, as a save's result and the chat row have it. */
export interface SavedComment { card: string; text: string; change: string; by?: string; resolution?: string; cards?: string[] }

/** The comments a tool row's text says its save changed. */
export function savedComments(rowText: string): SavedComment[] {
  const m = /\n→ comments: (.*)\n/.exec(rowText)
  if (!m) return []
  try {
    return JSON.parse(m[1]) as SavedComment[]
  } catch {
    return []
  }
}
