// A core transport double backed by the production host operation executor.
import { createHash } from 'node:crypto'
import { Builder } from 'flatbuffers'
import * as H from '../src/gen/tsugumi/host'
import { D, R, cardPath } from '../src/shared/rpc'
import { arr, get, jeq, parse, serialize, str } from '../src/shared/json'
import type { ProjectSession } from '../src/model/session'
import { hostDocumentReply } from './hostDocument'

export function operationFixture<T extends ProjectSession>(s: T): T {
  let files = new Map<string, Uint8Array>()
  const text = (path: string, value: unknown) => {
    if (value === undefined) return
    // Keep the executor's exact bytes: its inverse operations select file hashes.
    // Only a fixture installing a different initial document should replace them.
    const previous = files.get(path)
    if (previous && jeq(parse(new TextDecoder().decode(previous)), value as never)) return
    files.set(path, new TextEncoder().encode(serialize(value as never)))
  }
  const definitions = (prefix: string, values: ProjectSession['definitionDrafts']) => {
    for (const [id, d] of values) {
      const dir = prefix + 'definitions/' + id + '/'
      text(dir + (d.link === undefined ? '_card.json' : '_link.json'), d.link ?? d.card)
      text(dir + '_meta.json', d.meta)
    }
  }
  s.projectId = 'fixture'
  s.connected = true
  const session = s as any
  session.client ??= { close() {}, readFile: async (path: string) => {
    const bytes = files.get(path)
    if (!bytes) throw new Error('Missing fixture file: ' + path)
    return bytes.slice()
  } }
  s.operateProject = async request => {
    // A presentation fixture may install a confirmed initial document directly.
    text('tsugumi.json', s.project); text('layout.json', s.layout)
    definitions('', s.definitionDrafts)
    for (const [prefix, ext] of s.externals) {
      text(prefix + 'tsugumi.json', ext.project); text(prefix + 'layout.json', ext.layout)
      definitions(prefix, ext.definitions)
    }
    for (const path of s.sourcePaths) if (!files.has(path)) files.set(path, new Uint8Array())
    const builder = new Builder(); builder.finish(request.pack(builder))
    const response = hostDocumentReply({ operation: Buffer.from(builder.asUint8Array()).toString('base64'),
      revision: Number(s.rev), files: Object.fromEntries([...files].map(([path, bytes]) => [path, Buffer.from(bytes).toString('base64')])) })
    const value = response.reply.value
    if (response.reply.reply === 'Failure') throw new Error(value.diagnostic)
    const before = files
    files = new Map(Object.entries(response.files as Record<string, string>).map(([path, bytes]) => [path, Buffer.from(bytes, 'base64')]))
    const decode = (path: string) => new TextDecoder().decode(files.get(path))
    const delta = Object.assign(new R.DocumentDeltaT(), { baseRev: s.rev, rev: BigInt(response.revision), opId: request.opId, origin: 'ui',
      projectJson: decode('tsugumi.json'), layoutJson: decode('layout.json') })
    const ids = new Set([...files.keys()].flatMap(path => /^definitions\/[^/]+\/_(card|link)\.json$/.test(path) ? [path.split('/')[1]] : []))
    delta.definitionsRemoved = [...s.definitionDrafts.keys()].filter(id => !ids.has(id))
    delta.definitionsUpsert = [...ids].map(id => new R.DefinitionT(id, files.has(`definitions/${id}/_card.json`) ? decode(`definitions/${id}/_card.json`) : '{}',
      files.has(`definitions/${id}/_meta.json`) ? decode(`definitions/${id}/_meta.json`) : null,
      files.has(`definitions/${id}/_link.json`) ? decode(`definitions/${id}/_link.json`) : null))
    delta.resolution = (arr(get(parse(decode('tsugumi.json')), 'canvas', 'cards')) ?? []).map(card => {
      const id = str(get(card, 'id'))!, definition = files.has(`definitions/${id}/_card.json`) ? parse(decode(`definitions/${id}/_card.json`)) : undefined
      const inputs = (arr(get(definition, 'inputs')) ?? []).map(input => Object.assign(new D.PortT(), { name: str(get(input, 'name')) }))
      return Object.assign(new R.ResolvedCardT(), { card: cardPath([id]), inputs })
    })
    delta.sourcesUpsert = [...files].filter(([path]) => path !== 'tsugumi.json' && path !== 'layout.json' && !/^definitions\/[^/]+\/_(card|link|meta)\.json$/.test(path))
      .map(([path, bytes]) => Object.assign(new R.SourceInfoT(), { path, hash: createHash('sha256').update(bytes).digest('hex'), sizeBytes: BigInt(bytes.length) }))
    delta.sourcesRemoved = [...before.keys()].filter(path => !files.has(path))
    return Object.assign(new R.SaveResultT(), { rev: delta.rev, delta, operationData: [...Buffer.from(value.operation_data.base64, 'base64')] })
  }
  return s
}
