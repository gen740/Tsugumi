import { clone, jeq, J, JObj, get, obj } from '../shared/json'
import type { DefinitionDraft, ExternalProject, DocState } from './session'

export const defEq = (a: DefinitionDraft | undefined, b: DefinitionDraft | undefined): boolean =>
  a === b || (!!a && !!b && !!a.removed === !!b.removed && jeq(a.card, b.card) && jeq(a.meta, b.meta) && jeq(a.link, b.link))

export const extEq = (a: ExternalProject | undefined, b: ExternalProject | undefined): boolean => {
  if (a === b) return true
  if (!a || !b || !!a.removed !== !!b.removed || !jeq(a.project, b.project) || !jeq(a.layout, b.layout)) return false
  if (a.definitions.size !== b.definitions.size) return false
  for (const [k, v] of a.definitions) if (!defEq(v, b.definitions.get(k))) return false
  return true
}

export function cloneDef(d: DefinitionDraft): DefinitionDraft {
  return { card: clone(d.card), meta: clone(d.meta), link: clone(d.link), removed: d.removed }
}

export function cloneExternal(e: ExternalProject): ExternalProject {
  return { project: clone(e.project), layout: clone(e.layout), definitions: new Map([...e.definitions].map(([k, v]) => [k, cloneDef(v)])), removed: e.removed }
}

// Wire adapters only. Absent records are absent object members, not nulls.
export function documentStateJSON(state: DocState): J {
  const definition = (d: DefinitionDraft): J => {
    const out = obj([['card', d.card]])
    if (d.meta !== undefined) out.set('meta', d.meta)
    if (d.link !== undefined) out.set('link', d.link)
    return out
  }
  return obj([['project', state.project], ['layout', state.layout],
    ['definitions', new JObj([...state.definitions].map(([id, d]) => [id, definition(d)]))],
    ['externals', new JObj([...state.externals].map(([id, e]) => [id,
      obj([['project', e.project], ['layout', e.layout], ['definitions', new JObj([...e.definitions].map(([k, d]) => [k, definition(d)]))]])]))]])
}
export function readDocumentState(value: J): DocState {
  const definitions = (v: J | undefined) => new Map(v instanceof JObj ? [...v.entries()].map(([id, d]) => [id,
    { card: get(d, 'card') ?? obj(), meta: get(d, 'meta'), link: get(d, 'link') }]) : [])
  const externals = get(value, 'externals')
  return { project: get(value, 'project') ?? obj(), layout: get(value, 'layout') ?? obj(),
    definitions: definitions(get(value, 'definitions')),
    externals: new Map(externals instanceof JObj ? [...externals.entries()].map(([id, e]) => [id,
      { project: get(e, 'project') ?? obj(), layout: get(e, 'layout') ?? obj(), definitions: definitions(get(e, 'definitions')) }]) : []) }
}
