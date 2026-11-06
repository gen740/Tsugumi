// What the Swift and the web Agent share (agent/Shared/agent.json): the RPC
// requests offered as tools, the fields the Agent fills itself, its own
// reference tool and the request that has Draft Cards implemented.
import shared from '../../../agent/Shared/agent.json'

export const AGENT = shared

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
