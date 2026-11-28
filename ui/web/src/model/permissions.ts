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
  const name = text(op.args.name) || text(op.targets.name)
  switch (op.name) {
    case 'nix_path_lock_update':
      return [name ? L('Nix のソース {0} の固定（tsugumi.lock）を更新', name) : L('すべての Nix のソースの固定（tsugumi.lock）を更新')]
    case 'run_history_delete':
      return [L('実行記録 {0} 件と、その値・ログ・残したファイルを削除', text(op.targets.run_count) || '0')]
    case 'run_scratch_delete':
      return [L('作業フォルダ {0} 個を削除（実行記録と残した出力は残ります）', Array.isArray(op.targets.folders) ? op.targets.folders.length : 0)]
    case 'project_cache_clear':
      return [L('このプロジェクトの Nix ビルドキャッシュを削除（実行データは残ります）')]
    case 'template_add':
      return [L('フォルダ {0} をテンプレート {1} として追加', text(op.args.path), name)]
    case 'template_import':
      return [L('{0} からテンプレート {1} を取り込む', text(op.args.url), name)]
    case 'version_checkout': {
      const version = op.targets.version as Record<string, unknown> | undefined
      return [L('今の状態を記録してから、版 {0} に切り替える', text(version?.id).slice(0, 12) || text(op.args.commit))]
    }
    case 'version_init':
      return [L('版の記録を始める')]
    case 'version_mark':
      return [L('今の状態を版 {0} として残す', name)]
    default:
      return [op.name]
  }
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
