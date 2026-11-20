import { clone, jeq } from '../../shared/json'
import type { DefinitionDraft, ExternalProject } from './session'

export const defEq = (a: DefinitionDraft | undefined, b: DefinitionDraft | undefined): boolean =>
  a === b || (!!a && !!b && jeq(a.card, b.card) && jeq(a.meta, b.meta))

export function cloneDef(d: DefinitionDraft): DefinitionDraft {
  return { card: clone(d.card), meta: clone(d.meta) }
}

export function cloneExternal(e: ExternalProject): ExternalProject {
  return { project: clone(e.project), layout: clone(e.layout), definitions: new Map([...e.definitions].map(([k, v]) => [k, cloneDef(v)])) }
}
