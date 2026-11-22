// The project documents as this UI edits them.
//
// - A project's layout.bin (protocol/layout_document.fbs) and its notes'
//   Markdown (notes/<id>.md) are one layout view, the JSON object hostd edits
//   under "layout.bin": {flow, cards: {key: place}, boundaries: {scope: bars},
//   boundary, notes: [{id, text, ...}], groups}.
// - A Card's README.md (front matter `description`, Markdown body) and its
//   _comments.json are one note object: {description, content, comments}.
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

export interface Readme { description: string; body: string }

/** A README.md: its front matter's description and its body. */
export function parseReadme(text: string): Readme {
  if (!text.startsWith('---\n')) return { description: '', body: text }
  const end = text.indexOf('\n---\n', 3)
  if (end < 0) return { description: '', body: text }
  let description = ''
  for (const line of text.slice(4, end).split('\n')) {
    const colon = line.indexOf(':')
    if (colon > 0 && line.slice(0, colon) === 'description') description = line.slice(colon + 1).replace(/^ /, '')
  }
  let body = text.slice(end + 5)
  if (body.startsWith('\n')) body = body.slice(1)
  return { description, body }
}

/** The text of a README.md; undefined when it says nothing. The description
 *  is one line: line breaks become spaces. */
export function printReadme(r: Readme): string | undefined {
  const description = r.description.replace(/\s*\n\s*/g, ' ').trim()
  if (!description && !r.body) return undefined
  const front = description ? `---\ndescription: ${description}\n---\n` : r.body.startsWith('---\n') ? '---\n---\n' : ''
  return front + (front && r.body ? '\n' : '') + r.body
}

// ---- A Card's note object --------------------------------------------------------------

/** {description, content, comments} from a Card's README.md and _comments.json. */
export function cardNotes(readme: string | null | undefined, commentsJson: string | null | undefined): J | undefined {
  const out = obj()
  if (readme) {
    const r = parseReadme(readme)
    if (r.description) out.set('description', r.description)
    if (r.body) out.set('content', r.body)
  }
  const comments = arr(get(commentsJson ? tryParse(commentsJson) : undefined, 'comments'))
  if (comments?.length) out.set('comments', comments)
  return out.size ? out : undefined
}

/** A note object as the README.md text and the _comments.json document it is. */
export function splitCardNotes(notes: J | undefined): { readme?: string; comments?: J } {
  if (!isObj(notes)) return {}
  const readme = printReadme({ description: str(notes.get('description')) ?? '', body: str(notes.get('content')) ?? '' })
  const list = arr(notes.get('comments'))
  return { readme, comments: list?.length ? obj([['comments', list]]) : undefined }
}

// ---- Param values ----------------------------------------------------------------------

const paramFields: Record<string, string> = {
  bool: 'bool', int64: 'int64', uint64: 'uint64', double: 'double', string: 'string',
  'bool[]': 'bools', 'int64[]': 'int64s', 'uint64[]': 'uint64s', 'double[]': 'doubles', 'string[]': 'strings',
}

/** The field of a ParamValue that holds a value of the param type. */
export const paramField = (type: string | undefined): string => paramFields[type ?? ''] ?? 'string'

/** A param value in its ParamValue form: {"<type field>": value}. */
export const paramValue = (type: string | undefined, v: J): J => obj([[paramField(type), v]])

/** The value of a ParamValue (`data` or a sweep entry), whatever field holds it. */
export function paramData(data: J | undefined, type?: string): J | undefined {
  if (!isObj(data)) return undefined
  if (type !== undefined) return data.get(paramField(type))
  return data.entries()[0]?.[1]
}
