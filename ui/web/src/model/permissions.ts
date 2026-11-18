import { get, J } from '../../shared/json'
import { api } from '../api'
import { Observable } from './observable'

export type PermissionKind = string
export interface PermissionInfo { kind: PermissionKind; label: string; icon: string }

class PermissionCatalog extends Observable {
  kinds: PermissionInfo[] = []
  private loaded = false
  async load() {
    if (this.loaded) return
    try {
      const result = await api<{ kinds: { id: string; label: string; icon: string }[] }>('/api/permission-kinds')
      this.kinds = result.kinds.map(p => ({ kind: p.id, label: p.label, icon: p.icon }))
      this.loaded = true
      this.changed()
    } catch { /* Requests still show their ids and require explicit selection. */ }
  }
}
export const permissionCatalog = new PermissionCatalog()
export const permissionInfo = (kind: PermissionKind): PermissionInfo =>
  permissionCatalog.kinds.find(p => p.kind === kind) ?? { kind, label: kind, icon: 'hand.raised' }

/** Parse every kind, including future ids. Special access/operation rows have their own display. */
export function parsePermission(raw: string): { raw: string; kind: PermissionKind; card: string; folder?: string } | undefined {
  if (raw.startsWith('project_access ') || raw.includes('operation_approval\n')) return undefined
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

/** Only the properties shown as badges on a Card; this is not the permission catalog. */
export function cardPermissions(card: J | undefined): PermissionKind[] {
  const out: PermissionKind[] = []
  if (get(card, 'network') === true) out.push('network')
  if (get(card, 'sandbox') === false) out.push('sandbox_off')
  const folders = get(card, 'hostFolders')
  if (Array.isArray(folders) && folders.length) out.push(folders.some((f) => get(f as J, 'write') === true) ? 'host_folder_write' : 'host_folder_read')
  return out
}
