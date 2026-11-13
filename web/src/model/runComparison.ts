// A comparison reads retained records only; it never loads them onto the canvas.
import { D, R, RPCClient, s, segs } from '../shared/rpc'
import { describeValue } from '../shared/flatjson'
import { isObj, serialize, tryParse } from '../shared/json'
import { L } from '../i18n'
import { DisplaySnapshot, errorText } from './session'

export interface ComparedCard { summary: R.InvocationSummaryT; record?: R.RecordResultT; error?: string }
export interface ComparedRow { card: string; left?: ComparedCard; right?: ComparedCard }
/** `unknown`: the records do not hold enough to tell, so it is never shown as unchanged. */
export type Difference = 'same' | 'changed' | 'unknown'
export interface ComparedField { name: string; left?: string; right?: string; difference: Difference }
export type ComparisonRPC = Pick<RPCClient, 'latestInvocations' | 'record'>

export async function loadRunComparison(rpc: ComparisonRPC, left: string, right: string, signal?: AbortSignal): Promise<ComparedRow[]> {
  async function load(run: string) {
    const page = await rpc.latestInvocations(run)
    const entries: ComparedCard[] = page.records.map(summary => ({ summary }))
    // Bound retained image/value downloads even for a large Workflow.
    let next = 0
    await Promise.all(Array.from({ length: Math.min(4, entries.length) }, async () => {
      while (next < entries.length && !signal?.aborted) {
        const item = entries[next++]
        try { item.record = await rpc.record(run, segs(item.summary.card), item.summary.invocation, false) }
        catch (e) { item.error = errorText(e) }
      }
    }))
    return new Map(entries.map(e => [segs(e.summary.card).join('/'), e]))
  }
  const [a, b] = await Promise.all([load(left), load(right)])
  return [...new Set([...a.keys(), ...b.keys()])].sort().map(card => ({ card, left: a.get(card), right: b.get(card) }))
}

const names = (a: Iterable<string>, b: Iterable<string>) => [...new Set([...a, ...b])].sort()

/** Parameters pair by name. A side whose record holds no parameters cannot be compared. */
export function comparedParams(left?: R.RecordResultT, right?: R.RecordResultT): ComparedField[] {
  const values = (r?: R.RecordResultT) => {
    const json = tryParse(s(r?.record?.paramsJson))
    return new Map(isObj(json) ? json.entries().map(([k, v]) => [k, serialize(v).trim()]) : [])
  }
  const unrecorded = (r?: R.RecordResultT) => !!r?.record && r.record.paramsJson === null
  const a = values(left), b = values(right), unknown = unrecorded(left) || unrecorded(right)
  return names(a.keys(), b.keys()).map(name => ({
    name, left: a.get(name), right: b.get(name), difference: unknown ? 'unknown' : a.get(name) === b.get(name) ? 'same' : 'changed',
  }))
}

const sameBytes = (a: number[], b: number[]) => a.length === b.length && a.every((v, i) => v === b[i])

/** The kind and schema come first, then the digest or the returned payload.
 *  The response may omit a large payload. Its size alone cannot show equality. */
export function valueDifference(a?: D.NamedValueT, b?: D.NamedValueT): Difference {
  if (!a || !b) return a || b ? 'changed' : 'same'
  if (a.kind !== b.kind || s(a.rootType) !== s(b.rootType) || !sameBytes(a.schemaBfbs, b.schemaBfbs)) return 'changed'
  const leftHash = s(a.contentHash), rightHash = s(b.contentHash)
  if (leftHash && rightHash) return leftHash === rightHash ? 'same' : 'changed'
  if ((!a.inlineBytes.length && a.sizeBytes !== 0n) || (!b.inlineBytes.length && b.sizeBytes !== 0n)) return 'unknown'
  return sameBytes(a.inlineBytes, b.inlineBytes) ? 'same' : 'changed'
}

export function comparedValues(side: 'inputs' | 'outputs', left?: R.RecordResultT, right?: R.RecordResultT): ComparedField[] {
  const values = (r?: R.RecordResultT) => new Map((r?.record?.[side] ?? []).map(v => [s(v.name), v]))
  const text = (v?: D.NamedValueT) => v && describeValue({
    name: s(v.name), rootType: s(v.rootType), sizeBytes: v.sizeBytes, artifactId: null,
    inlineBytes: v.inlineBytes, schemaBfbs: v.schemaBfbs,
  })
  const a = values(left), b = values(right)
  return names(a.keys(), b.keys()).map(name => ({ name, left: text(a.get(name)), right: text(b.get(name)), difference: valueDifference(a.get(name), b.get(name)) }))
}

