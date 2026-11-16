import { clone, jeq, J, JObj, get, obj } from '../shared/json'
import type { DefinitionDraft, ExternalProject } from './session'

export const defEq = (a: DefinitionDraft | undefined, b: DefinitionDraft | undefined): boolean =>
  a === b || (!!a && !!b && jeq(a.card, b.card) && jeq(a.meta, b.meta) && jeq(a.link, b.link))

export function cloneDef(d: DefinitionDraft): DefinitionDraft {
  return { card: clone(d.card), meta: clone(d.meta), link: clone(d.link) }
}

export function cloneExternal(e: ExternalProject): ExternalProject {
  return { project: clone(e.project), layout: clone(e.layout), definitions: new Map([...e.definitions].map(([k, v]) => [k, cloneDef(v)])) }
}
