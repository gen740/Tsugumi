// Three-way merge of JSON documents: what `ours` changed from `base` is
// applied on top of `theirs`. Objects merge key by key; arrays of objects
// with an "id" merge element by element; other arrays merge as sets of
// elements (a wire added on each side keeps both). When both sides changed
// the same value differently, ours wins.
import { clone, get, J, jeq, JObj, str } from '../shared/json'
import type { DefinitionDraft, ExternalProject } from './session'

export function merge3(base: J | undefined, ours: J | undefined, theirs: J | undefined): J | undefined {
  if (jeq(ours, base)) return theirs
  if (jeq(theirs, base) || jeq(theirs, ours)) return ours
  if (base === undefined || ours === undefined || theirs === undefined) return ours
  if (base instanceof JObj && ours instanceof JObj && theirs instanceof JObj) {
    const out = new JObj()
    const seen = new Set<string>()
    for (const [k, tv] of theirs.entries()) {
      seen.add(k)
      const v = merge3(base.get(k), ours.get(k), tv)
      if (v !== undefined) out.set(k, v)
    }
    for (const [k, ov] of ours.entries()) {
      if (seen.has(k)) continue
      seen.add(k)
      const v = merge3(base.get(k), ov, undefined)
      if (v !== undefined) out.set(k, v)
    }
    return out
  }
  if (Array.isArray(base) && Array.isArray(ours) && Array.isArray(theirs)) {
    const ids = (a: J[]): string[] | undefined => {
      const list = a.map((x) => str(get(x, 'id'))).filter((x): x is string => x !== undefined)
      return list.length === a.length && new Set(list).size === list.length ? list : undefined
    }
    const bi = ids(base), oi = ids(ours), ti = ids(theirs)
    if (bi && oi && ti) {
      const bm = new Map(bi.map((id, i) => [id, base[i]]))
      const om = new Map(oi.map((id, i) => [id, ours[i]]))
      const out: J[] = []
      ti.forEach((id, i) => {
        const v = merge3(bm.get(id), om.get(id), theirs[i])
        if (v !== undefined) out.push(v)
      })
      oi.forEach((id, i) => {
        if (ti.includes(id)) return
        const v = merge3(bm.get(id), ours[i], undefined)
        if (v !== undefined) out.push(v)
      })
      return out
    }
    // Set semantics: their elements, without what ours removed, plus what ours added.
    const has = (a: J[], v: J) => a.some((x) => jeq(x, v))
    const removed = base.filter((v) => !has(ours, v))
    const out = theirs.filter((v) => !has(removed, v))
    for (const v of ours) if (!has(base, v) && !has(out, v)) out.push(v)
    return out
  }
  return ours
}

export const defEq = (a: DefinitionDraft | undefined, b: DefinitionDraft | undefined): boolean =>
  a === b || (!!a && !!b && !!a.removed === !!b.removed && jeq(a.card, b.card) && jeq(a.meta, b.meta) && jeq(a.link, b.link))

/** Three-way merge of one Card's definition files (undefined = no definition). */
export function mergeDefinition(base: DefinitionDraft | undefined, ours: DefinitionDraft | undefined, theirs: DefinitionDraft | undefined): DefinitionDraft | undefined {
  if (defEq(ours, base)) return theirs
  if (defEq(theirs, base) || defEq(theirs, ours)) return ours
  if (!ours) return undefined // removed here: the removal wins
  if (!theirs) return ours
  const card = merge3(base?.card, ours.card, theirs.card) ?? ours.card
  const meta = merge3(base?.meta, ours.meta, theirs.meta)
  const link = merge3(base?.link, ours.link, theirs.link)
  return { card: link === undefined ? card : new JObj(), meta, link }
}

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

export function mergeExternal(base: ExternalProject | undefined, ours: ExternalProject, theirs: ExternalProject | undefined): ExternalProject {
  if (extEq(ours, base)) return theirs ?? ours
  if (!theirs || extEq(theirs, base)) return ours
  const out = cloneExternal(theirs)
  out.project = merge3(base?.project, ours.project, theirs.project) ?? ours.project
  out.layout = merge3(base?.layout, ours.layout, theirs.layout) ?? ours.layout
  const ids = new Set([...ours.definitions.keys(), ...theirs.definitions.keys(), ...(base ? base.definitions.keys() : [])])
  out.definitions = new Map()
  for (const id of ids) {
    const d = mergeDefinition(base?.definitions.get(id), ours.definitions.get(id), theirs.definitions.get(id))
    if (d) out.definitions.set(id, d)
  }
  return out
}
