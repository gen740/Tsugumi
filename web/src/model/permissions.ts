// What a Card may do beyond computing (core/src/permissions.hh): reach the
// network, run without the sandbox, see folders of this host, have Nix read a
// source outside nixpkgs. The Agent's
// saves that widen these wait for the user, except for the kinds the user
// allows without asking (settings).
import { get, J } from '../shared/json'

export type PermissionKind = 'network' | 'sandbox_off' | 'host_folder_read' | 'host_folder_write' | 'nix_source'

export const PERMISSIONS: { kind: PermissionKind; label: string; icon: string }[] = [
  { kind: 'network', label: 'ネットワークへのアクセス', icon: 'network' },
  { kind: 'sandbox_off', label: '隔離（sandbox）なしで実行', icon: 'shield.slash' },
  { kind: 'host_folder_read', label: 'このマシンのフォルダを読む', icon: 'folder' },
  { kind: 'host_folder_write', label: 'このマシンのフォルダに書く', icon: 'folder.badge.plus' },
  { kind: 'nix_source', label: '外部の Nix ソースを読む', icon: 'shippingbox' },
]

export const permissionInfo = (k: PermissionKind) => PERMISSIONS.find((p) => p.kind === k)!

/** One widening a save asks for: "<kind> <card>[ <folder>]" (nix_source: "<kind> <name> <url>#<hash>"). */
export function parsePermission(raw: string): { raw: string; kind: PermissionKind; card: string; folder?: string } | undefined {
  const [kind, card, ...rest] = raw.split(' ')
  if (!card || !PERMISSIONS.some((p) => p.kind === kind)) return undefined
  return { raw, kind: kind as PermissionKind, card, folder: rest.length ? rest.join(' ') : undefined }
}

/** What a Card's settings (_card.json) give it, for its icons. */
export function cardPermissions(card: J | undefined): PermissionKind[] {
  const out: PermissionKind[] = []
  if (get(card, 'network') === true) out.push('network')
  if (get(card, 'sandbox') === false) out.push('sandbox_off')
  const folders = get(card, 'hostFolders')
  if (Array.isArray(folders) && folders.length) out.push(folders.some((f) => get(f as J, 'write') === true) ? 'host_folder_write' : 'host_folder_read')
  return out
}
