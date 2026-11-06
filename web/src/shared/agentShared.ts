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
