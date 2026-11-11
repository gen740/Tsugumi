// A comparison reads retained records only; it never loads them onto the canvas.
import { D, R, RPCClient, s, segs } from '../shared/rpc'
import { describeValue } from '../shared/flatjson'
import { isObj, serialize, tryParse } from '../shared/json'
import { errorText } from './session'

export interface ComparedCard { summary: R.InvocationSummaryT; record?: R.RecordResultT; error?: string }
export interface ComparedRow { card: string; left?: ComparedCard; right?: ComparedCard }
export interface ComparedField { name: string; left?: string; right?: string; changed: boolean }
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

function paired(a: Map<string, string>, b: Map<string, string>): ComparedField[] {
  return [...new Set([...a.keys(), ...b.keys()])].sort().map(name => ({ name, left: a.get(name), right: b.get(name), changed: a.get(name) !== b.get(name) }))
}

export function comparedParams(left?: R.RecordResultT, right?: R.RecordResultT): ComparedField[] {
  const values = (r?: R.RecordResultT) => {
    const json = tryParse(s(r?.record?.paramsJson))
    return new Map(isObj(json) ? json.entries().map(([k, v]) => [k, serialize(v).trim()]) : [])
  }
  return paired(values(left), values(right))
}

export function comparedValues(side: 'inputs' | 'outputs', left?: R.RecordResultT, right?: R.RecordResultT): ComparedField[] {
  const values = (r?: R.RecordResultT) => new Map((r?.record?.[side] ?? []).map(v => [s(v.name), describeValue({
    name: s(v.name), rootType: s(v.rootType), sizeBytes: v.sizeBytes, artifactId: null,
    inlineBytes: v.inlineBytes, schemaBfbs: v.schemaBfbs,
  })]))
  const fields = paired(values(left), values(right))
  // A schema/type change is meaningful even when the displayed value is equal.
  const sameBytes = (a: number[], b: number[]) => a.length === b.length && a.every((v, i) => v === b[i])
  for (const f of fields) {
    const a = left?.record?.[side].find(v => s(v.name) === f.name), b = right?.record?.[side].find(v => s(v.name) === f.name)
    if (a && b && (!sameBytes(a.inlineBytes, b.inlineBytes) || !sameBytes(a.schemaBfbs, b.schemaBfbs) || a.kind !== b.kind)) f.changed = true
  }
  return fields
}

/** Ignore run identity and frame revision when comparing the retained content. */
export function sameDisplay(a?: D.DisplayT | null, b?: D.DisplayT | null): boolean {
  return a?.valueType === b?.valueType && JSON.stringify(a?.value) === JSON.stringify(b?.value)
}
