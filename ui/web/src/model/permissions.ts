import { get, J } from '../../shared/json'
import * as H from '../../shared/gen/tsugumi/host'
import { L, language } from '../i18n'
import { hostCall } from '../wire/host'
import { s } from '../wire/rpc'
import { Observable } from './observable'

export type PermissionKind = string
export interface PermissionInfo { kind: PermissionKind; label: string; icon: string }

class PermissionCatalog extends Observable {
  kinds: PermissionInfo[] = []
  private loaded = false
  /** Reads the catalog from hostd; presentation tests replace it. */
  read = async (): Promise<PermissionInfo[]> => {
    const result = await hostCall<H.PermissionKindsT>(H.RequestPayload.PermissionKindsRequest,
      new H.PermissionKindsRequestT(language()), H.ReplyPayload.PermissionKinds)
    return result.kinds.map(p => ({ kind: s(p.id), label: s(p.label), icon: s(p.icon) }))
  }
  async load() {
    if (this.loaded) return
    try {
      this.kinds = await this.read()
      this.loaded = true
      this.changed()
    } catch { /* Requests still show their ids and require explicit selection. */ }
  }
}
export const permissionCatalog = new PermissionCatalog()
export const permissionInfo = (kind: PermissionKind): PermissionInfo =>
  permissionCatalog.kinds.find(p => p.kind === kind) ?? { kind, label: kind, icon: 'hand.raised' }

/** Parse every kind, including future ids. Operation rows have their own display. */
export function parsePermission(raw: string): { raw: string; kind: PermissionKind; card: string; folder?: string } | undefined {
  if (raw.includes('operation_approval\n')) return undefined
  const [kind, card = '', ...rest] = raw.split(' ')
  if (!kind) return undefined
  return { raw, kind, card, folder: rest.length ? rest.join(' ') : undefined }
}

/** An unknown permission needs an explicit choice even when approval uses Return. */
export function defaultPermissionSelection(asked: string[]): string[] {
  return asked.filter(raw => {
    const request = parsePermission(raw)
    return !request || permissionCatalog.kinds.some(p => p.kind === request.kind)
  })
}

/** An operation approval as hostd describes it: the tool, its targets as the
 *  project reports them, and the arguments the Agent proposed. */
export interface OperationApproval {
  name: string
  targets: Record<string, unknown>
  args: Record<string, unknown>
  /** Every line hostd sent, JSON laid out to read. */
  detail: string
}

const OPERATION = 'operation_approval\n'
const PROPOSED = 'Proposed arguments: '

export function parseOperationApproval(asked: string[]): OperationApproval | undefined {
  const raw = asked.find(r => r.startsWith(OPERATION))
  if (raw === undefined) return undefined
  const lines = raw.slice(OPERATION.length).split('\n')
  const object = (text: string | undefined): Record<string, unknown> => {
    try {
      const v = JSON.parse(text ?? '')
      return v && typeof v === 'object' && !Array.isArray(v) ? v as Record<string, unknown> : {}
    } catch { return {} }
  }
  const detail = lines.map(line => {
    const prefix = line.startsWith(PROPOSED) ? PROPOSED : ''
    try { return prefix + JSON.stringify(JSON.parse(line.slice(prefix.length)), null, 2) } catch { return line }
  }).join('\n')
  const args = lines.find(l => l.startsWith(PROPOSED))?.slice(PROPOSED.length)
  return { name: lines[0] ?? '', targets: object(lines[1]), args: object(args), detail }
}

/** What approving will do, in sentences; the detail keeps the exact request. */
export function operationSummary(op: OperationApproval): string[] {
  const text = (v: unknown) => typeof v === 'string' ? v : typeof v === 'number' ? String(v) : ''
  const permissions = Array.isArray(op.targets.permissions) ? op.targets.permissions.map(text).filter(Boolean) : undefined
  if (permissions) {
    const out: string[] = []
    const sources: string[] = []
    for (const raw of permissions) {
      const request = parsePermission(raw)
      if (!request) out.push(raw)
      else if (request.kind === 'nix_source') sources.push(request.folder ? `${request.card} (${request.folder})` : request.card)
      else out.push(`${permissionInfo(request.kind).label}: ${request.card}${request.folder ? ` (${request.folder})` : ''}`)
    }
    if (sources.length) out.unshift(L('Nix のソースを追加: {0}', sources.join(', ')))
    return out
  }
  return [op.name]
}

/** Only the properties shown as badges on a Card; this is not the permission catalog. */
export function cardPermissions(card: J | undefined): PermissionKind[] {
  const out: PermissionKind[] = []
  const grants = get(card, 'grants')
  const sandbox = get(card, 'sandbox')
  if (get(grants, 'network') === true) out.push('network')
  if (get(grants, 'gui') === true) out.push('gui')
  if (get(sandbox, 'filesystem') === false) out.push('sandbox_off')
  if (['pid', 'ipc', 'identity'].some(key => get(sandbox, key) === false)) out.push('sandbox_options')
  const folders = get(grants, 'hostPaths')
  if (Array.isArray(folders) && folders.length) out.push(folders.some((f) => get(f as J, 'write') === true) ? 'host_folder_write' : 'host_folder_read')
  return out
}
