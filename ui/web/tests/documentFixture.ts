import { implementedCard } from '../src/model/cardFields'
import { serialize } from '../shared/json'
import { Builder } from 'flatbuffers'
import {
  BoundaryPlaceT, CardGroupT, CardPlaceT, GroupPortT, Layout, LayoutT, NotePlaceT, PositionT,
} from '../shared/gen/tsugumi/layout'
import { printReadme } from '../src/model/documents'
import { R } from '../src/wire/rpc'

export const textBytes = (text: string) => [...new TextEncoder().encode(text)]

export function layoutFixture(text: string) {
  const layout = JSON.parse(text)
  const notes = new Map<string, string>()
  const bars = (value: { inputs?: object; outputs?: object }, scope?: string) =>
    new BoundaryPlaceT(scope ?? null, value.inputs ? Object.assign(new PositionT(), value.inputs) : null,
      value.outputs ? Object.assign(new PositionT(), value.outputs) : null)
  const root = new LayoutT(layout.flow ?? null,
    Object.entries(layout.cards ?? {}).map(([key, place]) => Object.assign(new CardPlaceT(key), place)),
    Object.entries(layout.boundaries ?? {}).map(([scope, value]) => bars(value as object, scope)),
    layout.boundary ? bars(layout.boundary) : null,
    (layout.notes ?? []).map(({ text, ...place }: NotePlaceT & { text?: string }) => {
      if (typeof place.id === 'string' && text !== undefined) notes.set(place.id, text)
      return Object.assign(new NotePlaceT(), place)
    }),
    (layout.groups ?? []).map(({ outputs, ...group }: CardGroupT) =>
      Object.assign(new CardGroupT(), group, { outputs: (outputs ?? []).map(port => Object.assign(new GroupPortT(), port)) })))
  const builder = new Builder()
  Layout.finishLayoutBuffer(builder, root.pack(builder))
  return { bytes: [...builder.asUint8Array()], notes }
}

export const layoutBytes = (text: string) => layoutFixture(text).bytes

export function projectFixture(fields: object = {}): string {
  return JSON.stringify({ formatVersion: 1, name: 't', nixPath: [], trackedPaths: [], agentExclude: [],
    recordLogs: true, recordIO: false, logLimitBytes: 134217728, outputLimitBytes: 68719476736,
    outputLimitEntries: 1000, canvas: { cards: [], connections: [] }, ...fields })
}

export function cardFixture(kind: string, fields: object = {}): string {
  return JSON.stringify({ ...JSON.parse(serialize(implementedCard(kind, 'none', ['aarch64-linux']))), ...fields })
}

export function definitionFixture(id: string, cardJson: string, notesJson?: string) {
  const notes = notesJson ? JSON.parse(notesJson) : {}
  const readme = printReadme({ title: notes.title ?? id, description: notes.description ?? '', body: notes.content ?? '' })
  return new R.DefinitionT(id, cardJson, readme,
    notes.comments ? JSON.stringify({ comments: notes.comments }) : null)
}