/** Where a Card's inputs came from: a Card of the same run, or a value saved by an earlier run. */
function inputSources(card: ComparedCard): Map<string, string> {
  const record = card.record?.record
  const out = new Map((record?.inputs ?? []).map(v => [s(v.name), L('保存した入力')]))
  for (const x of record?.inputSources ?? []) out.set(s(x.port), `${segs(x.card).join('/')} #${x.invocation ?? 0n}`)
  return out
}

/** What a Card ran under besides its parameters and code: the recorded Nix
 *  environment, the system it ran as, and where each input came from. */
export function comparedConditions(left?: ComparedCard, right?: ComparedCard): ComparedField[] {
  const summary = (c?: ComparedCard) => c && (c.record?.record?.summary ?? c.summary)
  const a = summary(left), b = summary(right)
  const field = (name: string, x: string | undefined, y: string | undefined, known: boolean): ComparedField =>
    ({ name, left: x, right: y, difference: !a || !b ? 'changed' : !known ? 'unknown' : x === y ? 'same' : 'changed' })
  const environment = (v?: R.InvocationSummaryT) => v && (s(v.environmentId) || L('記録なし'))
  const service = (v?: R.InvocationSummaryT) => s(v?.executionService) === 'local' ? L('プロジェクトの実行環境') : s(v?.executionService) || undefined
  const method = (v?: R.InvocationSummaryT) => {
    if (s(v?.emulator)) return L('エミュレーション: {0}', s(v?.emulator))
    const id = s(v?.executionService)
    return id === 'local' ? L('直接実行') : id ? L('外部の実行デーモン') : undefined
  }
  const fields = [
    // Recorded on one side only (a Card that stopped before its build, say): not comparable.
    field(L('環境'), environment(a), environment(b), Boolean(s(a?.environmentId)) === Boolean(s(b?.environmentId))),
    field(L('実行方式'), method(a), method(b), !!method(a) && !!method(b)),
    field(L('実行サービス'), service(a), service(b), !!service(a) && !!service(b)),
  ]
  const aSystem = s(a?.executionSystem) || undefined, bSystem = s(b?.executionSystem) || undefined
  if (aSystem || bSystem) fields.push(field(L('CPU アーキテクチャ'), aSystem, bSystem, !!aSystem && !!bSystem))
  const x = left ? inputSources(left) : new Map<string, string>(), y = right ? inputSources(right) : new Map<string, string>()
  const available = (c: ComparedCard | undefined, source: string | undefined) => !c || !!source || !!c.record?.record && c.record.record.recordIo !== false
  const other = (c?: ComparedCard) => c?.record?.record ? L('この実行の Card からは届いていません') : undefined
  for (const port of names(x.keys(), y.keys())) {
    const source = x.get(port), target = y.get(port)
    fields.push(field(L('入力元 {0}', port), source ?? other(left), target ?? other(right), available(left, source) && available(right, target)))
  }
  return fields
}

/** A run starts at one Card and either goes on downstream or runs that Card alone. */
export function runScope(run?: R.RunSummaryT): string | undefined {
  if (!run) return
  const target = segs(run.target).join('/')
  return run.propagate === false ? L('{0} だけ（保存した入力）', target) : L('{0} から下流', target)
}

const sameNumbers = (a: ArrayLike<number>, b: ArrayLike<number>) => {
  if (a.length !== b.length) return false
  // NaN marks a gap in a chart: two gaps at the same place are the same content.
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i] && !(a[i] !== a[i] && b[i] !== b[i])) return false
  return true
}
const sameStrings = (a: string[], b: string[]) => a.length === b.length && a.every((v, i) => v === b[i])

/** Compares what the two displays show, ignoring run identity and frame revision. */
export function sameDisplay(a?: DisplaySnapshot, b?: DisplaySnapshot): boolean {
  const x = a?.value, y = b?.value
  if (!x || !y) return !x && !y
  switch (x.kind) {
    case 'text': case 'markdown': return y.kind === x.kind && x.text === y.text
    case 'scalar': return y.kind === 'scalar' && sameNumbers([x.value], [y.value]) && x.label === y.label
    case 'table': return y.kind === 'table' && sameStrings(x.columns, y.columns) && x.rows.length === y.rows.length && x.rows.every((r, i) => sameStrings(r, y.rows[i]))
    case 'image': return y.kind === 'image' && x.mediaType === y.mediaType && x.alt === y.alt && sameNumbers(x.data, y.data)
    case 'module': return y.kind === 'module' && x.name === y.name && x.json === y.json && Object.keys(x.blobs).length === Object.keys(y.blobs).length && Object.keys(x.blobs).every(k => !!y.blobs[k] && sameNumbers(x.blobs[k], y.blobs[k]))
    case 'points': return y.kind === 'points' && sameNumbers(x.x, y.x) && sameNumbers(x.y, y.y) && sameNumbers(x.z, y.z) && sameNumbers(x.colors, y.colors)
  }
}
