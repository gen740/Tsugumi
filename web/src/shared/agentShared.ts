// What the Swift and the web Agent share (agent/Shared/agent.json): the RPC
// requests offered as tools, the fields the Agent fills itself, its own
// reference tool and model options.
import shared from '../../../agent/Shared/agent.json'
import { L as localize } from '../i18n'

export function projectAccessNeededNow(request: string): boolean {
  try { return JSON.parse(request.slice('project_access '.length)).needed_now !== false } catch { return true }
}

/** Core supplies this description identically for live and restored requests. */
export function projectAccessLabel(request: string, L = localize): string {
  try {
    const v = JSON.parse(request.slice('project_access '.length))
    const actions: Record<string, string> = { Read: L('内容の読み取り'), Write: L('内容の変更'), Run: L('Card の実行'),
      CardPermissions: L('Card の権限の変更'), Shell: L('シェルコマンドの実行') }
    return L('{0}\n{1}\n許可する操作: {2}\n有効期間: {3}', v.title, v.path, actions[v.access] ?? L('アクセス'),
      v.scope === 'Server' ? L('取り消すまで') : L('この会話の間'))
  } catch { return request }
}

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
