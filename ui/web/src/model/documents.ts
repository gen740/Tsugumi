// The project documents as this UI edits them.
//
// - A project's layout.bin (protocol/layout_document.fbs) and its notes'
//   Markdown (notes/<id>.md) are one layout view, the JSON object hostd edits
//   under "layout.bin": {flow, cards: {key: place}, boundaries: {scope: bars},
//   boundary, notes: [{id, text, ...}], groups}.
// - A Card's README.md (front matter `title` and `description`, Markdown body) and its
//   _comments.json are one note object: {title, description, content, comments}.
import { ByteBuffer } from 'flatbuffers'
import { Layout } from '../../shared/gen/tsugumi/layout/layout'
import { arr, get, isObj, J, JObj, num, obj, str, tryParse } from '../../shared/json'

const placeKeys = ['x', 'y', 'collapsed', 'inputOrder', 'outputOrder'] as const
const noteKeys = ['id', 'scope', 'color', 'x', 'y', 'width', 'height', 'fontSize'] as const

/** One value as JSON: numbers keep their value, absent ones are left out. */
function value(v: unknown): J | undefined {
  if (v === null || v === undefined) return undefined
  if (typeof v === 'number') return num(v)
  if (typeof v === 'boolean' || typeof v === 'string') return v
  if (Array.isArray(v)) return v.length ? v.map(x => value(x) ?? null) : undefined
  return undefined
}
function fields(source: Record<string, unknown>, keys: readonly string[]): JObj {
  const out = obj()
  for (const k of keys) {
    const v = value(source[k])
    if (v !== undefined && v !== '') out.set(k, v)
  }
  return out
}

/** The layout view of a layout.bin and its notes' text ({} when there is none). */
export function layoutView(bytes: Uint8Array | null | undefined, notes: Map<string, string>): JObj {
  const view = obj()
  if (!bytes?.length) return view
  const l = Layout.getRootAsLayout(new ByteBuffer(bytes)).unpack()
  if (l.flow) view.set('flow', String(l.flow))
  const cards = obj()
  for (const c of l.cards) if (c.key) cards.set(String(c.key), fields(c as unknown as Record<string, unknown>, placeKeys))
  if (cards.size) view.set('cards', cards)
  const bars = (b: { inputs: { x: number | null, y: number | null } | null, outputs: { x: number | null, y: number | null } | null }) => {
    const out = obj()
    for (const side of ['inputs', 'outputs'] as const) {
      const p = b[side]
      if (p) out.set(side, fields(p as unknown as Record<string, unknown>, ['x', 'y']))
    }
    return out
  }
  const boundaries = obj()
  for (const b of l.boundaries) if (b.scope !== null) boundaries.set(String(b.scope), bars(b))
  if (boundaries.size) view.set('boundaries', boundaries)
  if (l.boundary) view.set('boundary', bars(l.boundary))
  if (l.notes.length) {
    view.set('notes', l.notes.map(n => {
      const note = fields(n as unknown as Record<string, unknown>, noteKeys)
      if (n.scope !== null) note.set('scope', String(n.scope))
      const text = n.id !== null ? notes.get(String(n.id)) : undefined
      if (text !== undefined) note.set('text', text)
      return note
    }))
  }
  if (l.groups.length) {
    view.set('groups', l.groups.map(g => {
      const group = fields(g as unknown as Record<string, unknown>, ['id', 'scope', 'name', 'color', 'cards'])
      if (g.outputs.length) group.set('outputs', g.outputs.map(o => obj([['card', String(o.card ?? '')], ['port', String(o.port ?? '')]])))
      return group
    }))
  }
  return view
}

/** The note id of a project's notes/<id>.md, if the path is one. */
export function noteId(prefix: string, path: string): string | undefined {
  if (!path.startsWith(prefix + 'notes/') || !path.endsWith('.md')) return undefined
  const id = path.slice(prefix.length + 6, -3)
  return /^[A-Za-z0-9_.-]{1,128}$/.test(id) && id !== '.' && id !== '..' ? id : undefined
}

// ---- README.md ----------------------------------------------------------------------

export interface Readme { title: string; description: string; body: string }

/** A Card's required README front matter and its Markdown body. */
export function parseReadme(text: string): Readme | undefined {
  if (!text.startsWith('---\n')) return undefined
  const end = text.indexOf('\n---\n', 3)
  if (end < 0) return undefined
  const fields = new Map<string, string>()
  for (const line of text.slice(4, end).split('\n')) {
    const match = /^(title|description):(?: ([^\r]*))?$/.exec(line)
    if (!match || fields.has(match[1])) return undefined
    fields.set(match[1], match[2] ?? '')
  }
  const title = fields.get('title'), description = fields.get('description')
  if (!title?.trim() || description === undefined) return undefined
  return { title, description, body: text.slice(end + 5).replace(/^\n/, '') }
}

/** The two front matter values occupy one line each. */
export function printReadme(r: Readme): string {
  const line = (value: string) => value.replace(/\s*[\r\n]+\s*/g, ' ')
  return `---\ntitle: ${line(r.title)}\ndescription: ${line(r.description)}\n---\n` + (r.body ? '\n' + r.body : '')
}

// ---- A Card's note object --------------------------------------------------------------

/** {title, description, content, comments} from a Card's README.md and _comments.json. */
export function cardNotes(readme: string | null | undefined, commentsJson: string | null | undefined): J | undefined {
  const out = obj()
  if (readme != null) out.set('readmeText', readme)
  const r = readme ? parseReadme(readme) : undefined
  if (r) {
    out.set('title', r.title)
    out.set('description', r.description)
    if (r.body) out.set('content', r.body)
  }
  const comments = arr(get(commentsJson ? tryParse(commentsJson) : undefined, 'comments'))
  if (comments?.length) out.set('comments', comments)
  return out.size ? out : undefined
}

/** A note object as the README.md text and the _comments.json document it is. */
export function splitCardNotes(notes: J | undefined): { readme?: string; comments?: J } {
  if (!isObj(notes)) return {}
  const original = str(notes.get('readmeText'))
  const saved = original === undefined ? undefined : parseReadme(original)
  const title = str(notes.get('title')), description = str(notes.get('description')) ?? '', body = str(notes.get('content')) ?? ''
  // The write guard compares the exact original bytes; both front matter orders are valid.
  const readme = title === undefined || saved?.title === title && saved.description === description && saved.body === body
    ? original : printReadme({ title, description, body })
  const list = arr(notes.get('comments'))
  return { readme, comments: list?.length ? obj([['comments', list]]) : undefined }
}
